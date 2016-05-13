#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "threads/thread.h"

/* Status of each page. We believe that what each state means is quite
 * clear from the name. */
enum page_status { IN_MEMORY, IN_SWAP, IN_FILE, GROWING_STACK };

/* Supplementary page table element. */
struct page
  {
    void * address;             /* user virtual address of the page. */
    enum page_status status;    /* The status of the page. */
    bool writable;              /* true if it is writable. */
    /* File offset for read-only pages. Swap index for writable pages.
     * It can have any value if the page is in memory, but it is wise
     * to keep the offset value. */
    uint32_t offset;
    /* Bytes to read from executable. It can have any value if the page
     * is in swap, in memory, or not-yet-allocated stack. */
    size_t read_bytes;
    struct hash_elem elem;
  };

bool init_suppl_page_table (struct thread * holder);
struct page * add_suppl_page (struct page * src);
void delete_suppl_page_table (struct thread * holder);
bool load_page (struct page * spg);
void lock_suppl_page_table (struct thread * holder);
void lock_pagedir (struct thread * holder);
void unlock_suppl_page_table (struct thread * holder);
void unlock_pagedir (struct thread * holder);
void modify_suppl_page (struct page * spg, enum page_status status,
                        uint32_t offset);
struct page * search_suppl_page (struct thread * holder, void * address);

#endif  /* VM_PAGE_H */
