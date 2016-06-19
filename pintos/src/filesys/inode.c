#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include <stdio.h>
#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Number of inode pointers. */
#define DIRECT_BLOCKS 120
#define SINGLY_INDIRECT_BLOCKS 4
/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* On-disk inode.
   Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    uint32_t type;                      /* Type of file.(enum inode_type) */
    off_t length;                       /* File size in bytes. */
    disk_sector_t block_direct[DIRECT_BLOCKS];
    disk_sector_t block_singly[SINGLY_INDIRECT_BLOCKS];
    disk_sector_t block_doubly;
    unsigned magic;                     /* Magic number. */
  };

/* On-disk inode for FREE_MAP_SECTOR.
 * Must be exactly DISK_SECTOR_SIZE bytes long. */
struct inode_disk_0
  {
    disk_sector_t start;                /* First data sector. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
  };

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, DISK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    disk_sector_t sector;               /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct lock mutex;                  /* Mutex for metadata. */
  };

/* Set the length of INODE_ to LENGTH. */
static bool
inode_set_length (const struct inode * inode_, off_t length)
{
  disk_sector_t inode = inode_->sector;
  off_t offset;
  if (inode == FREE_MAP_SECTOR)
    offset = sizeof (disk_sector_t);
  else
    offset = sizeof (uint32_t);
  return buffer_cache_write (inode, offset, sizeof (&length), &length, false);
}

/* Returns the type of inode. Possible values are given as enum inode_type. */
uint32_t
inode_get_type (const struct inode * inode)
{
  uint32_t type;
  if (!buffer_cache_read (inode->sector, END_OF_FILE, 0, sizeof (type), &type))
    return TYPE_ERROR;
  return type;
}

static void
inode_set_type (const struct inode * inode, uint32_t type)
{
  buffer_cache_write (inode->sector, 0, sizeof (type), &type, false);
}

/* Reads the sector value pointed by POS of SECTOR. When ALLOC is true,
 * allocate a free block and write it to POS if the corresponding block
 * is not yet allocated. If not, just returns 0 when there is no corres-
 * ponding block. Returns the sector value when succeeded. Otherwise
 * return -1. */
static disk_sector_t
read_sector (disk_sector_t sector, off_t pos, bool alloc)
{
  disk_sector_t result;
  if (!buffer_cache_read (sector, END_OF_FILE, pos, sizeof (result), &result))
    return -1;
  if (result > 0) return result;
  /* Sector not yet allocated. */
  if (!alloc) return 0;
  if (!free_map_allocate (1, &result)) return -1;
  if (!buffer_cache_write (sector, pos, sizeof (result), &result, false))
    return -1;
  if (!buffer_cache_write (result, 0, 0, NULL, true))     /* Zero out. */
    return -1;
  return result;
}

/* Returns the disk sector that contains byte offset POS within
 * INODE. If ALLOC is set to true, it allocates a sector if the sector
 * is not yet allocated. Otherwise it returns 0 if the corresponding
 * sector is not yet allocated. Returns -1 when error occurs.*/
