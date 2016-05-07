#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdbool.h>
#include "threads/thread.h"
#include "vm/page.h"

void init_swap (void);
bool swap_in (struct thread * holder, struct page * spg, void * pg);
void * swap_out (void);

#endif  /* VM_SWAP_H */
