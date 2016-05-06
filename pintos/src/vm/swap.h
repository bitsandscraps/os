#ifndef VM_SWAP_H
#define VM_SWAP_H

void init_swap (void);
void swap_in (disk_sector_t index, void * page);
bool swap_out (void * page);

#endif  /* VM_SWAP_H */
