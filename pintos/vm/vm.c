/* vm.c: Generic interface for virtual memory objects. */
/* vm.c: 가상 메모리 객체를 위한 일반 인터페이스 */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"

/* Initializes the virtual memory subsystem by invoking each subsystem's
 * intialize codes. */
/* 가상 메모리 하위 시스템을 초기화한다.
 * 각 하위 시스템의 초기화 코드를 호출한다. */
void
vm_init (void) {
	vm_anon_init ();
	vm_file_init ();
#ifdef EFILESYS  /* For project 4 */
	pagecache_init ();
#endif
	register_inspect_intr ();
	/* DO NOT MODIFY UPPER LINES. */
	/* 위의 줄은 수정하지 말 것. */
	/* TODO: Your code goes here. */
	/* TODO: 여기에 코드를 작성하라. */
}

/* Get the type of the page. This function is useful if you want to know the
 * type of the page after it will be initialized.
 * This function is fully implemented now. */
/* 페이지의 타입을 가져온다. 
 * 이 함수는 페이지가 초기화된 후 그 타입을 알고 싶을 때 유용하다.
 * 이 함수는 이미 완전히 구현되어 있다. */
enum vm_type
page_get_type (struct page *page) {
	int ty = VM_TYPE (page->operations->type);
	switch (ty) {
		case VM_UNINIT:
			return VM_TYPE (page->uninit.type);
		default:
			return ty;
	}
}

/* Helpers */
/* 헬퍼 함수들 */
static struct frame *vm_get_victim (void);
static bool vm_do_claim_page (struct page *page);
static struct frame *vm_evict_frame (void);

/* Create the pending page object with initializer. If you want to create a
 * page, do not create it directly and make it through this function or
 * `vm_alloc_page`. */
/* 초기화 함수를 사용하여 대기(pending) 상태의 페이지 객체를 생성한다.
 * 페이지를 직접 생성하지 말고, 반드시 이 함수 또는 `vm_alloc_page`를 통해 생성해야 한다. */
bool
vm_alloc_page_with_initializer (enum vm_type type, void *upage, bool writable,
		vm_initializer *init, void *aux) {

	ASSERT (VM_TYPE(type) != VM_UNINIT)

	struct supplemental_page_table *spt = &thread_current ()->spt;

	/* Check wheter the upage is already occupied or not. */
	/* upage가 이미 점유되어 있는지 확인한다. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		/* TODO: 페이지를 생성하고, VM 타입에 맞는 초기화 함수를 가져온다.
		 * TODO: 그리고 uninit_new를 호출하여 "uninit" 페이지 구조체를 생성한다.
		 * TODO: uninit_new 호출 이후에 필드를 수정해야 한다. */

		/* TODO: Insert the page into the spt. */
		/* TODO: 페이지를 보조 페이지 테이블에 삽입한다. */
	}
err:
	return false;
}

/* Find VA from spt and return page. On error, return NULL. */
/* 보조 페이지 테이블에서 VA(가상 주소)를 찾아 페이지를 반환한다.
 * 실패 시 NULL을 반환한다. */
struct page *
spt_find_page (struct supplemental_page_table *spt UNUSED, void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function. */
	/* TODO: 이 함수를 구현하라. */

	return page;
}

/* Insert PAGE into spt with validation. */
/* PAGE를 검증한 뒤 보조 페이지 테이블에 삽입한다. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	int succ = false;
	/* TODO: Fill this function. */
	/* TODO: 이 함수를 구현하라. */

	return succ;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	vm_dealloc_page (page);
	return true;
}

/* Get the struct frame, that will be evicted. */
/* 제거(evict)될 프레임을 가져온다. */
static struct frame *
vm_get_victim (void) {
	struct frame *victim = NULL;
	 /* TODO: The policy for eviction is up to you. */
	 /* TODO: 어떤 프레임을 제거할지 결정하는 정책은 직접 구현한다. */

	return victim;
}

/* Evict one page and return the corresponding frame.
 * Return NULL on error.*/
