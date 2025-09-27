#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include "threads/malloc.h"
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "intrinsic.h"
#include "threads/palloc.h"  // palloc_get_page/free
#include "threads/vaddr.h"   // PGSIZE
#include "lib/kernel/hash.h"
#ifdef VM
#include "vm/vm.h"
#endif

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

static void fd_table_init(struct thread *current);
static bool duplicate_pte (uint64_t *pte, void *va, void *aux);

/* 부모가 만든 자식 상태 노드와 커맨드라인을 자식에게 건네기 위한 구조체 */
struct exec_info {
	char *cmdline;                 /* palloc_get_page()로 복사한 커맨드라인 */
	struct child_status *cs;       /* 부모가 만들어 children에 넣어둔 노드 */
};

struct fork_args {
	struct thread *parent;
	struct intr_frame parent_if;  // 사본으로 전달
	struct child_status *cs;
};

/* 제대로된 dup2 */
struct dupmap_ent {
	struct file *parent_fp;         // 키
	struct file *child_fp;          // 값
	struct hash_elem elem;
};

/* vm용 구조체 */
#ifdef VM
struct load_aux {
	struct file *file;
	off_t ofs;
	size_t read_bytes;
	size_t zero_bytes;
};
#endif


/* General process initializer for initd and other process. */
/* initd 및 기타 프로세스를 위한 일반 초기화 함수. */
static void
process_init (void) {
	struct thread *current = thread_current ();
	if (!current->proc_inited) {
		list_init(&current->children);
		current->proc_inited = true;
	}
}

/* 해시 */
static unsigned dupmap_hash (const struct hash_elem *e, void *aux) {
	const struct dupmap_ent *x = hash_entry(e, struct dupmap_ent, elem);
	return hash_bytes(&x->parent_fp, sizeof x->parent_fp);
}
static bool dupmap_less (const struct hash_elem *a, const struct hash_elem *b, void *aux) {
	const struct dupmap_ent *xa = hash_entry(a, struct dupmap_ent, elem);
	const struct dupmap_ent *xb = hash_entry(b, struct dupmap_ent, elem);
	return (uintptr_t)xa->parent_fp < (uintptr_t)xb->parent_fp;
}
static struct dupmap_ent *dupmap_find (struct hash *m, struct file *parent_fp) {
	struct dupmap_ent key;
	key.parent_fp = parent_fp;
	struct hash_elem *e = hash_find(m, &key.elem);
	return e ? hash_entry(e, struct dupmap_ent, elem) : NULL;
}
static void dupmap_free_action(struct hash_elem *e, void *aux) {
	struct dupmap_ent *ent = hash_entry(e, struct dupmap_ent, elem);
	free(ent);
}

/* Starts the first userland program, called "initd", loaded from FILE_NAME.
 * The new thread may be scheduled (and may even exit)
 * before process_create_initd() returns. Returns the initd's
 * thread id, or TID_ERROR if the thread cannot be created.
 * Notice that THIS SHOULD BE CALLED ONCE. */
/* FILE_NAME에서 사용자 영역의 첫 프로그램 "initd"를 시작한다.
 * process_create_initd()가 반환되기 전에 새 스레드가 스케줄되거나
 * 심지어 종료될 수도 있다. 성공 시 initd의 스레드 id를, 생성 실패 시
 * TID_ERROR를 반환한다.
 * 주의: 이 함수는 한 번만 호출되어야 한다. */
tid_t
process_create_initd (const char *file_name) {
	process_init();

	char *fn_copy;
	tid_t tid;

	/* Make a copy of FILE_NAME.
	 * Otherwise there's a race between the caller and load(). */
	/* FILE_NAME의 복사본을 만든다.
	 * 그렇지 않으면 호출자와 load() 사이에서 경쟁 상태가 생길 수 있다. */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);
	
	// 첫 토큰만 잘라서 스레드 이름으로 사용
	char tname[16];
	size_t i = 0;
	while (i < sizeof tname - 1 && file_name[i] != '\0' && file_name[i] != ' ')
		tname[i++] = file_name[i];
	tname[i] = '\0';

	struct child_status *cs = malloc(sizeof *cs);
	if (!cs) { palloc_free_page(fn_copy);
		return TID_ERROR; }

	cs->tid = TID_ERROR;
	cs->exit_code = -1;
	cs->exited = false;
	cs->waited = false;
	cs->ref_cnt = 2;
	sema_init(&cs->sema, 0);
	sema_init(&cs->load_sema, 0);
	cs->load_done = false;
	cs->load_ok = false;
	list_push_back(&thread_current()->children, &cs->elem);

	struct exec_info *ei = malloc(sizeof *ei);
	if (!ei) { 
		list_remove(&cs->elem); free(cs); palloc_free_page(fn_copy);
		return TID_ERROR; }
	ei->cmdline = fn_copy;
	ei->cs = cs;

	/* Create a new thread to execute FILE_NAME. */
	/* FILE_NAME을 실행할 새 스레드를 생성한다. */
	tid = thread_create (tname, PRI_DEFAULT, initd, ei);
	if (tid == TID_ERROR) {
		list_remove(&cs->elem);
		free(cs);
		palloc_free_page (fn_copy);
		free(ei);
		return TID_ERROR;
	}
	cs->tid = tid;
	return tid;
}

