#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "threads/thread.h"

struct page
  {
    void * address;       /* user virtual address of the page. */
    bool isswap;          /* true if it is in swap space. false if code. */
    uint32_t offset;
    size_t read_bytes;
    struct hash_elem elem;
  };

bool init_suppl_page_table (struct thread * holder);
bool add_suppl_page (struct thread * holder, void * uaddr,
                     uint32_t offset, size_t read_bytes, bool isswap);
void delete_suppl_page_table (struct thread * holder);
void delete_suppl_page (struct thread * holder, void * address);
struct page * search_suppl_page (struct thread * holder, void * address);

#endif  /* VM_PAGE_H */
