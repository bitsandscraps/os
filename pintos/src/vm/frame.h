#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stdbool.h>
#include <stddef.h>

/* Data structure to store information about frames. */
struct frame
  {
    void * address;           /* the kernel virtual address of the frame */
    struct thread * holder;   /* holder of the frame. */
    void * vaddr;             /* the virtual address of the page. */
    struct hash_elem elem;
  };

void init_frame (void);
bool add_frame (struct thread * holder, void * address, void * vaddr);
void delete_frame (void * address);

#endif  /* vm/frame.h */