static disk_sector_t
byte_to_sector (disk_sector_t inode, off_t pos, bool alloc) 
{
  /* Need to handle FREE_MAP_SECTOR in a special way. */
  if (inode == FREE_MAP_SECTOR)
  {
    struct inode_disk_0 inode_disk;
    if (buffer_cache_read (FREE_MAP_SECTOR, END_OF_FILE, 0,
                            sizeof (inode_disk), &inode_disk))
    {
      if (pos < inode_disk.length)
        return inode_disk.start + pos / DISK_SECTOR_SIZE;
      if (!alloc) return 0;
    }
    return -1;
  }
  /* Number of pointers to sectors a disk can hold. */
  const size_t num_sectors = DISK_SECTOR_SIZE / sizeof (disk_sector_t);
  /* POS is in INDEX-th sector of the file. */
  size_t index = pos / DISK_SECTOR_SIZE;
  /* first field is type, second field is inode->length */
  off_t offset = sizeof (uint32_t) + sizeof (size_t);
  if (index < DIRECT_BLOCKS)              /* in direct block range */
  {
    offset += sizeof (disk_sector_t) * index;
    return read_sector (inode, offset, alloc);
  }
  index -= DIRECT_BLOCKS;
  offset += DIRECT_BLOCKS * sizeof (disk_sector_t);
  /* index of the sector inside the indirect block. */
  size_t subindex = index % num_sectors;
  /* On which indirect block does the pointer to the desired block
   * resides. */
  index /= num_sectors;
  if (index < SINGLY_INDIRECT_BLOCKS)     /* in singly-indirect block range */
  {
    offset += index * sizeof (disk_sector_t);
    /* Pointer to the corresponding singly-indirect block. */
    disk_sector_t pointer = read_sector (inode, offset, alloc);
    if (pointer < 1) return pointer;
    offset = subindex * sizeof (disk_sector_t);
    return read_sector (pointer, offset, alloc);
  }
  /* In doubly-indirect block. */
  index -= SINGLY_INDIRECT_BLOCKS;
  offset += SINGLY_INDIRECT_BLOCKS * sizeof (disk_sector_t);
  /* File too large to be handled by file system. */
  if (index >= num_sectors) return alloc ? -1 : 0;
  /* Pointer to the doubly-indirect block. */
  disk_sector_t pointer = read_sector (inode, offset, alloc);
  if (pointer < 1) return pointer;      /* Read Sector Error */
  offset = index * sizeof (disk_sector_t);
  /* Pointer to the corresponding singly-indirect block. */
  pointer = read_sector (pointer, offset, alloc);
  if (pointer < 1) return pointer;      /* Read Sector Error */
  offset = subindex * sizeof (disk_sector_t);
  return read_sector (pointer, offset, alloc);
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;
/* Lock that should be acquired before accessing OPEN_INODES. */
static struct lock open_inodes_lock;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init (&open_inodes_lock);
}

