#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdbool.h>
#include "devices/disk.h"
#include "threads/thread.h"
#include "vm/page.h"

void init_swap (void);
void acquire_tloatol (void);
void release_tloatol (void);
void delete_swap (struct page * spg);
void swap_in (disk_sector_t index, void * pg);
void * swap_out (void * vaddr);

#endif  /* VM_SWAP_H */
