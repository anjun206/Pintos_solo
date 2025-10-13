/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/file.h"
#include "vm/vm.h"            // vm_alloc_page_with_initializer, spt_*
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"      // pml4_*
#include "threads/malloc.h"   // malloc/free
#include "threads/synch.h"
#include "filesys/file.h"
#include "lib/kernel/list.h"  // list_elem, list_*
#include "lib/round.h"
#include "lib/string.h"       // memset

extern struct lock filesys_lock;

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);


static struct mmap_region *find_mmap_region(void *addr) {
  struct thread *t = thread_current();
  for (struct list_elem *e = list_begin(&t->mmap_list);
       e != list_end(&t->mmap_list); e = list_next(e)) {
    struct mmap_region *r = list_entry(e, struct mmap_region, elem);
    if (r->addr == addr) return r;
  }
  return NULL;
}

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
	.swap_in = file_backed_swap_in,
	.swap_out = file_backed_swap_out,
	.destroy = file_backed_destroy,
	.type = VM_FILE,
};

/* The initializer of file vm */
void
vm_file_init (void) {
	list_init(&thread_current()->mmap_list);
}

/* Initialize the file backed page */
bool
file_backed_initializer (struct page *page, enum vm_type type, void *kva) {
	/* 1) 페이지 핸들러를 파일 전용으로 바꿔준다. */
	page->operations = &file_ops;

	/* 2) do_mmap()이 vm_alloc_page_with_initializer()의 aux로 넘겨준
			struct file_page를 꺼내 page->file에 복사한다. */
	struct file_page *source = (struct file_page *)page->uninit.aux;

	if (source == NULL) {
		/* aux 없이 호출되면 초기화할 정보가 없으므로 실패 처리 */
		return false;
	}

	page->file = *source; /* file, offset, read_bytes, zero_bytes 모두 복사 */
	page->uninit.aux = NULL;
	free(source); /* aux는 더 이상 필요 없음 */

	/* 주의: dst->file 은 do_mmap() 쪽에서 이미 file_reopen()으로
		독립 핸들을 만들어 넘겨주는 것이 가장 안전(페이지별 close 가능) */

	return true;
}

/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in(struct page *page, void *kva) {
  struct file_page *fp = &page->file;

  /* 파일에서 read_bytes 만큼 로드, 나머지는 0으로 */
  if (fp->read_bytes > 0) {
    int n;
    lock_acquire(&filesys_lock);
    n = file_read_at(fp->file, kva, fp->read_bytes, fp->offset);
    lock_release(&filesys_lock);
    if (n != (int)fp->read_bytes) return false;
  }
  if (fp->zero_bytes > 0) {
    memset((uint8_t *)kva + fp->read_bytes, 0, fp->zero_bytes);
  }
  return true;
}

/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page *page) {
  struct file_page *fp = &page->file;

  /* 페이지가 더티면 파일에 유효바이트만 write-back */
  struct thread *t = thread_current();
  if (pml4_is_dirty(t->pml4, page->va)) {
    if (fp->read_bytes > 0 && page->frame && page->frame->kva) {
      int n;
      lock_acquire(&filesys_lock);
      n = file_write_at(fp->file, page->frame->kva, fp->read_bytes, fp->offset);
      lock_release(&filesys_lock);
      if (n != (int)fp->read_bytes) {
        /* 쓰기 실패해도 프레임 회수는 진행해야 하므로 false로 막지는 않음 */
        /* 필요하면 디버그 로그만 남기자. */
      }
    }
  }
  /* 실제 매핑 해제와 프레임 free는 상위(eviction) 로직에서 처리 */
  return true;
}


/* Destroy the file-backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page *page) {
  struct file_page *fp = &page->file;

  /* 파괴 시점에 매핑이 남아있고 더티면 한 번 더 방어적 write-back */
  struct thread *t = thread_current();
  if (t && pml4_get_page(t->pml4, page->va) && pml4_is_dirty(t->pml4, page->va)) {
    if (fp->read_bytes > 0 && page->frame && page->frame->kva) {
      lock_acquire(&filesys_lock);
      (void) file_write_at(fp->file, page->frame->kva, fp->read_bytes, fp->offset);
      lock_release(&filesys_lock);
    }
  }

  /* 페이지별로 file_reopen()으로 받은 독립 핸들이라면 여기서 닫아도 안전 */
  if (fp->file) {
    lock_acquire(&filesys_lock);
    file_close(fp->file);
    lock_release(&filesys_lock);
    fp->file = NULL;
  }
}

