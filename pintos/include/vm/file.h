#ifndef VM_FILE_H
#define VM_FILE_H

#include <stdbool.h>
#include <stddef.h>         // size_t
#include "filesys/file.h"
#include "lib/kernel/list.h"

struct page;
enum vm_type;

/* 프로세스의 mmap 영역 메타 */
struct mmap_region {
  void *addr;                 // mmap 시작 가상주소(페이지 정렬)
  size_t page_cnt;            // 매핑된 페이지 수
  struct list_elem elem;
};


struct file_page {
  struct file *file;
  off_t offset;
  size_t read_bytes;
  size_t zero_bytes;
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif
