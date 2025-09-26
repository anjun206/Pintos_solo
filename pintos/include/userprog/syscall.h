#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#include <stdbool.h>
#include "threads/interrupt.h"

struct file;
extern struct lock filesys_lock;

void syscall_init (void);
void system_exit (int status);
bool fdref_inc(struct file *fp);
void fdref_dec(struct file *fp);

#endif /* userprog/syscall.h */
