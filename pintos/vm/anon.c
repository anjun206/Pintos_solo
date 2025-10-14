/* anon.c: Implementation of page for non-disk image (a.k.a. anonymous page). */
#include "vm/anon.h"

#include "vm/vm.h"
#include "devices/disk.h"
#include "lib/kernel/bitmap.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/mmu.h"

/* DO NOT MODIFY BELOW LINE */
static bool anon_swap_in (struct page *page, void *kva);
static bool anon_swap_out (struct page *page);
static void anon_destroy (struct page *page);

// 전역 변수
static struct bitmap *swap_table;
static struct disk *swap_disk;

// 비트 마스킹용 락
static struct lock swap_lock;

static const size_t SECTORS_PER_SLOT = PGSIZE / DISK_SECTOR_SIZE;

/* DO NOT MODIFY this struct */
static const struct page_operations anon_ops = {
	.swap_in = anon_swap_in,
	.swap_out = anon_swap_out,
	.destroy = anon_destroy,
	.type = VM_ANON,
};

/* Initialize the data for anonymous pages */
void
vm_anon_init (void) {
	/* swap_disk를 설정하세요 */
	swap_disk = disk_get(1, 1);
	if (swap_disk == NULL) return;

	disk_sector_t swap_dsize = disk_size(swap_disk);
	size_t slot_count = swap_dsize / SECTORS_PER_SLOT;

	swap_table = bitmap_create(slot_count);
	if (swap_table == NULL) return;

	bitmap_set_all(swap_table, false);
	lock_init(&swap_lock);
}

/* Initialize the file mapping */
bool
anon_initializer (struct page *page, enum vm_type type, void *kva) {
	/* Set up the handler */
	page->operations = &anon_ops;
	page->anon.slot_idx = SIZE_MAX;
	return true; 
}

/* Swap in the page by read contents from the swap disk. */
static bool
anon_swap_in (struct page *page, void *kva) {
	struct anon_page *ap = &page->anon;
	if (ap->slot_idx == SIZE_MAX) return false;  // 스왑에 없으면 실패

	disk_sector_t base = (disk_sector_t)(ap->slot_idx * SECTORS_PER_SLOT);

	for (size_t i = 0; i < SECTORS_PER_SLOT; i++) {
		void *dst = (uint8_t *)kva + i * DISK_SECTOR_SIZE;
		disk_read(swap_disk, base + (disk_sector_t)i, dst);
	}

	lock_acquire(&swap_lock);
	bitmap_reset(swap_table, ap->slot_idx);      // 슬롯 반납
	lock_release(&swap_lock);

	ap->slot_idx = SIZE_MAX;
	return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
	ASSERT(page != NULL);
	ASSERT(page->frame != NULL);

	lock_acquire(&swap_lock);
	size_t idx = bitmap_scan_and_flip(swap_table, 0, 1, false);
	lock_release(&swap_lock);
	if (idx == BITMAP_ERROR) return false;       // 스왑 공간 없음

	disk_sector_t base = (disk_sector_t)(idx * SECTORS_PER_SLOT);

	void *kva = page->frame->kva;
	for (size_t i = 0; i < SECTORS_PER_SLOT; i++) {
		const void *src = (const uint8_t *)kva + i * DISK_SECTOR_SIZE;
		disk_write(swap_disk, base + (disk_sector_t)i, src);
	}

	page->anon.slot_idx = idx;
	return true;
}


/* Destroy the anonymous page. PAGE will be freed by the caller. */
static void anon_destroy(struct page *page) {
	struct anon_page *ap = &page->anon;

	/* 1) 스왑 슬롯 해제: 프레임 유무와 무관하게, 슬롯이 있으면 해제 */
	if (ap->slot_idx != SIZE_MAX) {
		lock_acquire(&swap_lock);
		bitmap_reset(swap_table, ap->slot_idx);
		lock_release(&swap_lock);
		ap->slot_idx = SIZE_MAX;
	}

	/* 2) 프레임 반납: 매핑 해제 → 연결 해제 → 프레임 free */
	if (page->frame != NULL) {
		struct thread *cur = thread_current();
		if (pml4_get_page(cur->pml4, page->va) != NULL) {
		pml4_clear_page(cur->pml4, page->va);
		}

		page->frame->page = NULL;
		vm_free_frame(page->frame);
		page->frame = NULL;
	}
}
