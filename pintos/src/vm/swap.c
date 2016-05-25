#include "vm/swap.h"
#include <bitmap.h>
#include <round.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/page.h"

/* What it means is quite obvious from its name. */
#define PAGE_SIZE_IN_SECTORS (DIV_ROUND_UP (PGSIZE, DISK_SECTOR_SIZE))

/* ORDER OF ACQUIRING LOCKS
 * The locks should be acquired according to the following order:
 *    the lock over all the other locks(TLOATOL)
 *    supplementary page table lock of the current thread
 *    frame lock
 *    supplementary page table lock of another thread
 *    swap lock
 *    page directory lock of another thread
 *    page directory lock of the current thread
 * If acquiring does not follow the order, deadlock may happen. */

/* Pointer to the swap space. */
static struct disk * swap_disk;
/* Bitmap to maintain information about empty slots. */
static struct bitmap * swap_pool;
/* Mutex associated to swap_pool. */
static struct lock swap_lock;
/* Meta lock to prevent deadlocks. Abbreviated to TLOATOL. */
static struct lock the_lock_over_all_the_other_locks;

/* Acquries the_lock_over_all_the_other_locks. It must be acquired
 * before it waits for its supplementary page table lock. */
void
acquire_tloatol (void)
{
  if (DEBUG_DEADLOCK)
    printf ("thread %p acquires tloatol\n", thread_current ());
  lock_acquire (&the_lock_over_all_the_other_locks);
}

/* Releases the_lock_over_all_the_other_locks. */
void
release_tloatol (void)
{
  if (DEBUG_DEADLOCK)
    printf ("thread %p releases tloatol\n", thread_current ());
  lock_release (&the_lock_over_all_the_other_locks);
}

/* Acquries the swap_lock. */
static void
lock_swap (void)
{
  if (DEBUG_DEADLOCK)
    printf ("thread %p acquires swap\n", thread_current ());
  lock_acquire (&swap_lock);
}

/* Releases the swap_lock. */
static void
unlock_swap (void)
{
  if (DEBUG_DEADLOCK)
    printf ("thread %p releases swap\n", thread_current ());
  lock_release (&swap_lock);
}

/* Intializes the swap space and associated locks. Called in
 * threads/init.c. */
void
init_swap (void)
{
  /* Swap space is hd1:1. */
  swap_disk = disk_get (1, 1);
  swap_pool = bitmap_create (disk_size (swap_disk) / PAGE_SIZE_IN_SECTORS);
  bitmap_set_all (swap_pool, false);
  lock_init (&swap_lock);
  lock_init (&the_lock_over_all_the_other_locks);
}

/* Mark the slot where the given page SPG resides as free. SPG must be
 * currently in swap space. */
void
delete_swap (struct page * spg)
{
  ASSERT (spg != NULL);
  ASSERT (spg->status == IN_SWAP);
  lock_swap ();
  bitmap_reset (swap_pool, spg->offset);
  unlock_swap ();
}

/* Copy the INDEX-th page stored in the swap space to the kerel virtual
 * address PG. Mark the slot whre the page originally was as vacant. */
void
swap_in (disk_sector_t index, void * pg)
{
  size_t i;
  /* Load this page from swap. */
  for (i = 0; i < PAGE_SIZE_IN_SECTORS; ++i)
    disk_read (swap_disk, index * PAGE_SIZE_IN_SECTORS + i,
               pg + i * DISK_SECTOR_SIZE);
  lock_swap ();
  /* Check whether the given index is anywhere valid. */
  ASSERT (bitmap_test (swap_pool, index));
  bitmap_reset (swap_pool, index);  /* Mark as empty. */
  unlock_swap ();
}

/* Evicts a frame from the physical memmory and swap it into the swap
 * space. Updates the page tables both initial and supplementary of the
 * victim's holder. VADDR is the user virtual address of the page that
 * will be added in place of the victim. Returns the kernel virtual
 * address of the evicted frame. Must be called after acquiring the
 * TLOATOL. It releases TLOATOL during its operation. */
void *
swap_out (void * vaddr)
{
  size_t pg_index;
  size_t i;
  struct frame old;
  evict_frame (vaddr, &old);
  struct page * spg = search_suppl_page (old.holder, old.vaddr);
  ASSERT (spg != NULL);
  enum page_status status = IN_FILE;
  uint32_t offset = spg->offset;
  switch (spg->type)
    {
      case TO_SWAP:
        /* The victim must be cached in swap space because it is writable. */
        lock_swap ();
        release_tloatol ();
        /* Search for an empty slot. */
        pg_index = bitmap_scan_and_flip (swap_pool, 0, 1, false);
        unlock_swap ();
        if (pg_index == BITMAP_ERROR) return NULL;
        for (i = 0; i < PAGE_SIZE_IN_SECTORS; ++i)
          disk_write (swap_disk, pg_index * PAGE_SIZE_IN_SECTORS + i,
                      old.address + i * DISK_SECTOR_SIZE);
        status = IN_SWAP;
        offset = pg_index;
        break;
      case TO_FILE:
        /* Mmaped file. Write back to the file only if it is dirty. */
        release_tloatol ();
        if (pagedir_is_dirty (old.holder->pagedir, old.vaddr))
          file_write_at (spg->file, old.address, spg->read_bytes, spg->offset);
        break;
      case READ_ONLY:
        /* Victim is not written to swap since it can always be read
         * from executable. */
        release_tloatol ();
        break;
      default:
        ASSERT (false);
    }
  modify_suppl_page (spg, status, offset);
  /* Clear the page from pagedir. */
  lock_pagedir (old.holder);
  pagedir_clear_page (old.holder->pagedir, old.vaddr);
  unlock_pagedir (old.holder);
  /* The lock is acquired during the function call of evict_frame. */
  if (thread_current () != old.holder)
    unlock_suppl_page_table (old.holder);
  return old.address;
}

