#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include <stdbool.h>

/* If set to true, every time a lock is acquired or release,
 * the current thread and the name of the lock is printed. */
#define DEBUG_DEADLOCK (false)

/* Data structure to store information about frames. */
struct frame
  {
    void * address;           /* the kernel virtual address of the frame */
    struct thread * holder;   /* holder of the frame. */
    void * vaddr;             /* the virtual address of the page. */
    struct list_elem elem;
  };

void init_frame (void);
bool add_frame (void * address, void * vaddr);
void delete_frame (void * address);
void evict_frame (void * vaddr, struct frame * old);

#endif  /* vm/frame.h */
