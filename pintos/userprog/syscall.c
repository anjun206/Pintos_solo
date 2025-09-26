#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "filesys/file.h"
#include "threads/init.h"
#include "filesys/filesys.h"
#include "filesys/directory.h"
#include "threads/vaddr.h"   // is_user_vaddr()
#include "threads/mmu.h"     // pml4_get_page()
#include "devices/input.h"   // input_getc()
#include "threads/palloc.h"  // palloc_get_page, palloc_free_page
#include "userprog/process.h"
#include "threads/synch.h"
#include "list.h"
#include "lib/kernel/hash.h"

struct lock filesys_lock;

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

/* f->R. 뭐시기보다 이게 더 있어보임 ㄹㅇ */
#define SC_NO(f)   ((f)->R.rax)
#define ARG0(f)    ((f)->R.rdi)
#define ARG1(f)    ((f)->R.rsi)
#define ARG2(f)    ((f)->R.rdx)
#define ARG3(f)    ((f)->R.r10)   /* 4th is r10 */
#define ARG4(f)    ((f)->R.r8)
#define ARG5(f)    ((f)->R.r9)
#define RETVAL(f)  ((f)->R.rax)
#define RET(f, v)  ( (f)->R.rax = (uint64_t)(v) )

#define FIRST_FD 2
#define FD_GROW_STEP 32 

#define STDIN_FD  ((struct file*)-1)
#define STDOUT_FD ((struct file*)-2)

/* 프로토 타입 */
static void system_halt(void) NO_RETURN;

static tid_t system_fork(const char *thread_name, struct intr_frame *parent_if);

static int system_exec(const char *cmdline);

static int  system_wait(tid_t pid);

static bool system_create(const char *file, unsigned initial_size);
static bool system_remove(const char *file);
static int  system_open(const char *file);
static void system_close(int fd);

static int  system_filesize(int fd);

static int  system_read(int fd, void *buffer, unsigned size);
static long  system_write(int fd, const void *buffer, unsigned size);

static void system_seek(int fd, unsigned position);
static unsigned system_tell(int fd);

static int system_dup2(int oldfd, int newfd);

/* 시스템콜 헬퍼 */
static struct file *fd_get(int fd);
static void assert_user_range(const void *uaddr, size_t size);
static bool copy_in_string(char *kdst, const char *usrc, size_t max_len);
static void copy_in(void *kdst, const void *usrc, size_t n);
static void copy_out(void *udst, const void *ksrc, size_t n);
static int  fd_alloc(struct file *f);   // 빈 슬롯 찾아 file* 넣고 fd 반환

static unsigned file_ref_hash(const struct hash_elem *e, void *aux);
static bool file_ref_less(const struct hash_elem *a, const struct hash_elem *b, void *aux);
static struct file_ref *ref_find(struct file *fp);

// dup2
struct file_ref {
  struct file *fp;
  int refcnt;
  // struct list_elem elem;
  struct hash_elem elem;
};

// static struct list file_ref_list;
static struct lock file_ref_lock;
static struct hash file_ref_ht;
/* System call.
 *
 * Previously system call services was handled by the interrupt handler
 * (e.g. int 0x80 in linux). However, in x86-64, the manufacturer supplies
 * efficient path for requesting the system call, the `syscall` instruction.
 *
 * The syscall instruction works by reading the values from the the Model
 * Specific Register (MSR). For the details, see the manual. */
/* 시스템 콜.
 *
 * 과거에는 시스템 콜 서비스가 인터럽트 핸들러(예: 리눅스의 int 0x80)에 의해
 * 처리되었다. 하지만 x86-64에서는 제조사가 `syscall` 명령을 통해 시스템 콜을
 * 요청하는 더 효율적인 경로를 제공한다.
 *
 * `syscall` 명령은 모델별 레지스터(MSR)의 값을 읽어 동작한다.
 * 자세한 내용은 매뉴얼을 참고하라. */

