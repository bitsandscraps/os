#include "vm/page.h"
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/swap.h"

/* Helper functions for acquiring and releasing locks. */

/* Acquires lock on the thread HOLDER's supplementary page table. */
void lock_suppl_page_table (struct thread * holder)
{
  if (DEBUG_DEADLOCK)
    printf ("thread %p acquires sptl of %p\n", thread_current (), holder);
  lock_acquire (&holder->suppl_page_table_lock);
}

/* Releases lock on the thread HOLDER's supplementary page table. */
void unlock_suppl_page_table (struct thread * holder)
{
  if (DEBUG_DEADLOCK)
    printf ("thread %p releases sptl of %p\n", thread_current (), holder);
  lock_release (&holder->suppl_page_table_lock);
}

/* Acquires lock on the thread HOLDER's page directory. */
void lock_pagedir (struct thread * holder)
{
  if (DEBUG_DEADLOCK)
    printf ("thread %p acquires pdl of %p\n", thread_current (), holder);
  lock_acquire (&holder->pagedir_lock);
}

/* Releases lock on the thread HOLDER's page directory. */
void unlock_pagedir (struct thread * holder)
{
  if (DEBUG_DEADLOCK)
    printf ("thread %p releases pdl of %p\n", thread_current (), holder);
  lock_release (&holder->pagedir_lock);
}

/* Hash function for the supplementary page table. */
static unsigned
page_hash (const struct hash_elem * elem, void * aux UNUSED)
{
  const struct page * elem_pg = hash_entry (elem, struct page, elem);
  return hash_bytes (&elem_pg->address, sizeof (elem_pg->address));
}

/* Comparison function for the supplementary page table. */
static bool
page_less (const struct hash_elem * a, const struct hash_elem * b,
           void * aux UNUSED)
{
  const struct page * a_pg = hash_entry (a, struct page, elem);
  const struct page * b_pg = hash_entry (b, struct page, elem);
  return a_pg->address < b_pg->address;
}

/* Free routine called when the supplementary page table is destroyed.
 * If the page is in swap, it is removed from swap table. If the page
 * is in memory, the corresponding frame is removed from frame table. */
static void
page_free (struct hash_elem * elem, void * aux UNUSED)
{
  struct page * elem_pg = hash_entry (elem, struct page, elem);
  if (elem_pg->status == IN_SWAP)
    delete_swap (elem_pg);
  else if (elem_pg->status == IN_MEMORY)
    delete_frame (pagedir_get_page (thread_current ()->pagedir,
                                    elem_pg->address));
  free (elem_pg);
}

/* Initializes the supplementary page table of the thread HOLDER.
 * Called in init_thread of threads/thread.c. */
bool
init_suppl_page_table (struct thread * holder)
{
  ASSERT (holder != NULL);
  return hash_init (&holder->suppl_page_table, page_hash, page_less, NULL);
}

/* Copies the given page SRC and add it to the supplementary page table
 * of the current thread. Returns the added node. If malloc failed,
 * returns NULL. */
struct page *
add_suppl_page (struct page * src)
{
  struct page * spg = (struct page *)(malloc (sizeof (struct page)));
  struct hash_elem * elem;
  if (spg == NULL) return NULL;
  struct thread * curr = thread_current ();
  ASSERT (src->address < PHYS_BASE);
  spg->address = src->address;
  spg->offset = src->offset;
  spg->read_bytes = src->read_bytes;
  spg->status = src->status;
  spg->writable = src->writable;
  elem = hash_insert (&curr->suppl_page_table, &spg->elem);
  ASSERT (elem == NULL);
  return spg;
}

/* Destroyes the supplementary page table of the thread HOLDER. All
 * the dynamically allocated memory are freed, at least we hope so. */
void
delete_suppl_page_table (struct thread * holder)
{
  ASSERT (holder != NULL);
  /* Must acquire TLOATOL before starting anything. */
  acquire_tloatol ();
  lock_acquire (&holder->suppl_page_table_lock);
  lock_acquire (&holder->pagedir_lock);
  hash_destroy (&holder->suppl_page_table, page_free);
  release_tloatol ();
  lock_release (&holder->suppl_page_table_lock);
  lock_release (&holder->pagedir_lock);
}

/* Edit the given SPG to be in status STATUS and offset OFFSET. Address,
 * writable, and read_bytes are left unchanged. No sychronization is
 * done, must be called after acquiring mutex of supplementary page
 * table.*/ 
void
modify_suppl_page (struct page * spg, enum page_status status, uint32_t offset)
{
  ASSERT (spg != NULL);
  spg->status = status;
  spg->offset = offset;
}

/* Finds the supplementary page table element for the given page of
 * user virtual address ADDRESS from the supplementary page table of
 * thread HOLDER. Returns that page table element. If failed, returns
 * NULL. */
struct page *
search_suppl_page (struct thread * holder, void * address)
{
  struct page spg;
  ASSERT (holder != NULL);
  ASSERT (address < PHYS_BASE);
  spg.address = address;
  struct hash_elem * elem = hash_find (&holder->suppl_page_table, &spg.elem);
  if (elem == NULL) return NULL;
  return hash_entry (elem, struct page, elem);
}

/* Loads a page that is not in memory yet, according to the given SPG
 * that must be already in the supplementary page table of the current
 * thread. Returns true if suceeded. TLOATOL must be acquired before
 * call, since it is released during call. */
bool
load_page (struct page * spg)
{
  ASSERT (spg != NULL);
  void * address = spg->address;
  ASSERT (address < PHYS_BASE);
  bool freepage;    /* Set to true if it allocates a free page. */
  bool dir_result;
  void * pg = palloc_get_page (PAL_USER);
  uint32_t offset = 0;
  enum page_status status;
  struct thread * curr = thread_current ();
  freepage = (pg != NULL);
  if (freepage)
    {
      /* Add the newly allocated frame to the frame table. */
      if (!add_frame (pg, spg->address))
        {
          release_tloatol ();
          palloc_free_page (pg);
          return false;
        }
      release_tloatol ();
    }
  /* There is no free frame. We must swap a frame out. TLOATOL is
   * released by swap_out. */
  else
    pg = swap_out (spg->address);
  if (pg == NULL)
    return false;
  switch (spg->status)
    {
      /* Given SPG must not already be in memory. */
      case IN_MEMORY:
        ASSERT (false);
      case IN_SWAP:
        /* Load this page from swap space. */
        swap_in (spg->offset, pg);
        break;
      case IN_FILE:
        /* Load this page from exectubale. */
        if (file_read_at (curr->executable, pg, spg->read_bytes, spg->offset)
               != (int) spg->read_bytes)
          {
            if (freepage) palloc_free_page (pg);
            return false;
          }
        memset (pg + spg->read_bytes, 0, PGSIZE - spg->read_bytes);
        /* The offset must be recorded. */
        offset = spg->offset;
        break;
      case GROWING_STACK:
        memset (pg, 0, PGSIZE);
        break;
      default:
        ASSERT (false);
    }
  /* Modify the supplementary page table correctly. No need to set
   * offset for a page in memmory. */
  status = IN_MEMORY;
  modify_suppl_page (spg, status, offset);
  /* Add the page to the page directory. */
  lock_pagedir (curr);
  dir_result = pagedir_set_page (curr->pagedir, address, pg, spg->writable);
  unlock_pagedir (curr);
  if (!dir_result)
    {
      if (freepage) palloc_free_page (pg);
      return false;
    }
  return true;
}

