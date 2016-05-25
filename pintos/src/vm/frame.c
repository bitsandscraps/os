#include "vm/frame.h"
#include <stdio.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "vm/page.h"

/* Circular list of physical frames in the memory. */
static struct list frame_table;

/* Mutex associated to frame_table. */
static struct lock frame_lock;

/* Current node of frame_table. */
static struct list_elem * frame_curr;

/* Initialize frame_table, frame_lock, and frame_curr. Called in
 * threads/init.c. */
void
init_frame (void)
{
  list_init (&frame_table);
  frame_curr = list_end (&frame_table);
  lock_init (&frame_lock);
}

/* Acquire frame_lock. This function must be called before calling any
 * other operations on frame_table. */
static void
lock_frame (void)
{
  if (DEBUG_DEADLOCK)
    printf ("thread %p acquires frame\n", thread_current ());
  lock_acquire (&frame_lock);
}

/* Release frame_lock. This function must be called after all the
 * manipulations on frame_table is completed. */
static void
unlock_frame (void)
{
  if (DEBUG_DEADLOCK)
    printf ("thread %p releases frame\n", thread_current ());
  lock_release (&frame_lock);
}

/* Add the information of a physical frame at physical address, actually
 * the kernel virtual address, ADDRESS to the frame_table. VADDR is the
 * corresponding user virtual address, which is needed to access the
 * page table. Returns false if malloc fails. */
bool
add_frame (void * address, void * vaddr)
{
  struct frame * fr = (struct frame *)(malloc (sizeof (struct frame)));
  if (fr == NULL) return false;
  ASSERT (address != NULL);
  ASSERT (vaddr != NULL);
  fr->address = address;
  fr->holder = thread_current ();
  fr->vaddr = vaddr;
  lock_frame ();
  list_push_front (&frame_table, &fr->elem);
  unlock_frame ();
  return true;
}

/* Remove the node holding the information on physical frame at kernel
 * virtual address ADDRESS. External synchronization is needed to
 * ensure atomicity. Returns the removed node. */
static struct frame * 
remove_frame (void * address)
{
  struct list_elem * elem;
  ASSERT (address != NULL);
  for (elem = list_begin (&frame_table); elem != list_end (&frame_table);
       elem = list_next (elem))
    {
      struct frame * fr = list_entry (elem, struct frame, elem);
      if (fr->address == address)
        {
          /* Move the iterator if it is the current list element.
           * Without this, frame_curr may become invalid. */
          if (frame_curr == elem)
            frame_curr = list_remove (elem);
          else
            list_remove (elem);
          return fr; 
        }
    }
  /* The caller is trying to remove a physical frame that is not in the
   * frame_table. */
  ASSERT (false);
  return NULL;
}

/* Remove and free the node with the information about the physical
 * frame with kernel virtual address ADDRESS from frame_table.
 * Appropriate synchronization is performed. */
void
delete_frame (void * address)
{
  ASSERT (address != NULL);
  lock_frame ();
  free (remove_frame (address));
  unlock_frame ();
}

/* Pick a frame to evict according to the second chance algorithm.
 * Edit the table so that the physical frame is now held by the current
 * thread and its user virtual address is VADDR. The original
 * information about the evicted frame is stored in OLD. Returns true
 * if it had found a frame to evict before arriving at the end of the
 * list. If false is returned, the function should be called once more.
 * The lock of the suppl_page_table of the current thread must be
 * acquired before calling this function. */
static bool
evict_loop (void * vaddr, struct frame * old)
{
  struct frame * victim;
  struct thread * curr = thread_current ();
  if (frame_curr == list_end (&frame_table))
    frame_curr = list_begin (&frame_table);
  for ( ; frame_curr != list_end (&frame_table);
        frame_curr = list_next (frame_curr))
    {
      victim = list_entry (frame_curr, struct frame, elem);
      /* Second chance algorithm. */
      if (pagedir_is_accessed (victim->holder->pagedir, victim->vaddr))
        pagedir_set_accessed (victim->holder->pagedir, victim->vaddr, false);
      else
        {
          /* If the holder of the victim is current thread, the caller
           * have already acquired lock of the suppl page table. */
          if (victim->holder != curr)
            lock_suppl_page_table (victim->holder);
          *old = *victim;           /* Copy the original to OLD. */
          victim->holder = curr;    /* Update the frame table. */
          victim->vaddr = vaddr;
          return true;
        }
    }
  return false;
}

/* Wrapper function of evict_loop. Does some synchronization jobs and
 * keeps on calling evict_loop until it finds a frame to evict. VADDR
 * is the virtual address of the page that will be swapped in. The
 * original information of the frame is stored in OLD. */
void
evict_frame (void * vaddr, struct frame * old)
{
  lock_frame ();
  /* Loop until evict_loop finds a frame to evict. */
  while (!evict_loop (vaddr, old))
    continue;
  unlock_frame ();
}

