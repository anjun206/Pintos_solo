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

  /* 아직 스왑 안 간 페이지라면 0으로 초기화 (스택/새 anon 페이지 기본값) */
  if (ap->slot_idx == SIZE_MAX) {
    memset(kva, 0, PGSIZE);
    return true;
  }

  /* 스왑 슬롯에서 복원 (나중에 eviction 구현 시 사용) */
  lock_acquire(&swap_lock);
  disk_sector_t base = (disk_sector_t)(ap->slot_idx * SECTORS_PER_SLOT);
  for (size_t i = 0; i < SECTORS_PER_SLOT; i++) {
    disk_read(swap_disk, base + i, (uint8_t *)kva + i * DISK_SECTOR_SIZE);
  }
  bitmap_reset(swap_table, ap->slot_idx);   /* 슬롯 반환 */
  lock_release(&swap_lock);
  ap->slot_idx = SIZE_MAX;
  return true;
}

/* Swap out the page by writing contents to the swap disk. */
static bool
anon_swap_out (struct page *page) {
  /* 아직 eviction 안 쓰면 더미로 true만 줘도 무방. 
     아래는 나중을 위한 실제 구현(선택) */
  if (!page->frame || !page->frame->kva) return true;

  lock_acquire(&swap_lock);
  size_t idx = bitmap_scan_and_flip(swap_table, 0, 1, false);
  lock_release(&swap_lock);
  if (idx == BITMAP_ERROR) return false;

  disk_sector_t base = (disk_sector_t)(idx * SECTORS_PER_SLOT);
  for (size_t i = 0; i < SECTORS_PER_SLOT; i++) {
    disk_write(swap_disk, base + i, (uint8_t *)page->frame->kva + i * DISK_SECTOR_SIZE);
  }

  page->anon.slot_idx = idx;

  /* 매핑 해제 및 프레임 반환 */
  struct thread *cur = thread_current();
  if (pml4_get_page(cur->pml4, page->va))
    pml4_clear_page(cur->pml4, page->va);
  page->frame->page = NULL;
  vm_free_frame(page->frame);
  page->frame = NULL;

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
