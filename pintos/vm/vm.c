/* vm.c: Generic interface for virtual memory objects. */
/* vm.c: 가상 메모리 객체를 위한 일반 인터페이스 */

#include "threads/malloc.h"
#include "vm/vm.h"
#include "vm/inspect.h"
#include "threads/mmu.h"
#include "vm/anon.h"
#include "vm/file.h"
#include "vm/uninit.h"
#include "filesys/file.h"
#include "threads/vaddr.h"

#define STACK_MAX_BYTES (1 << 20)  // 스택 성장 한계


#ifdef VM
/* process.c의 struct load_aux와 동일한 레이아웃 (미러 선언) */
struct load_aux {
  struct file *file;
  off_t ofs;
  size_t read_bytes;
  size_t zero_bytes;
};
#endif

static uint64_t spt_hash(const struct hash_elem *e, void *aux) {
  const struct page *p = hash_entry(e, struct page, spt_elem);
  return hash_bytes(&p->va, sizeof p->va);
}

static bool spt_less(const struct hash_elem *a, const struct hash_elem *b, void *aux) {
  const struct page *pa = hash_entry(a, struct page, spt_elem);
  const struct page *pb = hash_entry(b, struct page, spt_elem);
  return pa->va < pb->va;
}

/* UNINIT(FILE)용 aux deep-copy: 파일 핸들은 반드시 duplicate */
static void *dup_aux_for_file_uninit(const void *aux0) {
  if (aux0 == NULL) return NULL;
  const struct load_aux *src = aux0;
  struct load_aux *dst = malloc(sizeof *dst);
  if (!dst) return NULL;
  *dst = *src;
  if (dst->file) {
    dst->file = file_duplicate(dst->file);   /* 없으면 file_reopen(dst->file) 사용 */
    if (!dst->file) { free(dst); return NULL; }
  }
  return dst;
}

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

	/* 페이지 정렬 보장 */
	upage = pg_round_down(upage);
  
	/* Check wheter the upage is already occupied or not. */
	/* upage가 이미 점유되어 있는지 확인한다. */
	if (spt_find_page (spt, upage) == NULL) {
		/* TODO: Create the page, fetch the initialier according to the VM type,
		 * TODO: and then create "uninit" page struct by calling uninit_new. You
		 * TODO: should modify the field after calling the uninit_new. */
		/* TODO: 페이지를 생성하고, VM 타입에 맞는 초기화 함수를 가져온다.
		 * TODO: 그리고 uninit_new를 호출하여 "uninit" 페이지 구조체를 생성한다.
		 * TODO: uninit_new 호출 이후에 필드를 수정해야 한다. */

		/* 페이지 객체 생성 */
		struct page *page = calloc(1, sizeof *page);
		if (page == NULL)
			goto err;

		page->va = upage;

		/* 타입별 초기화 */
 	 	bool (*type_init)(struct page *, enum vm_type, void *kva) = NULL;
		switch (VM_TYPE(type)) {
			case VM_ANON: type_init = anon_initializer; break;
			case VM_FILE: type_init = file_backed_initializer; break;
			default:
				free(page);
				goto err;
		}

		/* uninit 래퍼 구성 (lazy load) */
		uninit_new(page, upage, init, type, aux, type_init);

		page->writable = writable;

		/* TODO: Insert the page into the spt. */
		/* TODO: 페이지를 보조 페이지 테이블에 삽입한다. */
		if (!spt_insert_page(spt, page)) {
			free(page);
			goto err;
		}
		return true;
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
	if (!spt) return NULL;
	struct page key;
	key.va = pg_round_down(va);
	struct hash_elem *e = hash_find(&spt->h, &key.spt_elem);
	return e ? hash_entry(e, struct page, spt_elem) : NULL;
}

/* Insert PAGE into spt with validation. */
/* PAGE를 검증한 뒤 보조 페이지 테이블에 삽입한다. */
bool
spt_insert_page (struct supplemental_page_table *spt UNUSED,
		struct page *page UNUSED) {
	/* TODO: Fill this function. */
	/* TODO: 이 함수를 구현하라. */
	page->va = pg_round_down(page->va);
	struct hash_elem *old = hash_insert(&spt->h, &page->spt_elem);
	return old == NULL;
}