/* A thread function that launches first user process. */
/* 첫 사용자 프로세스를 시작하는 스레드 함수. */
static void
initd (void *aux_) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();

	struct exec_info *ei = aux_;
	struct thread *cur = thread_current();

	if (cur->fd_table == NULL || cur->fd_cap == 0) {
    cur->fd_table = (struct file **)palloc_get_page(PAL_ZERO);
    if (cur->fd_table == NULL)
      PANIC("fd_table alloc failed");
    cur->fd_cap = PGSIZE / (int)sizeof(cur->fd_table[0]);
    cur->fd_table_from_palloc = true;

    cur->fd_table[0] = (struct file *)-1; /* stdin */
    cur->fd_table[1] = (struct file *)-2; /* stdout */
  }

	cur->my_status = ei->cs;

	char *cmd = ei->cmdline;
	free(ei);

	if (process_exec (cmd) < 0) {
		// 로드 실패 통지
		if (cur->my_status && !cur->my_status->load_done) {
			cur->my_status->load_ok = false;
			cur->my_status->load_done = true;
			sema_up(&cur->my_status->load_sema);
		}
		/* exec 실패: 부모에게 -1로 종료 신호 주고 종료 */
		cur->exit_status = -1;
		thread_exit();	
	}
	NOT_REACHED ();
}

/* Clones the current process as `name`. Returns the new process's thread id, or
 * TID_ERROR if the thread cannot be created. */
/* 현재 프로세스를 `name`으로 복제한다. 새 프로세스의 스레드 id를 반환하거나,
 * 생성 실패 시 TID_ERROR를 반환한다. */
tid_t
process_fork (const char *name, struct intr_frame *if_ UNUSED) {
	process_init();

	struct thread *parent = thread_current();

	// 1) child_status 노드 생성 + 부모 children에 등록
	struct child_status *cs = malloc(sizeof *cs);
	if (!cs) return TID_ERROR;
	cs->tid = TID_ERROR;
	cs->exit_code = -1;
	cs->exited = false;
	cs->waited = false;
	cs->ref_cnt = 2;                 // parent + child
	sema_init(&cs->sema, 0);
	sema_init(&cs->load_sema, 0);    // fork에서는 안 쓰지만 구조체 일관성
	cs->load_done = true;            // fork 경로는 사용 안 함
	cs->load_ok = true;
	list_push_back(&parent->children, &cs->elem);

	/* Clone current thread to new thread.*/
	/* 현재 스레드를 새 스레드로 복제. */
	struct fork_args *fa = malloc(sizeof *fa);
	if (!fa) { list_remove(&cs->elem); free(cs); return TID_ERROR; }
	fa->parent = parent;
	memcpy(&fa->parent_if, if_, sizeof *if_);
	fa->cs = cs;

	tid_t tid = thread_create (name, PRI_DEFAULT, __do_fork, fa);
	if (tid == TID_ERROR) {
		list_remove(&cs->elem);
		free(cs);
		free(fa);
		return TID_ERROR;
	}
	cs->tid = tid;                   // 이제 TID 기록

	sema_down(&cs->load_sema);
	if (!cs->load_ok) {
		list_remove(&cs->elem);
		if (--cs->ref_cnt == 0) free(cs);
		return TID_ERROR;
	}
	return tid;
}

#ifndef VM
/* Duplicate the parent's address space by passing this function to the
 * pml4_for_each. This is only for the project 2. */
/* 부모의 주소 공간을 복제하기 위해 이 함수를 pml4_for_each에 전달한다.
 * 이 코드는 프로젝트 2에서만 사용된다. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent =  aux;

	/* 1. TODO: If the parent_page is kernel page, then return immediately. */
	/* 1. TODO: parent_page가 커널 페이지라면 즉시 반환한다. */
	if (is_kernel_vaddr(va)) return true;

	/* 2. Resolve VA from the parent's page map level 4. */
	/* 2. 부모의 pml4에서 VA에 해당하는 물리 페이지를 얻는다. */
	void *parent_page = pml4_get_page (parent->pml4, va);
	if (parent_page == NULL) return false;

	/* 3. TODO: Allocate new PAL_USER page for the child and set result to
	 *    TODO: NEWPAGE. */
	/* 3. TODO: 자식용 PAL_USER 페이지를 새로 할당하고 결과를 NEWPAGE에 저장한다. */
	void *newpage = palloc_get_page(PAL_USER);
	if (newpage == NULL) return false;

	/* 4. TODO: Duplicate parent's page to the new page and
	 *    TODO: check whether parent's page is writable or not (set WRITABLE
	 *    TODO: according to the result). */
	/* 4. TODO: 부모 페이지의 내용을 새 페이지로 복제하고,
	 *    TODO: 부모 페이지가 쓰기 가능인지 확인하여 WRITABLE을 설정한다. */
	memcpy(newpage, parent_page, PGSIZE);
	bool writable = (*pte & PTE_W) != 0; 

	/* 5. Add new page to child's page table at a37-52
	ddress VA with WRITABLE
	 *    permission. */
	/* 5. WRITABLE 권한으로 VA 주소에 새 페이지를 자식의 페이지 테이블에 매핑한다. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: if fail to insert page, do error handling. */
		/* 6. TODO: 매핑에 실패하면 에러 처리를 수행한다. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* A thread function that copies parent's execution context.
 * Hint) parent->tf does not hold the userland context of the process.
 *       That is, you are required to pass second argument of process_fork to
 *       this function. */
/* 부모의 실행 컨텍스트를 복사하는 스레드 함수.
 * 힌트) parent->tf에는 프로세스의 사용자 영역 컨텍스트가 들어있지 않다.
 *       즉, process_fork의 두 번째 인자를 이 함수로 전달해야 한다. */
static void
__do_fork (void *aux) {
	struct fork_args *fa = aux;
	struct intr_frame if_;
	struct thread *parent = fa->parent;
	struct child_status *cs = fa->cs;
	struct thread *current = thread_current ();
	/* TODO: somehow pass the parent_if. (i.e. process_fork()'s if_) */
	/* TODO: parent_if를 전달하는 방법을 구현한다(예: process_fork()의 if_). */

	/* 1. Read the cpu context to local stack. */
	/* 1. CPU 컨텍스트를 로컬 스택으로 복사한다. */
	memcpy (&if_, &fa->parent_if, sizeof if_);
	free(fa);

	current->my_status = cs;
	process_init();

	/* 2. Duplicate PT */
	/* 2. 페이지 테이블 복제 */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: Your code goes here.
	 * TODO: Hint) To duplicate the file object, use `file_duplicate`
	 * TODO:       in include/filesys/file.h. Note that parent should not return
	 * TODO:       from the fork() until this function successfully duplicates
	 * TODO:       the resources of parent.*/
	/* TODO: 여기에 코드를 작성한다.
	 * TODO: 힌트) 파일 객체를 복제하려면 include/filesys/file.h의 `file_duplicate`를 사용하라.
	 * TODO:       이 함수가 부모의 자원을 성공적으로 복제하기 전까지
	 * TODO:       부모는 fork()에서 반환되어서는 안 된다. */
	struct hash dupmap;
	if (parent->fd_table && parent->fd_cap > 0) {
		current->fd_cap = parent->fd_cap;

		current->fd_table = palloc_get_page(PAL_ZERO);
		if (!current->fd_table) goto error;
		current->fd_table_from_palloc = true;
	
		hash_init(&dupmap, dupmap_hash, dupmap_less, NULL);	

		for (int i = 0; i < parent->fd_cap; i++) {
			struct file *p = parent->fd_table[i];
  	      	if (!p) continue;
			
			if (p == (struct file*)-1 || p == (struct file*)-2) {
				current->fd_table[i] = p;
				continue;
			}

			struct dupmap_ent *ent = dupmap_find(&dupmap, p);
			struct file *nf;
			/* 기존 존재 유무 */
			if (ent) {
				nf = ent->child_fp;   // 같은 child_fp 재사용 (오프셋 공유)
				if (!fdref_inc(nf)) goto fork_rollback;
				current->fd_table[i] = nf;

			/* 없으면 새로 만든다 */
			} else {
				nf = file_duplicate(p);
				if (!nf) goto fork_rollback;
				if (!fdref_inc(nf)) {  // 자식 내 참조 1 등록
					lock_acquire(&filesys_lock);
					file_close(nf);
					lock_release(&filesys_lock);
					goto fork_rollback;
				}
				current->fd_table[i] = nf;

				// 매핑 등록
				ent = malloc(sizeof *ent);
				if (!ent) goto fork_rollback;
				ent->parent_fp = p;
				ent->child_fp  = nf;
				hash_insert(&dupmap, &ent->elem);
			}
		}
	}

	// 성공 경로 — dupmap 엔트리 free
	hash_destroy(&dupmap, dupmap_free_action);

	cs->load_ok = true;
	cs->load_done = true;
	sema_up(&cs->load_sema);

	if_.R.rax = 0;

	/* Finally, switch to the newly created process. */
	/* 마지막으로 새로 생성한 프로세스로 전환한다. */
	do_iret (&if_);
fork_rollback:
	for (int j = 0; j < parent->fd_cap; j++) {
		struct file *q = current->fd_table ? current->fd_table[j] : NULL;
		if (!q || q == (struct file*)-1 || q == (struct file*)-2) continue;
		current->fd_table[j] = NULL;
		fdref_dec(q);                                // ref테이블에 올라간 것만 dec
	}
	if (current->fd_table) {
	  palloc_free_page(current->fd_table);
	  current->fd_table = NULL;
	  current->fd_cap = 0;
	  current->fd_table_from_palloc = false;
	}
	hash_destroy(&dupmap, dupmap_free_action);
	goto error;
error:
	cs->load_ok = false;
	cs->load_done = true;
	sema_up(&cs->load_sema);
	current->exit_status = -1;
  	thread_exit();
}

/* Switch the current execution context to the f_name.
 * Returns -1 on fail. */
/* 현재 실행 컨텍스트를 f_name으로 전환한다.
 * 실패 시 -1을 반환. */
int
process_exec (void *f_name) {
  	char *cmdline = f_name; 
	struct thread *t = thread_current();

	/* We cannot use the intr_frame in the thread structure.
	 * This is because when current thread rescheduled,
	 * it stores the execution information to the member. */
	/* 스레드 구조체 안의 intr_frame은 사용할 수 없다.
	 * 현재 스레드가 리스케줄될 때 실행 정보가 그 멤버에 저장되기 때문이다. */

	/* 기존 exec_file이 있다면 정리 (exec 체인 대비) */
	if (t->exec_file) {
		file_allow_write(t->exec_file);
		file_close(t->exec_file);
		t->exec_file = NULL;
	}

	 /* 유저모드용 세그먼트 셀렉터/플래그 셋업 */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	process_cleanup();

	/* 커맨드라인 토큰화: prog + argv[] */
	char *save_ptr;
	char *argv_k[64];           // 64개 제한
	int   argc = 0;

	char *prog = strtok_r(cmdline, " ", &save_ptr);
	if (prog == NULL) { palloc_free_page(cmdline); return -1; }
	argv_k[argc++] = prog;

	for (char *p = strtok_r(NULL, " ", &save_ptr);
		p != NULL && argc < 63;
		p = strtok_r(NULL, " ", &save_ptr)) {
		argv_k[argc++] = p;
	}
	argv_k[argc] = NULL;



	/* ELF 로드 (프로그램명만) */
	if (!load(prog, &_if)) {
		palloc_free_page(cmdline);
		if (t->my_status && !t->my_status->load_done) {
			t->my_status->load_ok = false;
			t->my_status->load_done = true;
			sema_up(&t->my_status->load_sema);
		}
		return -1;
	}

	uintptr_t top = (uintptr_t)_if.rsp;

	size_t str_bytes = 0;
	for (int i = 0; i < argc; ++i) str_bytes += strlen(argv_k[i]) + 1;

	/* 오반지 아닌 검사 */
	size_t pushes = 8ull * (argc + 4);                   // argv[]+(NULL) + argv + argc + fake-ret
	size_t pad    = ((top - str_bytes - pushes) & 0xfull); // 0..15, 최종 RSP 16B 정렬 보정

	if (str_bytes + pad + pushes > PGSIZE) {
		palloc_free_page(cmdline);
		return -1;
	}


	/* 스택에 인자 빌드*/
	uint64_t rsp = top;

	/* 문자열들을 역순으로 스택에 복사하고, 그 유저주소를 기록 */
	uint64_t argv_u[64];
	for (int i = argc - 1; i >= 0; --i) {
		size_t len = strlen(argv_k[i]) + 1;   // '\0' 포함
		rsp -= len;
		memcpy((void *)rsp, argv_k[i], len);
		argv_u[i] = rsp;
	}

	rsp -= pad;
	memset((void *)rsp, 0, pad);

	/* argv 테이블(포인터 배열)과 NULL 센티널 */
	rsp -= 8ull * (argc + 1);
	uint64_t *argv_area = (uint64_t *)rsp;
	for (int i = 0; i < argc; ++i) argv_area[i] = argv_u[i];
	argv_area[argc] = 0;


	/* 스택에 (char **argv) 푸시 */
	rsp -= sizeof(uint64_t);
	*(uint64_t *)rsp = (uint64_t)argv_area;
	
	uint64_t argv_base = (uint64_t)argv_area;

	/* 스택에 (int argc) 푸시 */
	rsp -= sizeof(uint64_t);
	*(uint64_t *)rsp = (uint64_t)argc;

	/* 가짜 반환 주소(0) 푸시 */
	rsp -= sizeof(uint64_t);
	*(uint64_t *)rsp = 0;

	/* 레지스터: argc/argv/rsp 세팅 */
	_if.R.rdi = argc;            // 첫 인자
	_if.R.rsi = argv_base;       // 둘째 인자
	_if.rsp   = rsp;

	/* If load failed, quit. */
	/* 적재에 실패하면 종료한다. */
	palloc_free_page (cmdline);

	// /* fd 테이블 초기화 */
	// fd_table_init(thread_current());
	
	/* 로드 성공 통지 */
	if (t->my_status && !t->my_status->load_done) {
		t->my_status->load_ok = true;
		t->my_status->load_done = true;
		sema_up(&t->my_status->load_sema);
	}

	/* Start switched process. */
	/* 전환된 프로세스를 시작한다. */
	do_iret (&_if);
	NOT_REACHED ();
}