#define MSR_STAR 0xc0000081         /* Segment selector msr */
/* 세그먼트 셀렉터 MSR */
#define MSR_LSTAR 0xc0000082        /* Long mode SYSCALL target */
/* 롱 모드 SYSCALL 진입 지점 주소 MSR */
#define MSR_SYSCALL_MASK 0xc0000084 /* Mask for the eflags */
/* EFLAGS 마스크를 설정하는 MSR */

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* The interrupt service rountine should not serve any interrupts
	 * until the syscall_entry swaps the userland stack to the kernel
	 * mode stack. Therefore, we masked the FLAG_FL. */
	/* syscall_entry가 유저랜드 스택을 커널 모드 스택으로 교체하기 전까지는
	 * 인터럽트 서비스 루틴이 어떤 인터럽트도 처리하면 안 된다.
	 * 따라서 EFLAGS의 해당 비트들을 마스킹(비활성화)했다. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);

  lock_init(&filesys_lock);
  // list_init(&file_ref_list);
  lock_init(&file_ref_lock);
  hash_init(&file_ref_ht, file_ref_hash, file_ref_less, NULL);
}

/* The main system call interface */
/* 주요 시스템 콜 인터페이스 */
/* 개쩌는 가시성 (아님) */
void syscall_handler(struct intr_frame *f) {
  switch (SC_NO(f)) {
    case SYS_HALT:   system_halt(); __builtin_unreachable();
    case SYS_EXIT:   system_exit((int)ARG0(f)); __builtin_unreachable();

    case SYS_FORK:   RET(f, system_fork((const char *)ARG0(f), f)); break;
    case SYS_EXEC:   RET(f, system_exec((const char *)ARG0(f))); /* 성공 시 복귀 안함 */ break;

    case SYS_WAIT:   RET(f, system_wait((tid_t)ARG0(f))); break;
 
    case SYS_CREATE: RET(f, system_create((const char *)ARG0(f), (unsigned)ARG1(f))); break;
    case SYS_REMOVE: RET(f, system_remove((const char *)ARG0(f))); break;

    case SYS_OPEN:   RET(f, system_open((const char *)ARG0(f))); break;
    case SYS_CLOSE:  system_close((int)ARG0(f)); break;

    case SYS_FILESIZE: RET(f, system_filesize((int)ARG0(f))); break;

    case SYS_READ:   RET(f, system_read((int)ARG0(f), (void *)ARG1(f), (unsigned)ARG2(f))); break;
    case SYS_WRITE:  RET(f, system_write((int)ARG0(f), (const void *)ARG1(f), (unsigned)ARG2(f))); break;
  
	  case SYS_SEEK:   system_seek((int)ARG0(f), (unsigned)ARG1(f)); break;
    case SYS_TELL:   RET(f, system_tell((int)ARG0(f))); break;

    /* dup2 extra 과제 */
    case SYS_DUP2:   RET(f, system_dup2((int)ARG0(f), (int)ARG1(f))); break;

    default:         system_exit(-1); __builtin_unreachable();
  }
}


static void
system_halt(void) { power_off(); __builtin_unreachable(); }

void
system_exit (int status) {
	struct thread *cur = thread_current();
	cur->exit_status = status;
	thread_exit();
	__builtin_unreachable();
}

static unsigned
system_tell(int fd) {
  struct file *f = fd_get(fd);
  if (f == NULL) return -1;
  if (f == STDOUT_FD || f == STDIN_FD) return -1;
  
  lock_acquire(&filesys_lock);
  off_t pose = file_tell(f);
  lock_release(&filesys_lock);
  return pose;
}

static tid_t
system_fork(const char *thread_name, struct intr_frame *parent_if) {
  char name[NAME_MAX + 1];
  if (!copy_in_string(name, thread_name, sizeof name))
    return TID_ERROR;
  return process_fork(name, parent_if);
}

static int
system_exec(const char *cmdline) {
  char *create = palloc_get_page(0);
  if (!create) return -1;
  if (!copy_in_string(create, cmdline, PGSIZE)) {
    palloc_free_page(create);
    system_exit(-1);
  }

  int r = process_exec(create);

  (void)r;
  system_exit(-1);
  __builtin_unreachable();
}