/* Do the mmap */
void *do_mmap(void *addr, size_t length, int writable,
              struct file *file, off_t offset) {
  if (addr == NULL || length == 0) return NULL;
  if (pg_ofs(addr) != 0) return NULL;           /* 페이지 정렬 필수 */
  if (file == NULL) return NULL;
  if (offset % PGSIZE != 0) return NULL;        /* 보통 오프셋도 페이지 정렬 요구 */

  struct thread *t = thread_current();

  /* 겹침 확인 */
  size_t page_cnt = DIV_ROUND_UP(length, PGSIZE);
  for (size_t i = 0; i < page_cnt; i++) {
    void *upage = addr + i * PGSIZE;
    if (spt_find_page(&t->spt, upage) != NULL) return NULL; /* 이미 매핑된 영역과 겹침 */
  }

  /* 실제 파일 길이(EOF 이후는 zero-fill) */
  int flen;
  lock_acquire(&filesys_lock);
  flen = file_length(file);
  lock_release(&filesys_lock);

  size_t remaining = length;
  off_t ofs = offset;
  void *upage = addr;

  /* 한 번에 실패 시 롤백을 위해 기록 */
  size_t i_alloc = 0;
  for (; i_alloc < page_cnt; i_alloc++) {
    size_t page_bytes = remaining >= PGSIZE ? PGSIZE : remaining;
    size_t file_left  = (ofs < flen) ? (size_t)(flen - ofs) : 0;
    size_t read_bytes = page_bytes < file_left ? page_bytes : file_left;
    size_t zero_bytes = PGSIZE - read_bytes;

    /* aux 준비: 페이지별 독립 파일 핸들을 넣는 것이 단순 */
    struct file_page *aux = malloc(sizeof *aux);
    if (!aux) goto fail;

    lock_acquire(&filesys_lock);
    struct file *f2 = file_reopen(file);
    lock_release(&filesys_lock);
    if (!f2) { free(aux); goto fail; }

    aux->file       = f2;
    aux->offset     = ofs;
    aux->read_bytes = read_bytes;
    aux->zero_bytes = zero_bytes;

    if (!vm_alloc_page_with_initializer(VM_FILE, upage, writable,
                                        file_backed_initializer, aux)) {
      /* initializer가 aux의 소유권을 가져가므로 실패 시 직접 닫고 free 필요 */
      lock_acquire(&filesys_lock);
      file_close(f2);
      lock_release(&filesys_lock);
      free(aux);
      goto fail;
    }

    remaining -= page_bytes;
    ofs       += page_bytes;
    upage     += PGSIZE;
  }

  /* mmap_region 등록 → munmap/exit에서 쓰기 */
  struct mmap_region *region = malloc(sizeof *region);
  if (!region) goto fail;   /* 단, 여기까지 페이지는 이미 등록됨 → munmap으로 회수 */
  region->addr = addr;
  region->page_cnt = page_cnt;
  list_push_back(&t->mmap_list, &region->elem);

  return addr;

fail:
  /* 부분 할당된 것들 정리 */
  do_munmap(addr);
  return NULL;
}

/* Do the munmap */
void do_munmap(void *addr) {
  struct thread *t = thread_current();
  struct mmap_region *region = find_mmap_region(addr);
  if (!region) return;    /* 규격상 잘못된 호출이면 무시하거나 실패 처리 */

  for (size_t i = 0; i < region->page_cnt; i++) {
    void *upage = addr + i * PGSIZE;
    struct page *p = spt_find_page(&t->spt, upage);
    if (!p) continue;

    /* 더티면 유효 바이트만 write-back (프레임이 있을 때만) */
    if (p->operations && p->operations->type == VM_FILE) {
      if (pml4_is_dirty(t->pml4, upage) && p->frame && p->frame->kva) {
        struct file_page *fp = &p->file;
        if (fp->read_bytes > 0) {
          lock_acquire(&filesys_lock);
          (void) file_write_at(fp->file, p->frame->kva, fp->read_bytes, fp->offset);
          lock_release(&filesys_lock);
        }
      }
    }

    /* 매핑 해제 및 페이지 제거 */
    pml4_clear_page(t->pml4, upage);
    vm_dealloc_page(p);     /* 내부에서 file_backed_destroy 호출되어 file_close */
  }

  list_remove(&region->elem);
  free(region);
}