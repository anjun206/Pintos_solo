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

extern struct lock filesys_lock;

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
	page->file.is_mmap = false;
	page->file.ctx = NULL;
	return true;
}

/* Swap in the page by read contents from the file. */
static bool file_backed_swap_in(struct page *page, void *kva) {
  	struct file_page *file_page UNUSED = &page->file;

	if (file_page->read_bytes == 0) {
		memset(kva, 0, PGSIZE);
		return true;
	}

  	ASSERT(file_page->file != NULL);

	lock_acquire(&filesys_lock);
	uint32_t n = file_read_at(file_page->file, kva, file_page->read_bytes, file_page->offset);
	lock_release(&filesys_lock);

	if (n != (uint32_t)file_page->read_bytes) return false;

	if (file_page->read_bytes < PGSIZE) {
		memset((uint8_t *)kva + file_page->read_bytes, 0, PGSIZE - file_page->read_bytes);
	}

	return true;
}

/* Swap out the page by writeback contents to the file. */
static bool file_backed_swap_out(struct page *page) {
	if (!page || !page->frame || !page->owner) return true;
  	struct file_page *file_page = &page->file;
	struct frame *frame = page->frame;
  	struct thread *t = page->owner;

	// clean이면 버리고 끝 (다시 파일에서 로드)
	if (!pml4_is_dirty(t->pml4, page->va)) return true;

	// mmap인 경우에만 write-back
	if (file_page->is_mmap) {
		lock_acquire(&filesys_lock);
		size_t w = file_write_at(file_page->file, frame->kva, file_page->read_bytes, file_page->offset);
		lock_release(&filesys_lock);
		if (w != file_page->read_bytes) return false;
	}
	// exec 로드 페이지는 VM_ANON이어야 하므로 여기로 오지 않게 설계
	pml4_set_dirty(t->pml4, page->va, false);
	return true;
}

