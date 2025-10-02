#ifndef VM_ANON_H
#define VM_ANON_H

#include <stdbool.h>   // bool
#include <stddef.h>    // size_t
struct page;
enum vm_type;

struct anon_page {
  // 미할당 상태 : SIZE_MAX or 할당 상태 : 해당 슬롯 idx
  size_t slot_idx;
};

void vm_anon_init (void);
bool anon_initializer (struct page *page, enum vm_type type, void *kva);

#endif
