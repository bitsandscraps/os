#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <stdbool.h>
#include <stddef.h>
#include "devices/disk.h"
#include "filesys/off_t.h"

#define END_OF_FILE (disk_sector_t)(0)

void buffer_cache_init (void);
void buffer_cache_remove (disk_sector_t sector);
bool buffer_cache_read (disk_sector_t sector, disk_sector_t next, off_t offset,
                        size_t length, void * buffer);
bool buffer_cache_write (disk_sector_t sector, off_t offset, size_t length,
                         const void * buffer, bool zero);
void buffer_cache_done (void);

#endif  /* FILESYS_CACHE_H */
