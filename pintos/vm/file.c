/* file.c: Implementation of memory backed file object (mmaped object). */

#include "filesys/file.h"

#include <string.h>

#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "vm/vm.h"

static bool file_backed_swap_in(struct page *page, void *kva);
static bool file_backed_swap_out(struct page *page);
static void file_backed_destroy(struct page *page);
static bool lazy_load_mmap(struct page *page, void *aux_);

/* DO NOT MODIFY this struct */
static const struct page_operations file_ops = {
    .swap_in = file_backed_swap_in,
    .swap_out = file_backed_swap_out,
    .destroy = file_backed_destroy,
    .type = VM_FILE,
};

/* The initializer of file vm */
void vm_file_init(void) {
  /* 아직 준비할 건 없음
   * 필요 시 락/리스트 등을 여기서 초기화 */
}

/* Initialize the file backed page */
bool file_backed_initializer(struct page *page, enum vm_type type, void *kva) {
  page->operations = &file_ops;
  // exec 경로 안전을 위해 기본값 초기화만 (mmap은 lazy_load_mmap에서 채움)
  page->file.file = NULL;
  page->file.offset = 0;
  page->file.read_bytes = 0;
  page->file.zero_bytes = 0;
  return true;
}
/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in(struct page *page, void *kva) {
  struct file_page *file_page UNUSED = &page->file;
}

/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page *page) {
  struct file_page *file_page UNUSED = &page->file;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page *page) {
	struct file_page *file_page UNUSED = &page->file;
	struct thread *cur = thread_current();

	// mmap 페이지로 초기화된 경우에만 write-back
	if (page->frame && file_page->file != NULL) {
		if (pml4_is_dirty(cur->pml4, page->va)) {
			// 페이지가 수정, 기록되었는지(dirty) 확인
			// filesys_lock_acquire();
			file_write_at(file_page->file, page->frame->kva, file_page->read_bytes,
							file_page->offset);
			// 변경된 내용을 파일의 올바른 위치(offset)에 다시 쓰는 로직
			pml4_set_dirty(cur->pml4, page->va, 0);
			// filesys_lock_release();  // 락 해야하나?
		}
	}

	pml4_clear_page(cur->pml4, page->va);
	// 페이지 테이블에서 해당 가상 주소 매핑을 제거
}

/* Do the mmap */
void *do_mmap(void *addr, size_t length, int writable, struct file *file,
              off_t offset) {
	struct thread *cur = thread_current();
	void *base = addr;
	void *upage = addr;

		if (upage == NULL) return NULL;
	if (!is_user_vaddr(upage)) return NULL;
	if (pg_ofs(upage) != 0) return NULL;
	if (pg_ofs(offset) != 0) return NULL;
	if (length <= 0) return NULL;
	if (file == NULL) return NULL;


	// 파일 객체의 byte 길이
	off_t file_len = file_length(file);
	if (file_len == 0) {
		// file_close(mmap_file);
		return NULL;
	}

	// 할당되야하는 페이지 수
	size_t page_count = (length + (PGSIZE - 1)) / PGSIZE;

	// [추가] 겹침 사전 검사: 대상 범위에 뭐라도 있으면 실패
	for (size_t i = 0; i < page_count; i++) {
		if (!is_user_vaddr(upage)) return NULL;
		if (spt_find_page(&cur->spt, upage) != NULL) return NULL;
		upage += PGSIZE;
	}
	upage = addr;

	// 페이지별 할당
	size_t remain = length;
	off_t ofs = offset;

	// 할당해야 하는 페이지 수 만큼 반복
	for (int i = 0; i < page_count; i++) {
		// aux 생성
		struct file_page *aux = malloc(sizeof *aux);
		if (!aux) {
			do_munmap(upage);
			return NULL;
		};

		// [수정] 페이지 단위로 읽을 양 계산 (≤ PGSIZE)
		size_t step = remain < PGSIZE ? remain : PGSIZE;
		size_t file_left = ofs < file_len ? (size_t)(file_len - ofs) : 0;
		size_t file_read_byte = file_left < step ? file_left : step;
		size_t file_zero_byte = PGSIZE - file_read_byte;

		// aux 초기화
		// aux->file = mmap_file;
		aux->file = file_reopen(file);
		if (aux->file == NULL) {
			free(aux);
			do_munmap(upage);
			return NULL;
		}
		aux->offset = ofs;
		aux->read_bytes = file_read_byte;
		aux->zero_bytes = file_zero_byte;

		// 페이지 할당
		if (!vm_alloc_page_with_initializer(VM_FILE, upage, writable,
											lazy_load_mmap, aux)) {
			file_close(aux->file);
			free(aux);
			do_munmap(upage);
			// file_close(mmap_file);
			return NULL;
		}

		// 다음 페이지로
		remain -= step;         // 매핑 길이는 PGSIZE 기준으로 소모
		ofs += file_read_byte;  // 파일 오프셋은 실제 읽은 만큼만 전진
		upage += PGSIZE;
	}

	return base;
}

static bool lazy_load_mmap(struct page *page, void *aux_) {
  ASSERT(page != NULL);
  ASSERT(page->frame != NULL);

  // struct file_page *file_page = &page->file;
  // void *kva = page->frame->kva;
  struct file_page *file_page = &page->file;
  struct file_page *init = aux_;
  *file_page = *init;
  free(init);
  void *kva = page->frame->kva;

  /* 파일에서 필요한 만큼 읽기 */
  if (file_page->read_bytes > 0) {
    int n = file_read_at(file_page->file, kva, file_page->read_bytes,
                         file_page->offset);
    if (n != (int)file_page->read_bytes) {
      return false;
    }
  }

  /* 남은 공간 0으로 채우기 */
  if (file_page->zero_bytes > 0) {
    memset((uint8_t *)kva + file_page->read_bytes, 0, file_page->zero_bytes);
  }

  return true;
}

/* Do the munmap */
void do_munmap(void *addr) {
  while (1) {
    struct thread *cur = thread_current();
    struct page *page = spt_find_page(&cur->spt, addr);

    if (page == NULL) return;

    // struct load_aux *aux = (struct load_aux *)page->uninit.aux;
    // page->file.aux = aux;

    hash_delete(&cur->spt.h, &page->spt_elem);
    file_backed_destroy(page);
    vm_dealloc_page(page);

    addr += PGSIZE;
  }
}