void
spt_remove_page (struct supplemental_page_table *spt, struct page *page) {
	if (!page) return;
	hash_delete(&spt->h, &page->spt_elem);
	vm_dealloc_page(page);
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
	void *kva = palloc_get_page(PAL_USER);

	// oom시 아직은 패닉으로 처리 나중에 evict로 대체
	if (kva == NULL) PANIC("vm get frame 실패");

	struct frame *frame = malloc(sizeof *frame);
	/* TODO: Fill this function. */
	/* TODO: 이 함수를 구현하라. */

	ASSERT (frame != NULL);
	frame->kva = kva;
	frame->page = NULL;
	// 나중에 프레임에 추가 기능
	return frame;
}

/* Growing the stack. */
/* 스택을 확장한다. */
static void
vm_stack_growth (void *addr UNUSED) {
	void *uva = pg_round_down(addr);

	/* 한도(예: 1MiB) 초과면 아무 것도 하지 않음 */
	if ((uintptr_t)USER_STACK - (uintptr_t)uva > (uintptr_t)STACK_MAX_BYTES)
		return;

	/* 이미 등록된 페이지면 끝 */
	if (spt_find_page(&thread_current()->spt, uva))
		return;

	/* 스택 표식(MARKER_0)을 단 ANON 페이지로 alloc → claim */
	if (vm_alloc_page(VM_ANON | VM_MARKER_0, uva, /*writable=*/true)) {
		vm_claim_page(uva);
  }
}

/* Handle the fault on write_protected page */
/* 쓰기 보호된 페이지에서 발생한 페이지 폴트를 처리한다. */
static bool
vm_handle_wp (struct page *page UNUSED) {
}

/* Return true on success */
/* 성공 시 true를 반환한다. */
bool
vm_try_handle_fault (struct intr_frame *f, void *addr,
		bool user, bool write, bool not_present) {
	if (!not_present) return false;
	if (!is_user_vaddr(addr) || addr == NULL) return false;

	void *uva = pg_round_down(addr);

	struct page *page = spt_find_page(&thread_current()->spt, uva);
	if (page != NULL) {
		/* 쓰기 의도인데 read-only면 실패 */
		if (write && !page->writable)
		return false;
		/* 실제 프레임을 확보하고 매핑 */
		return vm_do_claim_page(page);
	}

	// ===== Stack growth heuristic =====
	// rsp 기준: user PF이면 f->rsp, kernel PF이면 저장된 user_rsp 사용
	uintptr_t saved_rsp = (uintptr_t)(user ? f->rsp : thread_current()->user_rsp);

	// 커널 PF면 user_rsp가 "신뢰 가능한 값"인지 먼저 확인
	if (!user) {
		// 미설정이거나 유저 영역이 아니면 스택 성장 금지
		if (saved_rsp == 0 || !is_user_vaddr((void *)saved_rsp))
		return false;
	}

	uintptr_t ubound = (uintptr_t)USER_STACK;

	// saved_rsp 자체가 스택 상한 근처(전체 허용 한도 내)인지 확인
	// (엉뚱한 값이면 휴리스틱 전체 중단)
	if (saved_rsp >= ubound) return false;
	if (ubound - saved_rsp > (uintptr_t)STACK_MAX_BYTES) return false;

	void *uva = pg_round_down(addr);

	bool below_top  = (uintptr_t)addr < ubound;
	bool within_cap = (ubound - (uintptr_t)uva) <= (uintptr_t)STACK_MAX_BYTES;

	// 언더플로우 방지한 슬랙 하한
	uintptr_t slack_low = (saved_rsp >= 32) ? (saved_rsp - 32) : 0;
	bool near_rsp = ((uintptr_t)addr >= slack_low) && ((uintptr_t)addr < ubound);

	// 쓰기 fault + rsp 근처 + 한도 이내만 허용
	if (write && below_top && within_cap && near_rsp) {
		vm_stack_growth(uva);
		return spt_find_page(&thread_current()->spt, uva) ? vm_claim_page(uva) : false;
	}

	// if (user) {
	// 	/* 현재 사용자 스택 포인터 */
	// 	void *rsp = (void *)f->rsp;

	// 	/* 스택 상한 및 푸시/콜 슬랙 판정 */
	// 	bool below_user_stack = (uintptr_t)addr < (uintptr_t)USER_STACK;
	// 	bool within_limit =
	// 		((uintptr_t)USER_STACK - (uintptr_t)uva) <= (uintptr_t)STACK_MAX_BYTES;
	// 	bool near_rsp =
	// 		(uintptr_t)addr >= ((uintptr_t)rsp - 32) &&  /* push 등 여유 허용 */
	// 		(uintptr_t)addr <  (uintptr_t)USER_STACK;

	// 	if (below_user_stack && within_limit && near_rsp) {
	// 	/* 새 anonymous 스택 페이지를 등록하고 곧바로 클레임 */
	// 		if (!vm_alloc_page(VM_ANON | VM_MARKER_0, uva, true))
	// 			return false;
	// 		return vm_claim_page(uva);
	// 	}
	// }

	/* 그 외는 처리 너가해 */
	return false;
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
	va = pg_round_down(va);
	page = spt_find_page(&thread_current()->spt, va);
	if (!page) return false;
	return vm_do_claim_page (page);
}

