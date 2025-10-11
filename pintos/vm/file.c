/* file.c: Implementation of memory backed file object (mmaped object). */

#include "vm/vm.h"
#include "vm/file.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"
#include "threads/thread.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "filesys/file.h"
#include <string.h>
#include <debug.h>

static bool file_backed_swap_in (struct page *page, void *kva);
static bool file_backed_swap_out (struct page *page);
static void file_backed_destroy (struct page *page);

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
}

/* Initialize the file backed page */
bool
file_backed_initializer(struct page *page, enum vm_type type, void *kva) {
  page->operations = &file_ops;
  struct file_page *fp = &page->file;
  fp->file       = NULL;
  fp->ofs        = 0;
  fp->read_bytes = 0;
  fp->zero_bytes = 0;
  fp->is_mmap    = false;
  fp->is_head    = false;
  fp->npages     = 0;
  (void)type; (void)kva;
  return true;
}

/* Swap in the page by read contents from the file. */
static bool
file_backed_swap_in (struct page *page, void *kva) {
  struct file_page *fp = &page->file;
  ASSERT(fp->file != NULL);
  if (fp->read_bytes > 0) {
    int n = file_read_at(fp->file, kva, (int)fp->read_bytes, fp->ofs);
    if (n != (int)fp->read_bytes) return false;
  }
  if (fp->zero_bytes > 0) {
    memset((uint8_t *)kva + fp->read_bytes, 0, fp->zero_bytes);
  }
  return true;
}


/* Swap out the page by writeback contents to the file. */
static bool
file_backed_swap_out (struct page *page) {
  struct thread *t = thread_current();
  struct file_page *fp = &page->file;

  /* 파일로부터 매핑된 페이지가 dirty이고 mmap로 생성된 경우에만 write-back */
  if (fp->is_mmap && pml4_is_dirty(t->pml4, page->va)) {
    if (fp->read_bytes > 0) {
      int n = file_write_at(fp->file, page->frame->kva, (int)fp->read_bytes, fp->ofs);
      if (n != (int)fp->read_bytes) return false;
    }
    pml4_set_dirty(t->pml4, page->va, false);
  }

  /* PTE 해제는 상위에서 처리하는 것이 일반적이지만,
     여기서도 안전하게 끊어두자. */
  pml4_clear_page(t->pml4, page->va);
  page->frame->page = NULL;
  return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void
file_backed_destroy (struct page *page) {
  struct file_page *fp = &page->file;

  /* 프레임이 있으면 필요 시(write-back) 스왑아웃 */
  if (page->frame) {
    if (fp->is_mmap) {
      (void)file_backed_swap_out(page);
    } else {
      /* exec 세그먼트는 write-back 필요 없음: 그냥 매핑만 끊음 */
      struct thread *t = thread_current();
      pml4_clear_page(t->pml4, page->va);
      page->frame->page = NULL;
    }

    /* 프레임 해제 */
    struct frame *fr = page->frame;
    page->frame = NULL;
    fr->page = NULL;
    vm_free_frame(fr);  // palloc_free_page+free 대신 이 헬퍼 사용
  }

  /* ★ 파일 닫기는 mmap만! (exec FILE 페이지는 여기서 닫지 않음) */
  if (fp->is_mmap && fp->file) {
    file_close(fp->file);
    fp->file = NULL;
  }
}

/* addr부터 length 바이트를 파일에 매핑한다.
   - addr/offset은 페이지 정렬 필수
   - 각 페이지는 개별 file_reopen()을 보유(단순화)
   - 반환: 매핑 시작 주소, 실패 시 NULL */
void *
do_mmap (void *addr, size_t length, int writable,
         struct file *file, off_t offset) {
  if (length == 0 || file == NULL) return NULL;
  if (addr == NULL || pg_ofs(addr) != 0) return NULL;
  if (offset % PGSIZE != 0) return NULL;

  struct thread *t = thread_current();
  uint8_t *upage = addr;
  size_t   remain = length;
  size_t   npages = (length + PGSIZE - 1) / PGSIZE;

  /* 미리 충돌 검사 */
  for (size_t i = 0; i < npages; i++) {
    if (spt_find_page(&t->spt, upage + i * PGSIZE) != NULL)
      return NULL;
  }

  /* 페이지 등록 */
  for (size_t i = 0; i < npages; i++) {
    size_t page_read_bytes = remain >= PGSIZE ? PGSIZE : remain;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* 파일 핸들 per-page duplicate */
    struct file *dup = file_reopen(file);
    if (dup == NULL) {
      /* 롤백 */
      do_munmap(addr);
      return NULL;
    }

    if (!vm_alloc_page_with_initializer(VM_FILE, upage, writable,
                                        /*init=*/NULL, /*aux=*/NULL)) {
      file_close(dup);
      do_munmap(addr);
      return NULL;
    }
    struct page *p = spt_find_page(&t->spt, upage);
    ASSERT(p != NULL);

    /* 파일 페이지 메타 세팅 */
    p->file.file       = dup;
    p->file.ofs        = offset + (off_t)(i * PGSIZE);
    p->file.read_bytes = page_read_bytes;
    p->file.zero_bytes = page_zero_bytes;
    p->file.is_mmap    = true;
    p->file.is_head    = (i == 0);
    p->file.npages     = npages;

    upage  += PGSIZE;
    remain  = (remain > PGSIZE) ? (remain - PGSIZE) : 0;
  }

  return addr;
}

/* do_mmap에서 반환된 시작 주소로 언매핑 */
void
do_munmap(void *addr) {
  if (addr == NULL || pg_ofs(addr) != 0) return;

  struct thread *t = thread_current();
  struct page *head = spt_find_page(&t->spt, addr);
  if (head == NULL || VM_TYPE(head->operations->type) != VM_FILE || !head->file.is_mmap)
    return;

  size_t npages = head->file.is_head ? head->file.npages : 1;

  for (size_t i = 0; i < npages; i++) {
    void *va = (uint8_t *)addr + i * PGSIZE;
    struct page *p = spt_find_page(&t->spt, va);
    if (p == NULL) break;
    if (VM_TYPE(p->operations->type) != VM_FILE || !p->file.is_mmap) break;

    /* 여기서 swap_out 호출하지 말고 바로 제거 */
    spt_remove_page(&t->spt, p);  // destroy가 write-back/프레임/파일 정리
  }
}