/* Destory the file backed page. PAGE will be freed by the caller. */
static void file_backed_destroy(struct page *page) {
	struct file_page *file_page = &page->file;
	struct thread *t = page->owner;

	if (page->frame) {
		(void)file_backed_swap_out(page);

  		if (t && t->pml4) pml4_clear_page(t->pml4, page->va);

		struct frame *fr = page->frame;   // ★ 보관
		fr->page = NULL;
		fr->pinned = false;
		page->frame = NULL;

		vm_free_frame(fr);                // ★ 제대로 해제
	}
	// refcount로 공유 핸들 정리
	if (file_page->ctx) {
		if (file_page->ctx->refcnt > 0) file_page->ctx->refcnt--;
		if (file_page->ctx->refcnt == 0) {
			lock_acquire(&filesys_lock);
			file_close(file_page->ctx->file);
			lock_release(&filesys_lock);
			free (file_page->ctx);
		}
		file_page->ctx = NULL;
  	}
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
	lock_acquire(&filesys_lock);
	off_t file_len = file_length(file);
	lock_release(&filesys_lock);

	// 할당되야하는 페이지 수
	size_t page_count = (length + (PGSIZE - 1)) / PGSIZE;

	// [추가] 겹침 사전 검사: 대상 범위에 뭐라도 있으면 실패
	for (size_t i = 0; i < page_count; i++) {
		if (!is_user_vaddr(upage)) return NULL;
		if (spt_find_page(&cur->spt, upage) != NULL) return NULL;
		upage += PGSIZE;
	}
	upage = addr;

	// 매핑-wide 핸들 & refcnt
	struct mmap_ctx *ctx = malloc(sizeof *ctx);
	if (!ctx) return NULL;

	lock_acquire(&filesys_lock);
	ctx->file = file_reopen(file);
	lock_release(&filesys_lock);

	if (!ctx->file) { free(ctx); return NULL; }
	ctx->refcnt = 0;

	// 매핑 메타(리스트 노드)
	struct mmap_file *mm = malloc(sizeof *mm);
	if (!mm) { 
		lock_acquire(&filesys_lock);
		file_close(ctx->file);
		lock_release(&filesys_lock);

		free(ctx); return NULL; 
	}
	mm->base = base;
	mm->page_cnt = 0;
	mm->ctx = ctx;
	list_push_back(&cur->mmap_list, &mm->elem);

	// 페이지별 할당
	size_t remain = length;
	off_t ofs = offset;

	// 할당해야 하는 페이지 수 만큼 반복
	for (size_t i = 0; i < (length + PGSIZE - 1) / PGSIZE; i++) {
		struct file_page *aux = malloc(sizeof *aux);
		if (!aux) goto rollback;

		size_t step = remain < PGSIZE ? remain : PGSIZE;
		size_t file_left = ofs < file_len ? (size_t)(file_len - ofs) : 0;
		size_t rbytes = file_left < step ? file_left : step;
		size_t zbytes = PGSIZE - rbytes;

		aux->file = ctx->file;
		aux->offset = ofs;
		aux->read_bytes = rbytes;
		aux->zero_bytes = zbytes;
		aux->is_mmap = true;
		aux->ctx = ctx;

		if (!vm_alloc_page_with_initializer(VM_FILE, upage, writable, lazy_load_mmap, aux)) {
			free(aux);
			goto rollback;
		}

		ctx->refcnt++;
		mm->page_cnt++;     // ← 지금까지 몇 장 만들었는지 기록
		remain -= step;
		ofs    += rbytes;
		upage  += PGSIZE;
	}
	return base;

	rollback:
	// 정확히 mm->page_cnt 만큼만 되돌림 (base부터)
	for (size_t j = 0; j < mm->page_cnt; j++) {
		void *va = (uint8_t *)base + j * PGSIZE;
		struct page *p = spt_find_page(&cur->spt, va);
		if (p) {
			hash_delete(&cur->spt.h, &p->spt_elem);
			vm_dealloc_page(p);
		}
	}
	list_remove(&mm->elem);

	lock_acquire(&filesys_lock);
	if (ctx->file) file_close(ctx->file);
	lock_release(&filesys_lock);

	free(ctx);
	free(mm);
	return NULL;
}

static bool lazy_load_mmap(struct page *page, void *aux_) {
	ASSERT(page != NULL);
	ASSERT(page->frame != NULL);

	struct file_page *dst = &page->file;
	struct file_page *src = aux_;
	*dst = *src;
	free(src);

	void *kva = page->frame->kva;

  	/* 파일에서 필요한 만큼 읽기 */
	lock_acquire(&filesys_lock);
	int n = file_read_at(dst->file, kva, dst->read_bytes, dst->offset);
	lock_release(&filesys_lock);

	/* 남은 공간 0으로 채우기 */
	if (n != (int)dst->read_bytes) return false;
	if (dst->zero_bytes) memset((uint8_t*)kva + dst->read_bytes, 0, dst->zero_bytes);

	return true;
}

/* Do the munmap */
void do_munmap(void *addr) {
	struct thread *cur = thread_current();
	struct mmap_file *mm = NULL;

	// 해당 매핑 찾기
	for (struct list_elem *e = list_begin(&cur->mmap_list);
		e != list_end(&cur->mmap_list); e = list_next(e)) {
		struct mmap_file *x = list_entry(e, struct mmap_file, elem);
		if (x->base == addr) { mm = x; break; }
	}
	if (!mm) return;

	// 페이지 수만큼만 해제
	for (size_t i = 0; i < mm->page_cnt; i++) {
		void *va = mm->base + i * PGSIZE;
		struct page *p = spt_find_page(&cur->spt, va);
		if (!p) continue;

		hash_delete(&cur->spt.h, &p->spt_elem);
		vm_dealloc_page(p);
	}

	list_remove(&mm->elem);
	free(mm);
}
