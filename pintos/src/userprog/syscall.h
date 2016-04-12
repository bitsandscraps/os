#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

#define KERNEL_TERMINATE (-1)

void syscall_init (void);
void syscall_exit (int status);


#endif /* userprog/syscall.h */
