#ifndef VM_FILE_H
#define VM_FILE_H
#include "filesys/file.h"
#include "vm/vm.h"

struct page;
enum vm_type;

struct file_page {
	struct file *file;     /* 이 페이지의 데이터 원본 파일(각 페이지별 duplicate) */
	off_t  ofs;            /* 파일 내 오프셋 */
	size_t read_bytes;     /* 이 페이지에서 파일로부터 읽을 바이트 */
	size_t zero_bytes;     /* 남은 부분 0 채움 */
	bool   is_mmap;        /* mmap로 생성된 페이지인지 */
	bool   is_head;        /* 해당 mmap 영역의 첫 페이지인지 */
	size_t npages;         /* is_head일 때만 유효: mmap 영역 페이지 수 */
};

void vm_file_init (void);
bool file_backed_initializer (struct page *page, enum vm_type type, void *kva);
void *do_mmap(void *addr, size_t length, int writable,
		struct file *file, off_t offset);
void do_munmap (void *va);
#endif