static void fd_table_init(struct thread *t) {
  if (t->fd_table) return;           // 중복 방지
  t->fd_cap   = 64;
  t->fd_table = calloc(t->fd_cap, sizeof *t->fd_table);
  /* 0(stdin),1(stdout)은 예약
   * 테이블은 2부터 사용
   */
}


/* Waits for thread TID to die and returns its exit status.  If
 * it was terminated by the kernel (i.e. killed due to an
 * exception), returns -1.  If TID is invalid or if it was not a
 * child of the calling process, or if process_wait() has already
 * been successfully called for the given TID, returns -1
 * immediately, without waiting.
 *
 * This function will be implemented in problem 2-2.  For now, it
 * does nothing. */
/* 스레드 TID가 종료될 때까지 기다리고 그 종료 상태를 반환한다.
 * 커널에 의해 종료되었다면(예: 예외로 인해 kill) -1을 반환한다.
 * TID가 유효하지 않거나 호출 프로세스의 자식이 아니거나,
 * 혹은 해당 TID에 대해 이미 process_wait()가 성공적으로 호출된 적이 있다면
 * 기다리지 않고 즉시 -1을 반환한다.
 *
 * 이 함수는 문제 2-2에서 구현된다. 현재는 아무 동작도 하지 않는다. */
int
process_wait (tid_t child_tid UNUSED) {
	/* XXX: Hint) The pintos exit if process_wait (initd), we recommend you
	 * XXX:       to add infinite loop here before
	 * XXX:       implementing the process_wait. */
	/* XXX: 힌트) process_wait(initd)를 호출하면 pintos가 종료된다.
	 * XXX:       process_wait를 구현하기 전에는 이곳에 무한 루프를 넣는 것을 권장한다. */
	struct thread *cur = thread_current();
	struct list_elem *e;

	for (e = list_begin(&cur->children); e != list_end(&cur->children); e = list_next(e)) {
		struct child_status *cs = list_entry(e, struct child_status, elem);
		if (cs->tid == child_tid) {
			if (cs->waited) return -1;   // 이걸로 중복 wait 막음
			cs->waited = true;

			if (!cs->exited)
				sema_down(&cs->sema);

			int ex_code = cs->exit_code;
			
			list_remove(&cs->elem);

			if (--cs->ref_cnt == 0)
				free(cs);
			
			return ex_code;
		}
	}
	return -1; // 내 자식 아님 또는 존재안함
}

/* Exit the process. This function is called by thread_exit (). */
/* 프로세스를 종료한다. 이 함수는 thread_exit()에 의해 호출된다. */
void
process_exit (void) {
	/* TODO: Your code goes here.
	 * TODO: Implement process termination message (see
	 * TODO: project2/process_termination.html).
	 * TODO: We recommend you to implement process resource cleanup here. */
	/* TODO: 여기에 코드를 작성한다.
	 * TODO: 프로세스 종료 메시지를 구현하라
	 * TODO: (project2/process_termination.html 참고).
	 * TODO: 프로세스 자원 해제를 여기에서 구현하는 것을 권장한다. */
	struct thread *cur = thread_current ();


		if (cur->fd_table) {
			for (int i = 0; i < cur->fd_cap; i++) {
				struct file *p = cur->fd_table ? cur->fd_table[i] : NULL;
				if (!p) continue;
				cur->fd_table[i] = NULL;
				fdref_dec(p);
			}
		}

		if (cur->exec_file) {
			file_allow_write(cur->exec_file);
			file_close(cur->exec_file);
			cur->exec_file = NULL;
		}

		if (cur->fd_table) {
			if (cur->fd_table_from_palloc) palloc_free_page(cur->fd_table);
			else free(cur->fd_table);
			cur->fd_table = NULL;
			cur->fd_cap = 0;
			cur->fd_table_from_palloc = false;
		}

	/* 유저 프로세스 에서만 */
	if (cur->proc_inited) {
		/* 스펙 요구 종료 메시지 */
		printf("%s: exit(%d)\n", cur->name, cur->exit_status);


		if (cur->my_status) {
			cur->my_status->exit_code = cur->exit_status;
			cur->my_status->exited = true;
			sema_up(&cur->my_status->sema);

			if (--cur->my_status->ref_cnt == 0)
				free(cur->my_status);
			cur->my_status = NULL;
		}

		/* 부모가 wait 안하고 죽는 불상사 방지용 */
		/* 자식들의 부모 소유 해제 */
		while (!list_empty(&cur->children)) {
			struct list_elem *e = list_pop_front(&cur->children);	
			struct child_status *cs = list_entry(e, struct child_status, elem);
			if (--cs->ref_cnt == 0)
				free(cs);
		}
	}
	process_cleanup ();
}