static int
system_wait(tid_t pid) {
  return process_wait(pid);
}

static bool
system_create(const char *file, unsigned initial_size) {
  char kname[NAME_MAX + 1];

  bool ok = copy_in_string(kname, file, sizeof kname);
  if (!ok) {
    return false;
  }
  
  lock_acquire(&filesys_lock);
  bool crt = filesys_create(kname, initial_size);
  lock_release(&filesys_lock);
  return crt;
}


static bool
system_remove(const char *file) {
  char kname[NAME_MAX + 1];
  if (!copy_in_string(kname, file, sizeof kname))
    return false;

  lock_acquire(&filesys_lock);
  bool rem = filesys_remove(kname);
  lock_release(&filesys_lock);
  return rem;
}

static int
system_open(const char *file) {
  char kname[NAME_MAX + 1];
  if (!copy_in_string(kname, file, sizeof kname))
    return -1;

  lock_acquire(&filesys_lock);
  struct file *f = filesys_open(kname);
  lock_release(&filesys_lock);
  if (f == NULL) return -1;

  int fd = fd_alloc(f);
  if (fd < 0) {
    lock_acquire(&filesys_lock);
    file_close(f);
    lock_release(&filesys_lock);
    return -1;
  }

  if (!fdref_inc(f)) {                  // 실패 시
    thread_current()->fd_table[fd] = NULL;
    lock_acquire(&filesys_lock);
    file_close(f);                      // 직접 닫기
    lock_release(&filesys_lock);
    return -1;
  }
  return fd;
}

static void
system_close(int fd) {
  struct thread *t = thread_current();

  if (fd < 0 || fd >= t->fd_cap) return;   // 범위 밖
  struct file *f = t->fd_table[fd];
  if (f == NULL) return;

  if (!f) return;
  t->fd_table[fd] = NULL;
  fdref_dec(f);
}

static int
system_filesize(int fd) {
  struct file *f = fd_get(fd);
  if (f == STDOUT_FD || f == STDIN_FD) return -1;
  if (f == NULL) return -1;

  lock_acquire(&filesys_lock);
  off_t len = file_length(f);
  lock_release(&filesys_lock);
  return (int)len;
}

static int
system_read(int fd, void *buffer, unsigned size) {
  if (size == 0) return 0;

  struct file *f = fd_get(fd);
  if (!f) return -1;

  if (f == STDIN_FD) {                                // 키보드
    for (unsigned i = 0; i < size; i++) {
      uint8_t key = (uint8_t)input_getc();
      copy_out((uint8_t*)buffer + i, &key, 1);
    }
    return (int)size;
  }

  if (f == STDOUT_FD) return -1;

  void *read_page = palloc_get_page(PAL_ZERO);
  if (read_page == NULL) return -1;

  int total = 0;

  while (total < (int)size) {
    size_t chunk = size - total;
    if (chunk > PGSIZE) chunk = PGSIZE;

    lock_acquire(&filesys_lock);
    off_t n = file_read(f, read_page, (off_t)chunk);
    lock_release(&filesys_lock);

    if (n <= 0) break;

    copy_out((uint8_t*)buffer + total, read_page, (size_t)n);
    total += (int)n;
  }
  palloc_free_page(read_page);
  return total;
}

static long
system_write (int fd, const void *buf, unsigned size) {
  if (size == 0) return 0;
  if (buf == NULL) system_exit(-1);

  void *kpage = palloc_get_page(0);
  if (!kpage) return -1;

  long total = 0;

  struct file *f = fd_get(fd);
  if (!f) { palloc_free_page(kpage); return -1; }

  if (f == STDOUT_FD) {              // STDOUT
    while ((unsigned)total < size) {
      size_t chunk = size - (unsigned)total;
      if (chunk > PGSIZE) chunk = PGSIZE;

      copy_in(kpage, (const uint8_t *)buf + total, chunk);
      putbuf((const char *)kpage, chunk);

      total += (long)chunk;
    }
    palloc_free_page(kpage);
    return total;
  }

  if (f == STDIN_FD) { palloc_free_page(kpage); return -1; }

  while ((unsigned)total < size) {
    size_t chunk = size - (unsigned)total;
    if (chunk > PGSIZE) chunk = PGSIZE;

    copy_in(kpage, (const uint8_t *)buf + total, chunk);

    lock_acquire(&filesys_lock);
    off_t n = file_write(f, kpage, chunk);
    lock_release(&filesys_lock);

    if (n <= 0) break;
    total += (long)n;
    if ((size_t)n < chunk) break;
  }
  palloc_free_page(kpage);
  return total;
}


