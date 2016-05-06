#include "vm/swap.h"
#include <bitmap.h>
#include <round.h>
#include <stddef.h>
#include "devices/disk.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

#define PAGE_SIZE_IN_SECTORS (DIV_ROUND_UP (PGSIZE, DISK_SECTOR_SIZE))

static struct disk * swap_disk;
static size_t page_size;
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

void
swap_in (disk_sector_t index, struct frame * )
{
  size_t i;
  for (i = 0; i < PAGE_SIZE_IN_SECTORS; ++i)
    disk_read (swap_disk, disk_index + i, page + i * DISK_SECTOR_SIZE);
  lock_acquire (&swap_lock);
  ASSERT (bitmap_test (swap_pool, index));
  bitmap_reset (swap_pool, index);
  lock_release (&swap_lock);
}

disk_sector_t
swap_out (struct frame * victim)
{
  size_t pg_index;
  disk_sector_t disk_index;
  size_t i;
  lock_acquire (&swap_lock);
  pg_index = bitmap_scan_and_flip (swap_pool, 0, 1, false);
  lock_release (&swap_lock);
  if (pg_index == BITMAP_ERROR) return -1;
  disk_index = pg_index * PAGE_SIZE_IN_SECTORS;
  for (i = 0; i < PAGE_SIZE_IN_SECTORS; ++i)
    disk_write (swap_disk, disk_index + i, page + i * DISK_SECTOR_SIZE);
  return disk_index;
}