/* Free the current process's resources. */
/* 현재 프로세스의 자원을 해제한다. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* Destroy the current process's page directory and switch back
	 * to the kernel-only page directory. */
	/* 현재 프로세스의 페이지 디렉터리를 파괴하고
	 * 커널 전용 페이지 디렉터리로 되돌린다. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* Correct ordering here is crucial.  We must set
		 * cur->pagedir to NULL before switching page directories,
		 * so that a timer interrupt can't switch back to the
		 * process page directory.  We must activate the base page
		 * directory before destroying the process's page
		 * directory, or our active page directory will be one
		 * that's been freed (and cleared). */
		/* 올바른 순서가 매우 중요하다. 페이지 디렉터리를 전환하기 전에
		 * cur->pagedir를 NULL로 설정해야 타이머 인터럽트가
		 * 프로세스의 페이지 디렉터리로 다시 전환하지 못한다.
		 * 또한 프로세스의 페이지 디렉터리를 파괴하기 전에
		 * 기본 페이지 디렉터리를 활성화해야 한다. 그렇지 않으면
		 * 이미 해제(초기화)된 디렉터리를 활성 상태로 두게 된다. */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* Sets up the CPU for running user code in the nest thread.
 * This function is called on every context switch. */
/* 다음(next) 스레드에서 사용자 코드를 실행할 수 있도록 CPU를 설정한다.
 * 이 함수는 매 컨텍스트 스위치마다 호출된다. */
void
process_activate (struct thread *next) {
	/* Activate thread's page tables. */
	/* 스레드의 페이지 테이블을 활성화한다. */
	pml4_activate (next->pml4);

	/* Set thread's kernel stack for use in processing interrupts. */
	/* 인터럽트 처리 시 사용할 스레드의 커널 스택을 설정한다. */
	tss_update (next);
}

/* We load ELF binaries.  The following definitions are taken
 * from the ELF specification, [ELF1], more-or-less verbatim.  */
/* ELF 실행 파일을 적재한다. 아래 정의들은 [ELF1] 명세에서 거의 그대로 가져왔다. */

/* ELF types.  See [ELF1] 1-2. */
/* ELF 타입들. [ELF1] 1-2 참고. */
#define EI_NIDENT 16

#define PT_NULL    0            /* Ignore. */
/* 무시. */
#define PT_LOAD    1            /* Loadable segment. */
/* 적재 가능한 세그먼트. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
/* 동적 링킹 정보. */
#define PT_INTERP  3            /* Name of dynamic loader. */
/* 동적 로더의 이름. */
#define PT_NOTE    4            /* Auxiliary info. */
/* 보조 정보. */
#define PT_SHLIB   5            /* Reserved. */
/* 예약됨. */
#define PT_PHDR    6            /* Program header table. */
/* 프로그램 헤더 테이블. */
#define PT_STACK   0x6474e551   /* Stack segment. */
/* 스택 세그먼트. */

#define PF_X 1          /* Executable. */
/* 실행 가능. */
#define PF_W 2          /* Writable. */
/* 쓰기 가능. */
#define PF_R 4          /* Readable. */
/* 읽기 가능. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
 * This appears at the very beginning of an ELF binary. */
/* 실행 파일 헤더. [ELF1] 1-4 ~ 1-8 참고.
 * ELF 바이너리의 맨 앞에 위치한다. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* Abbreviations */
/* 약어(별칭) 정의 */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
 * Stores the executable's entry point into *RIP
 * and its initial stack pointer into *RSP.
 * Returns true if successful, false otherwise. */
/* FILE_NAME에서 ELF 실행 파일을 현재 스레드로 적재한다.
 * 실행 파일의 엔트리 포인트를 *RIP에,
 * 초기 스택 포인터를 *RSP에 저장한다.
 * 성공 시 true, 실패 시 false를 반환한다. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* Allocate and activate page directory. */
	/* 페이지 디렉터리를 할당하고 활성화한다. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());

	/* Open executable file. */
	/* 실행 파일을 연다. */
	file = filesys_open (file_name);
	if (file == NULL) {
		printf ("load: %s: open failed\n", file_name);
		goto done;
	}

	/* Read and verify executable header. */
	/* 실행 파일 헤더를 읽고 검증한다. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			/* amd64 아키텍처 */
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", file_name);
		goto done;
	}

	/* Read program headers. */
	/* 프로그램 헤더들을 읽는다. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* Ignore this segment. */
				/* 이 세그먼트는 무시. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* Normal segment.
						 * Read initial part from disk and zero the rest. */
						/* 일반 세그먼트.
						 * 앞부분은 디스크에서 읽고, 나머지는 0으로 채운다. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* Entirely zero.
						 * Don't read anything from disk. */
						/* 전체가 0인 세그먼트.
						 * 디스크에서 읽지 않는다. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* Set up stack. */
	/* 스택을 설정한다. */
	if (!setup_stack (if_))
		goto done;

	/* Start address. */
	/* 시작 주소 설정. */
	if_->rip = ehdr.e_entry;

	/* TODO: Your code goes here.
	 * TODO: Implement argument passing (see project2/argument_passing.html). */
	/* TODO: 여기에 코드를 작성한다.
	 * TODO: 인자 전달을 구현하라 (project2/argument_passing.html 참고). */
	t->exec_file = file;
	file_deny_write(file);
	file = NULL;

	success = true;