/* Initializes an inode with LENGTH bytes and TYPE type of data and
   writes the new inode to sector SECTOR on the file system
   disk.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (disk_sector_t sector, off_t length, uint32_t type)
{
  if (sector == FREE_MAP_SECTOR)
  {
    struct inode_disk_0 disk_inode;
    size_t sectors = bytes_to_sectors (length);
    disk_inode.length = length;
    disk_inode.magic = INODE_MAGIC;
    if (free_map_allocate (sectors, &disk_inode.start))
    {
      if (!buffer_cache_write (FREE_MAP_SECTOR, 0, sizeof (disk_inode),
                               &disk_inode, true))
        return false;
      if (sectors > 0)
      {
        size_t i;
        for (i = 0; i < sectors; ++i)
          if (!buffer_cache_write (disk_inode.start, 0, 0, NULL, true))
            return false;
      }
      return true;
    }
    return false;
  }

  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == DISK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->type = type;
      disk_inode->magic = INODE_MAGIC;
      if (buffer_cache_write (sector, 0, DISK_SECTOR_SIZE, disk_inode, false))
        success = true;
      /* Other contents are lazily loaded. */
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (disk_sector_t sector) 
{
  struct list_elem *e;
  struct inode *inode;

  lock_acquire (&open_inodes_lock);
  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          lock_release (&open_inodes_lock);
          inode_reopen (inode);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
  {
    lock_release (&open_inodes_lock);
    return NULL;
  }

  /* Initialize. */
  lock_init (&inode->mutex);
  inode->sector = sector;
  inode->removed = false;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  list_push_front (&open_inodes, &inode->elem);
  lock_release (&open_inodes_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  struct inode * result = inode;
  if (inode != NULL)
  {
    inode_lock (inode);
    if (inode->removed)
      result = NULL;
    else
      result->open_cnt++;
    inode_unlock (inode);
  }
  return result;
}

/* Returns INODE's inode number. */
disk_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  lock_acquire (&open_inodes_lock);
  inode_lock (inode);

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);
      lock_release (&open_inodes_lock);
 
      /* Deallocate blocks if removed. */
      if (inode->removed) 
        {
          size_t length = inode_length (inode);
          size_t pos;
          disk_sector_t sector;
          bool success = true;
          for (pos = 0; pos < length; pos += DISK_SECTOR_SIZE)
          {
            sector = byte_to_sector (inode->sector, pos, false);
            if ((int)sector == -1)
            {
              success = false;
              break;
            }
            if (sector > 0)
            {
              buffer_cache_remove (sector);
              free_map_release (sector, 1);
            }
          }
          buffer_cache_remove (inode->sector);
          free_map_release (inode->sector, 1);
        }
      inode_unlock (inode);
      free (inode); 
    }
  else
    {
      lock_release (&open_inodes_lock);
      inode_unlock (inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode_lock (inode);
  inode->removed = true;
  inode_set_type (inode, TYPE_ERROR);
  inode_unlock (inode);
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode_, void *buffer_, off_t size, off_t offset)
{
  off_t bytes_read = 0;
  uint8_t * buffer = buffer_;
  disk_sector_t inode = inode_->sector;

  while (size > 0) 
    {
      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      int sector_ofs = offset % DISK_SECTOR_SIZE;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      inode_lock (inode_);
      off_t inode_len = inode_length (inode_);
      off_t inode_left = inode_len > offset ? inode_len - offset : 0;
      int min_left = inode_left < sector_left ? inode_left : sector_left;
      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
      {
        inode_unlock (inode_);
        break;
      }
      /* Disk sector to read, starting byte offset within sector. */
      disk_sector_t sector_idx = byte_to_sector (inode, offset, true);
      /* If 0, there is no data. If -1, error has occurred. */
      if ((int)sector_idx < 1)
      {
        inode_unlock (inode_);
        break;
      }
      disk_sector_t sector_next;
      sector_next = byte_to_sector (inode, offset + DISK_SECTOR_SIZE, false);
      if ((int)sector_next == -1)
      {
        inode_unlock (inode_);
        break;
      }
      inode_unlock (inode_);

      if (!buffer_cache_read (sector_idx, sector_next, sector_ofs, chunk_size,
                              buffer + bytes_read))
        break;
      
      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      inode_left -= chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode_, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  disk_sector_t inode = inode_->sector;

  if (inode_->deny_write_cnt)
    return 0;

  inode_lock (inode_);
  off_t length = inode_length (inode_);
  inode_unlock (inode_);
  if (length < 0) return 0;

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      inode_lock (inode_);
      disk_sector_t sector_idx = byte_to_sector (inode, offset, true);
      inode_unlock (inode_);
      if ((int)sector_idx == -1) break;
      int sector_ofs = offset % DISK_SECTOR_SIZE;
      int sector_left = DISK_SECTOR_SIZE - sector_ofs;
      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < sector_left ? size : sector_left;
      if (chunk_size <= 0)
        break;

      if (!buffer_cache_write (sector_idx, sector_ofs, chunk_size,
                               buffer + bytes_written, false))
        break;

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
      /* Update length. */
      inode_lock (inode_);
      length = inode_length (inode_);
      if (length < offset)
        inode_set_length (inode_, offset);
      inode_unlock (inode_);
    }

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode) 
{
  inode_lock (inode);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode_unlock (inode);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode) 
{
  inode_lock (inode);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  inode_unlock (inode);
}

/* Returns the length, in bytes, of INODE's data. Returns -1 when error
 * occurs.*/
off_t
inode_length (const struct inode *inode)
{
  off_t length;
  off_t offset;
  if (inode->sector == FREE_MAP_SECTOR)
    offset = sizeof (disk_sector_t);
  else
    offset = sizeof (uint32_t);
  if (!buffer_cache_read (inode->sector, END_OF_FILE, offset, sizeof (length),
                          &length))
    return -1;
  return length;
}

void
inode_lock (struct inode * inode)
{
  lock_acquire (&inode->mutex);
}

void
inode_unlock (struct inode * inode)
{
  lock_release (&inode->mutex);
}

bool
inode_is_opened (struct inode * inode)
{
  inode_lock (inode);
  bool result = (inode->open_cnt != 1);
  inode_unlock (inode);
  return result;
}