static void
system_seek(int fd, unsigned position) {
  struct file *f = fd_get(fd);
  if (f == NULL) return;
  if (f == STDOUT_FD || f == STDIN_FD) return;

  lock_acquire(&filesys_lock);
  file_seek(f, position);
  lock_release(&filesys_lock);
}

static int
system_dup2(int oldfd, int newfd) {
  struct thread *t = thread_current();
  
  if (oldfd < 0 || oldfd >= t->fd_cap) return -1;
  if (oldfd >= t->fd_cap || newfd >= t->fd_cap) return -1;
  if (oldfd == newfd) return newfd;

  struct file *oldf = t->fd_table[oldfd];
  if (!oldf) return -1;

  if (t->fd_table[newfd] == oldf) return newfd;

  if (!fdref_inc(oldf)) return -1;

  if (t->fd_table[newfd]) system_close(newfd);

  t->fd_table[newfd] = oldf;
  return newfd;
}

// fd 뽑아보기
static struct
file *fd_get(int fd) {
  struct thread *t = thread_current();
  if (fd < 0 || fd >= t->fd_cap) return NULL;
  return t->fd_table[fd];
}

// 유저 주소 범위가 전부 매핑돼 있는지 확인
static void
assert_user_range(const void *uaddr, size_t size) {
  if (uaddr == NULL) system_exit(-1);
  if (size == 0) return;
  const uint8_t *begin = (const uint8_t *)uaddr;
  const uint8_t *end   = begin + size - 1;

  // 범위를 페이지 경계 단위로 훑는다
  for (const uint8_t *p = pg_round_down(begin); p <= end; p += PGSIZE) {
    if (!is_user_vaddr(p)) system_exit(-1);                 // 유저 주소 맞는지 체크
    if (pml4_get_page(thread_current()->pml4, p) == NULL)   // p매핑 pml4에 되어있는지 체크
      system_exit(-1);
  }
}

static bool
copy_in_string(char *kdst, const char *usrc, size_t max_len) {
  if (usrc == NULL) system_exit(-1);

  size_t i = 0;
  while (i < max_len) {
    const uint8_t *u = (const uint8_t *)usrc + i;

    if (!is_user_vaddr(u)) system_exit(-1);

    void *kp = pml4_get_page(thread_current()->pml4, pg_round_down(u));
    if (kp == NULL) system_exit(-1);

    char c = *(((char *)kp) + pg_ofs(u));  // base + ofs 로 1바이트 읽기
    kdst[i++] = c;
    if (c == '\0') return true;  // 정상 종료: 제한 내에서 NUL 발견
  }

  kdst[max_len - 1] = '\0';      // 끝문자열 null 처리 (최대 길이 벗어나서 절삭)
  return false;
}

static void
copy_in(void *kdst, const void *usrc, size_t n) {
  uint8_t *kd = (uint8_t *)kdst;
  const uint8_t *u = (const uint8_t *)usrc;

  while (n > 0) {
    if (!is_user_vaddr(u)) system_exit(-1);

    const void *kp = pml4_get_page(thread_current()->pml4, pg_round_down(u));
    if (kp == NULL) system_exit(-1);

    const uint8_t *ksrc = (const uint8_t *)kp + pg_ofs(u);

    size_t chunk = PGSIZE - pg_ofs(u);
    if (chunk > n) chunk = n;

    memcpy(kd, ksrc, chunk);
    kd += chunk;
    u  += chunk;
    n  -= chunk;
  }
}