done:
	/* We arrive here whether the load is successful or not. */
	/* 성공 여부와 관계없이 이 지점으로 온다. */
	if (file) file_close (file);
	return success;
}


/* Checks whether PHDR describes a valid, loadable segment in
 * FILE and returns true if so, false otherwise. */
/* PHDR가 FILE 안의 유효하고 적재 가능한 세그먼트를 기술하는지 검사한다.
 * 그렇다면 true, 아니면 false를 반환. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset and p_vaddr must have the same page offset. */
	/* p_offset과 p_vaddr는 동일한 페이지 오프셋을 가져야 한다. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset must point within FILE. */
	/* p_offset은 FILE의 범위 안을 가리켜야 한다. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz must be at least as big as p_filesz. */
	/* p_memsz는 p_filesz보다 크거나 같아야 한다. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* The segment must not be empty. */
	/* 세그먼트는 비어 있으면 안 된다. */
	if (phdr->p_memsz == 0)
		return false;

		
	/* The region cannot "wrap around" across the kernel virtual
	   address space. */
	/* 영역이 커널 가상 주소 공간을 가로질러 '랩 어라운드' 되어서는 안 된다. */
	uint64_t start = phdr->p_vaddr;
	uint64_t end = phdr->p_vaddr + phdr->p_memsz;
	if (end < start) return false;

	/* The virtual memory region must both start and end within the
	   user address space range. */
	/* 가상 메모리 영역의 시작과 끝이 모두 사용자 주소 공간 범위 안이어야 한다. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* Disallow mapping page 0.
	   Not only is it a bad idea to map page 0, but if we allowed
	   it then user code that passed a null pointer to system calls
	   could quite likely panic the kernel by way of null pointer
	   assertions in memcpy(), etc. */
	/* 페이지 0 매핑은 허용하지 않는다.
	   페이지 0을 매핑하는 것은 위험할 뿐 아니라, 허용할 경우
	   사용자 코드가 null 포인터를 시스템 콜에 넘겼을 때 memcpy() 등의
	   null 포인터 단언으로 커널 패닉이 날 가능성이 매우 높다. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	if (start < PGSIZE) return false;

	/* It's okay. */
	/* 유효한 세그먼트이다. */
	return true;
}

#ifndef VM
/* Codes of this block will be ONLY USED DURING project 2.
 * If you want to implement the function for whole project 2, implement it
 * outside of #ifndef macro. */
/* 이 블록의 코드는 프로젝트 2에서만 사용된다.
 * 프로젝트 2 전체에서 사용할 구현은 #ifndef 블록 밖에 작성하라. */

/* load() helpers. */
/* load() 보조 함수들. */
static bool install_page (void *upage, void *kpage, bool writable);

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
/* FILE의 OFS에서 시작하는 세그먼트를 주소 UPAGE에 적재한다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리를 다음과 같이 초기화한다:
 *
 * - UPAGE에서 READ_BYTES 바이트를 FILE의 OFS에서 읽어 채운다.
 *
 * - UPAGE + READ_BYTES에서 ZERO_BYTES 바이트를 0으로 채운다.
 *
 * WRITABLE이 true면 사용자 프로세스가 해당 페이지를 쓸 수 있어야 하며,
 * 그렇지 않으면 읽기 전용이어야 한다.
 *
 * 성공 시 true를, 메모리 할당 오류나 디스크 읽기 오류 시 false를 반환한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		/* 이 페이지를 어떻게 채울지 계산한다.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고,
		 * 남은 PAGE_ZERO_BYTES 바이트는 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* Get a page of memory. */
		/* 메모리 페이지를 하나 할당한다. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* Load this page. */
		/* 이 페이지에 내용을 적재한다. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* Add the page to the process's address space. */
		/* 프로세스의 주소 공간에 페이지를 매핑한다. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* Advance. */
		/* 다음 페이지로 진행. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* Create a minimal stack by mapping a zeroed page at the USER_STACK */
/* USER_STACK에 0으로 초기화된 페이지를 매핑하여 최소한의 스택을 만든다. */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
 * virtual address KPAGE to the page table.
 * If WRITABLE is true, the user process may modify the page;
 * otherwise, it is read-only.
 * UPAGE must not already be mapped.
 * KPAGE should probably be a page obtained from the user pool
 * with palloc_get_page().
 * Returns true on success, false if UPAGE is already mapped or
 * if memory allocation fails. */