/* 페이지 하나를 제거하고 해당 프레임을 반환한다.
 * 오류 시 NULL을 반환한다. */
static struct frame *
vm_evict_frame (void) {
	struct frame *victim UNUSED = vm_get_victim ();
	/* TODO: swap out the victim and return the evicted frame. */
	/* TODO: victim을 스왑 아웃하고 제거된 프레임을 반환한다. */

	return NULL;
}

/* palloc() and get frame. If there is no available page, evict the page
 * and return it. This always return valid address. That is, if the user pool
 * memory is full, this function evicts the frame to get the available memory
 * space.*/
/* palloc()을 호출하여 프레임을 가져온다. 
 * 사용 가능한 페이지가 없으면 페이지를 제거(evict)하고 반환한다.
 * 항상 유효한 주소를 반환한다.
 * 즉, 사용자 풀 메모리가 가득 찼을 경우 이 함수는 프레임을 제거해 가용 메모리를 확보한다. */
static struct frame *
vm_get_frame (void) {
	struct frame *frame = NULL;
	/* TODO: Fill this function. */
	/* TODO: 이 함수를 구현하라. */

	ASSERT (frame != NULL);
	ASSERT (frame->page == NULL);
	return frame;
}

/* Growing the stack. */
/* 스택을 확장한다. */
static void
vm_stack_growth (void *addr UNUSED) {
}

/* Handle the fault on write_protected page */
/* 쓰기 보호된 페이지에서 발생한 페이지 폴트를 처리한다. */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
/* 성공 시 true를 반환한다. */
bool
vm_try_handle_fault (struct intr_frame *f UNUSED, void *addr UNUSED,
		bool user UNUSED, bool write UNUSED, bool not_present UNUSED) {
	struct supplemental_page_table *spt UNUSED = &thread_current ()->spt;
	struct page *page = NULL;
	/* TODO: Validate the fault */
	/* TODO: 페이지 폴트를 검증한다. */
	/* TODO: Your code goes here */
	/* TODO: 여기에 코드를 작성하라. */

	return vm_do_claim_page (page);
}

/* Free the page.
 * DO NOT MODIFY THIS FUNCTION. */
/* 페이지를 해제한다.
 * 이 함수는 수정하지 말 것. */
void
vm_dealloc_page (struct page *page) {
	destroy (page);
	free (page);
}

/* Claim the page that allocate on VA. */
/* VA에 할당된 페이지를 확보(claim)한다. */
bool
vm_claim_page (void *va UNUSED) {
	struct page *page = NULL;
	/* TODO: Fill this function */
	/* TODO: 이 함수를 구현하라. */

	return vm_do_claim_page (page);
}

/* Claim the PAGE and set up the mmu. */
/* PAGE를 확보(claim)하고 MMU를 설정한다. */
static bool
vm_do_claim_page (struct page *page) {
	struct frame *frame = vm_get_frame ();

	/* Set links */
	/* 페이지와 프레임을 연결한다. */
	frame->page = page;
	page->frame = frame;

	/* TODO: Insert page table entry to map page's VA to frame's PA. */
	/* TODO: 페이지의 VA를 프레임의 PA에 매핑하는 페이지 테이블 엔트리를 삽입한다. */

	return swap_in (page, frame->kva);
}

/* Initialize new supplemental page table */
/* 새로운 보조 페이지 테이블을 초기화한다. */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
}

/* Copy supplemental page table from src to dst */
/* 보조 페이지 테이블을 src에서 dst로 복사한다. */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst UNUSED,
		struct supplemental_page_table *src UNUSED) {
}

/* Free the resource hold by the supplemental page table */
/* 보조 페이지 테이블이 보유한 리소스를 해제한다. */
void
supplemental_page_table_kill (struct supplemental_page_table *spt UNUSED) {
	/* TODO: Destroy all the supplemental_page_table hold by thread and
	 * TODO: writeback all the modified contents to the storage. */
	/* TODO: 쓰레드가 보유한 모든 보조 페이지 테이블을 파괴하고,
	 * TODO: 수정된 내용을 저장소에 기록(writeback)한다. */
}