void vm_free_frame(struct frame *frame) {
	ASSERT(frame != NULL);
	ASSERT(frame->page == NULL);
	palloc_free_page(frame->kva);
	free(frame);
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
	struct thread *cur = thread_current();
	if (!swap_in(page, frame->kva)) {
		frame->page = NULL;
		page->frame = NULL;
		vm_free_frame(frame);
		return false;
	}

	if (!pml4_set_page(cur->pml4, page->va, frame->kva, page->writable)) {
		frame->page = NULL;
		page->frame = NULL;
		vm_free_frame(frame);
		return false;
	}
	return true;
}

/* Initialize new supplemental page table */
/* 새로운 보조 페이지 테이블을 초기화한다. */
void
supplemental_page_table_init (struct supplemental_page_table *spt UNUSED) {
	hash_init(&spt->h, spt_hash, spt_less, NULL);
}

/* Copy supplemental page table from src to dst */
/* 보조 페이지 테이블을 src에서 dst로 복사한다. */
bool
supplemental_page_table_copy (struct supplemental_page_table *dst,
                              struct supplemental_page_table *src)
{
  struct hash_iterator it;
  hash_first(&it, &src->h);

  while (hash_next(&it)) {
    struct page *sp = hash_entry(hash_cur(&it), struct page, spt_elem);
    void *va = pg_round_down(sp->va);
    bool writable = sp->writable;

    /* 현재 상태 */
    enum vm_type cur = VM_TYPE(sp->operations->type);

    if (cur == VM_UNINIT) {
      /* 초기화되면 어떤 타입이 될지 (ANON/FILE) */
		enum vm_type after = page_get_type(sp);
		vm_initializer *init = sp->uninit.init;

		void *aux_copy = NULL;
		if (VM_TYPE(after) == VM_FILE) {
			/* lazy_load_segment용 aux deep-copy (파일 핸들 duplicate) */
			/* 나중에는 file_reopen()하고 file_close()로 대체 권장 */
			aux_copy = dup_aux_for_file_uninit(sp->uninit.aux);
			if (sp->uninit.aux && aux_copy == NULL) goto fail;
		} else {
			/* 보통 UNINIT(ANON)은 aux가 없거나 의미 없음 */
			aux_copy = NULL;
		}

		if (!vm_alloc_page_with_initializer(after, va, writable, init, aux_copy)) {
			if (aux_copy) {
				struct load_aux *ca = aux_copy;
				if (ca->file) file_close(ca->file);
			}
			goto fail;
		}
		
		/* UNINIT은 여기서 끝. 자식은 첫 PF 때 로드됨. */
		continue;
    }

    /* 이미 메모리에 올라온 페이지(ANON 또는 FILE) → 자식에 ANON 생성 후 내용 복사 */
    if (!vm_alloc_page_with_initializer(VM_ANON, va, writable, NULL, NULL))
		goto fail;
	if (!vm_claim_page(va))
		goto fail;

    struct page *dp = spt_find_page(dst, va);
    if (dp == NULL || dp->frame == NULL) goto fail;

    /* 부모 페이지는 이미 메모리에 있어야 함 (swap 미구현 가정) */
    if (sp->frame == NULL) goto fail;

    memcpy(dp->frame->kva, sp->frame->kva, PGSIZE);
  }

  return true;

fail:
  /* 부분 생성된 dst 정리 */
  supplemental_page_table_kill(dst);
  return false;
}


/* 콜백 함수 */
static void page_free_action(struct hash_elem *e, void *aux) {
  struct page *p = hash_entry(e, struct page, spt_elem);
  destroy(p);
  free(p);
}

/* Free the resource hold by the supplemental page table */
/* 보조 페이지 테이블이 보유한 리소스를 해제한다. */
void
supplemental_page_table_kill(struct supplemental_page_table *spt) {
	hash_destroy(&spt->h, page_free_action);
}