/* 사용자 가상 주소 UPAGE를 커널 가상 주소 KPAGE에 매핑한다.
 * WRITABLE이 true면 사용자 프로세스가 해당 페이지를 수정할 수 있고,
 * 그렇지 않으면 읽기 전용이다.
 * UPAGE는 이미 매핑되어 있으면 안 된다.
 * KPAGE는 palloc_get_page()로 사용자 풀에서 얻은 페이지여야 한다.
 * 성공 시 true, 이미 매핑되어 있거나 메모리 할당 실패 시 false를 반환한다. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* Verify that there's not already a page at that virtual
	 * address, then map our page there. */
	/* 해당 가상 주소에 기존 페이지가 없는지 확인한 후,
	 * 우리 페이지를 그곳에 매핑한다. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* From here, codes will be used after project 3.
 * If you want to implement the function for only project 2, implement it on the
 * upper block. */
/* 여기부터의 코드는 프로젝트 3 이후에 사용된다.
 * 프로젝트 2에서만 사용할 구현은 위쪽 블록에 작성하라. */

static bool
lazy_load_segment (struct page *page, void *aux_) {
	/* TODO: Load the segment from the file */
	/* TODO: 파일에서 세그먼트를 적재한다. */

	/* TODO: This called when the first page fault occurs on address VA. */
	/* TODO: 이 함수는 VA에서 최초의 페이지 폴트가 발생했을 때 호출된다. */

	/* TODO: VA is available when calling this function. */
	/* TODO: 이 함수를 호출할 때 VA는 유효하다. */

	struct load_aux *aux = aux_;
	void *kva = page->frame->kva;

	/* 파일에서 필요한 만큼 읽기 */
	if (aux->read_bytes > 0) {
		int n = file_read_at(aux->file, kva, aux->read_bytes, aux->ofs);
		if (n != (int)aux->read_bytes) {
			free(aux);
			return false;
		}
  	}

	/* 남은 공간 0으로 채우기 */
  	if (aux->zero_bytes > 0) {
    	memset((uint8_t *)kva + aux->read_bytes, 0, aux->zero_bytes);
  	}
	
	free(aux);
	return true;
}

/* Loads a segment starting at offset OFS in FILE at address
 * UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
 * memory are initialized, as follows:
 *
 * - READ_BYTES bytes at UPAGE must be read from FILE
 * starting at offset OFS.
 *
 * - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.
 *
 * The pages initialized by this function must be writable by the
 * user process if WRITABLE is true, read-only otherwise.
 *
 * Return true if successful, false if a memory allocation error
 * or disk read error occurs. */
/* FILE의 OFS에서 시작하는 세그먼트를 주소 UPAGE에 적재한다.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리를 다음과 같이 초기화한다:
 *
 * - UPAGE에서 READ_BYTES 바이트를 FILE의 OFS에서 읽어 채운다.
 *
 * - UPAGE + READ_BYTES에서 ZERO_BYTES 바이트를 0으로 채운다.
 *
 * WRITABLE이 true이면 사용자 프로세스가 해당 페이지를 쓸 수 있어야 하며,
 * 아니면 읽기 전용이어야 한다.
 *
 * 성공 시 true, 메모리 할당 오류나 디스크 읽기 오류 시 false를 반환한다. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* Do calculate how to fill this page.
		 * We will read PAGE_READ_BYTES bytes from FILE
		 * and zero the final PAGE_ZERO_BYTES bytes. */
		/* 이 페이지를 어떻게 채울지 계산한다.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고,
		 * 나머지 PAGE_ZERO_BYTES 바이트는 0으로 채운다. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: Set up aux to pass information to the lazy_load_segment. */
		/* TODO: lazy_load_segment에 정보를 전달하기 위한 aux를 설정한다. */
		struct load_aux *aux = malloc(sizeof *aux);
		if (!aux) return false;
		aux->file = file;
		aux->ofs  = ofs;
		aux->read_bytes = page_read_bytes;
		aux->zero_bytes = page_zero_bytes;

		/* 파일-백드 페이지로 lazy 등록 */
		if (!vm_alloc_page_with_initializer(VM_FILE, upage, writable,
											lazy_load_segment, aux)) {
			free(aux);
			return false;
		}

		/* Advance. */
		/* 다음 페이지로 진행. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage 	   += PGSIZE;
		ofs   	   += page_read_bytes;
	}
	return true;
}

/* Create a PAGE of stack at the USER_STACK. Return true on success. */
/* USER_STACK에 스택 페이지를 생성한다. 성공 시 true를 반환. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: Map the stack on stack_bottom and claim the page immediately.
	 * TODO: If success, set the rsp accordingly.
	 * TODO: You should mark the page is stack. */
	/* TODO: stack_bottom에 스택을 매핑하고 즉시 페이지를 클레임한다.
	 * TODO: 성공했다면 rsp를 적절히 설정한다.
	 * TODO: 해당 페이지가 스택임을 표시해야 한다. */

	/* 스택 페이지 VM에 등록 */
	if (!vm_alloc_page_with_initializer(VM_ANON | VM_MARKER_0,
										stack_bottom, true,
									 	NULL, NULL))
		return false;
	
	/* 즉시 클레임 */
	if (!vm_claim_page(stack_bottom))
		return false;

	if_->rsp = USER_STACK;
  	success = true;
	return success;
}
#endif /* VM */
