#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/disk.h"

struct bitmap;
enum inode_type { TYPE_DIR, TYPE_FILE, TYPE_ERROR };

void inode_init (void);
bool inode_create (disk_sector_t, off_t, uint32_t);
struct inode *inode_open (disk_sector_t);
struct inode *inode_reopen (struct inode *);
disk_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_lock (struct inode *);
void inode_unlock (struct inode *);
void inode_dir_lock (struct inode *);
void inode_dir_unlock (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);
uint32_t inode_get_type (const struct inode *);
bool inode_is_opened (struct inode *);

#endif /* filesys/inode.h */