static void
copy_out(void *udst, const void *ksrc, size_t n) {
  uint8_t *u = (uint8_t *)udst;
  const uint8_t *k = (const uint8_t *)ksrc;

  while (n > 0) {
    if (!is_user_vaddr(u)) system_exit(-1);

    void *kp = pml4_get_page(thread_current()->pml4, pg_round_down(u));
    if (kp == NULL) system_exit(-1);

    uint8_t *kdst = (uint8_t *)kp + pg_ofs(u);

    size_t chunk = PGSIZE - pg_ofs(u);
    if (chunk > n) chunk = n;

    memcpy(kdst, k, chunk);
    u += chunk;
    k += chunk;
    n -= chunk;
  }
}

static bool
fd_ensure_table(void) {
  struct thread *t = thread_current();
  if (t->fd_table && t->fd_cap > 0) return true;

  int cap = FD_GROW_STEP;
  struct file **newtab = (struct file **)palloc_get_page(PAL_ZERO);
  if (!newtab) return false;

  t->fd_table = newtab;
  t->fd_cap   = PGSIZE / (int)sizeof(t->fd_table[0]);

  t->fd_table[0] = STDIN_FD;
  t->fd_table[1] = STDOUT_FD;

  t->fd_table_from_palloc = true;
  return true;
}

static int fd_alloc(struct file *f) {
  struct thread *t = thread_current();
  if (!fd_ensure_table()) return -1;

  for (int i = FIRST_FD; i < t->fd_cap; i++) {
    if (t->fd_table[i] == NULL) {
      t->fd_table[i] = f;
      return i;
    }
  }
  return -1;
}


// static struct
// file_ref *ref_find(struct file *fp) {
//   struct list_elem *e;
//   for (e = list_begin(&file_ref_list); e != list_end(&file_ref_list); e = list_next(e)) {
//     struct file_ref *r = list_entry(e, struct file_ref, elem);
//     if (r->fp == fp) return r;
//   }
//   return NULL;
// }

// 카운트 up
bool
fdref_inc(struct file *fp) {
  if (fp == (struct file*)-1 || fp == (struct file*)-2) return true; 
  lock_acquire(&file_ref_lock);
  struct file_ref *r = ref_find(fp);
  if (!r) {
    r = malloc(sizeof *r);
    if (!r) {                      // 안전 처리
      lock_release(&file_ref_lock);
      return false;
    }
    r->fp = fp;
    r->refcnt = 1;
    hash_insert(&file_ref_ht, &r->elem);
    // list_push_back(&file_ref_list, &r->elem);
  } else {
    r->refcnt++;
  }
  lock_release(&file_ref_lock);
  return true;
}

// 카운트 down
void
fdref_dec(struct file *fp) {
  /* 얘도 마찬가지 */
  if (fp == (struct file*)-1 || fp == (struct file*)-2) return;
  lock_acquire(&file_ref_lock);
  struct file_ref *r = ref_find(fp);
  ASSERT(r != NULL);
  if (--r->refcnt == 0) {
    // list_remove(&r->elem);
    hash_delete(&file_ref_ht, &r->elem);
    lock_release(&file_ref_lock);
    // 마지막 참조 해제 시 실제 close
    lock_acquire(&filesys_lock);
    file_close(fp);
    lock_release(&filesys_lock);
    free(r);
    return;
  }
  lock_release(&file_ref_lock);
}


static unsigned file_ref_hash(const struct hash_elem *e, void *aux) {
  const struct file_ref *r = hash_entry(e, struct file_ref, elem);
  return hash_bytes(&r->fp, sizeof r->fp);
}
static bool file_ref_less(const struct hash_elem *a, const struct hash_elem *b, void *aux) {
  const struct file_ref *ra = hash_entry(a, struct file_ref, elem);
  const struct file_ref *rb = hash_entry(b, struct file_ref, elem);
  return ra->fp < rb->fp;
}
static struct file_ref *ref_find(struct file *fp) {
  struct file_ref key; key.fp = fp;
  struct hash_elem *e = hash_find(&file_ref_ht, &key.elem);
  return e ? hash_entry(e, struct file_ref, elem) : NULL;
}
