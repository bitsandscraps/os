#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <hash.h>
#include "devices/disk.h"
#include "threads/synch.h"

bool init_suppl_page_table (struct hash * spt);
bool add_suppl_page (struct hash * spt, struct lock * mutex,
                     void * uaddr, disk_sector_t index);
void delete_suppl_page (struct hash * spt, struct lock * mutex, void * uaddr);

#endif  /* VM_PAGE_H */
