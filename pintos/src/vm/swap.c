#include "vm/swap.h"
#include <bitmap.h>
#include <round.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "devices/disk.h"
#include "filesys/file.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "vm/frame.h"
#include "vm/page.h"

#define PAGE_SIZE_IN_SECTORS (DIV_ROUND_UP (PGSIZE, DISK_SECTOR_SIZE))

static struct disk * swap_disk;
static struct bitmap * swap_pool;
static struct lock swap_lock;

void
init_swap (void)
{
  swap_disk = disk_get (1, 1);
  swap_pool = bitmap_create (disk_size (swap_disk) / PAGE_SIZE_IN_SECTORS);
  bitmap_set_all (swap_pool, false);
  lock_init (&swap_lock);
}

bool
swap_in (struct thread * holder, struct page * spg, void * pg)
{
  size_t i;
  void * address = spg->address;
  if (spg->isswap)
    {
      /* Load this page from swap. */
      for (i = 0; i < PAGE_SIZE_IN_SECTORS; ++i)
        disk_read (swap_disk, spg->offset * PAGE_SIZE_IN_SECTORS + i,
                   pg + i * DISK_SECTOR_SIZE);
      lock_acquire (&swap_lock);
      ASSERT (bitmap_test (swap_pool, spg->offset));
      bitmap_reset (swap_pool, spg->offset);
      lock_release (&swap_lock);
      /* The page is now in memory, so no need of recording it in the suppl
       * page table. */
      lock_acquire (&holder->suppl_page_table_lock);
      delete_suppl_page (holder, address);
      lock_release (&holder->suppl_page_table_lock);
    }
  else
    {
      /* Load this page from exectubale. */
      if (file_read (holder->executable, pg, spg->read_bytes)
            != (int) spg->read_bytes)
          return false; 
      memset (pg + spg->read_bytes, 0, PGSIZE - spg->read_bytes);
      /* No need to delete it from supplementary page table. */
    }
  /* Add the page to the frame table. */
  if (!add_frame (holder, pg, address))
    return false;
  /* Add the page to the page directory. */
  lock_acquire (&holder->pagedir_lock);
  pagedir_set_page (holder->pagedir, address, pg, true);
  lock_release (&holder->pagedir_lock);
  return true;
}

void *
swap_out (void)
{
  size_t pg_index;
  disk_sector_t disk_index;
  size_t i;
  struct frame * victim = evict_frame ();
  void * victim_address = victim->address;
  lock_acquire (&victim->holder->suppl_page_table_lock);
  struct page * spg = search_suppl_page (victim->holder, victim->vaddr);
  lock_release (&victim->holder->suppl_page_table_lock);
  if (spg == NULL)
    {
      /* The victim is not in the supplementary page table,
       * i.e. it is not a code segment. */
      lock_acquire (&swap_lock);
      pg_index = bitmap_scan_and_flip (swap_pool, 0, 1, false);
      lock_release (&swap_lock);
      if (pg_index == BITMAP_ERROR) return NULL;
      victim_address = victim->address;
      disk_index = pg_index * PAGE_SIZE_IN_SECTORS;
      for (i = 0; i < PAGE_SIZE_IN_SECTORS; ++i)
        disk_write (swap_disk, disk_index + i,
                    victim_address + i * DISK_SECTOR_SIZE);
      lock_acquire (&victim->holder->suppl_page_table_lock);
      if (!add_suppl_page (victim->holder, victim->vaddr, pg_index, 0, true))
        {
          lock_release (&victim->holder->suppl_page_table_lock);
          return NULL;
        }
      lock_release (&victim->holder->suppl_page_table_lock);
    }
  /* If the victim is a code segment, it can just be cleared from
   * memory, since it can be loaded from executable. */
  lock_acquire (&victim->holder->pagedir_lock);
  pagedir_clear_page (victim->holder->pagedir, victim->vaddr);
  lock_release (&victim->holder->pagedir_lock);
  free (victim);
  return victim_address;
}

