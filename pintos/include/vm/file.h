#ifndef VM_FILE_H
#define VM_FILE_H

#include "filesys/file.h"
#include "vm/vm.h"
#include "lib/kernel/list.h"

struct page;
enum vm_type;

struct mmap_file {
  struct file *file;   /* file_reopen() 한 핸들 (매핑 단위로 1개) */
  void *base;          /* 매핑 시작 유저 주소 (page-aligned) */
  size_t length;       /* 요청 길이 (바이트, 마지막 페이지는 일부만 사용 가능) */
  struct list_elem elem;
};

struct mmap_ctx {
  struct file *file;
  size_t refcnt;
};

struct file_page {
  struct file *file;       /* backing file (mmap이나 exec) */
  off_t offset;               /* 이 페이지의 파일 오프셋 */
  size_t read_bytes;       /* fault-in 시 파일에서 읽을 바이트 수 */
  size_t zero_bytes;       /* 나머지를 0으로 채울 바이트 수 */
  bool is_mmap;
  struct mmap_ctx *ctx;   // 추가: 매핑 단위 공유 핸들
};


void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif
