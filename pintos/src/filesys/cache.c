#include "filesys/cache.h"
#include <list.h>
#include <string.h>
#include <stdio.h>
#include "devices/timer.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"

#define BUFFER_CACHE_LIMIT 64

/* Buffer cache element. */
struct buf_elem
  {
    disk_sector_t sector;                   /* Number of the sector. */
    bool is_dirty;                          /* True if modified. */
    /* Set to false when the cache is added to list and data is being
     * copied from disk. */
    bool is_ready;
    /* Set to true when it is removed. */
    bool is_removed;
    /* Number of threads reading or writing this block. A cache element
     * can be evicted only if holders is 0. */
    int holders;
    struct lock mutex;                      /* Mutex for metadata. */
    /* Mutex to avoid write race conditions. */
    struct lock write_lock;
    /* Condition variable to signal that data is completely copied from
     * disk. */
    struct condition data_ready;
    uint8_t data[DISK_SECTOR_SIZE];         /* Actual content. */
    struct list_elem elem;                  /* List element. */
  };

/* Data structure for read ahead. */
struct read_ahead_elem
  {
    disk_sector_t sector;                   /* Sector to read. */
    struct list_elem elem;                  /* List element. */
  };

/* Number of buffer caches. */
static unsigned int buffer_cache_cnt;
/* Buffer cache list. */
static struct list buffer_cache;
/* Mutex for buffer cache list. */
static struct lock buffer_cache_lock;
/* Circular list head. */
static struct list_elem * buffer_cache_curr;

/* Read ahead list. */
static struct list read_ahead;
/* Mutex for read ahead list. */
static struct lock read_ahead_lock;
/* Conditional variable to signal read ahead daemon that there may be
 * some blocks to read. */
static struct condition read_ahead_cond;
/* Set to true when filesys_done is called. Read ahead daemon will
 * terminate. */
static bool is_read_ahead_done;
/* To ensure read daemon has completed before pintos shutdowns. */
static struct semaphore read_daemon_sema;

/* Set to true when filesys_done is called. Write behind daemon will
 * terminate. */
static bool is_write_behind_done;
/* Conditional variable to signal it is time to write behind. */
static struct condition write_daemon_cond;
/* To ensure write daemon has completed before pintos shutdown. */
static struct semaphore write_daemon_sema;

static void read_ahead_daemon (void * aux UNUSED);
static void write_behind_daemon (void * aux UNUSED);
static void timer_daemon (void * aux UNUSED);
static struct buf_elem * buf_elem_init (disk_sector_t sector, bool hold);
static void buffer_cache_epilogue (struct buf_elem * target);
static struct buf_elem * buffer_cache_evict (disk_sector_t sector, bool hold);
static struct buf_elem * buffer_cache_find (disk_sector_t sector, bool hold);

void
buffer_cache_init (void)
{
  list_init (&buffer_cache);
  lock_init (&buffer_cache_lock);
  list_init (&read_ahead);
  lock_init (&read_ahead_lock);
  cond_init (&read_ahead_cond);
  sema_init (&read_daemon_sema, 0);
  sema_init (&write_daemon_sema, 0);
  cond_init (&write_daemon_cond);
  is_read_ahead_done = false;
  is_write_behind_done = false;
  buffer_cache_curr = list_begin (&buffer_cache);
  buffer_cache_cnt = 0;
  thread_create ("timer_daemon", PRI_DEFAULT, timer_daemon, NULL);
  thread_create ("read_ahead_daemon", PRI_DEFAULT, read_ahead_daemon, NULL);
  thread_create ("write_behind_daemon", PRI_DEFAULT, write_behind_daemon, NULL);
}

/* Wakes up every 5 seconds to signal write behind. */
static void
timer_daemon (void * aux UNUSED)
{
  lock_acquire (&buffer_cache_lock);
  while (!is_write_behind_done)
  {
    lock_release (&buffer_cache_lock);
    timer_msleep (5000);
    lock_acquire (&buffer_cache_lock);
    cond_signal (&write_daemon_cond, &buffer_cache_lock);
  }
  lock_release (&buffer_cache_lock);
}

/* If there is blocks to read ahead, perform read ahead. If not sleep. */
static void
read_ahead_daemon (void * aux UNUSED)
{
  struct read_ahead_elem * target;
  disk_sector_t sector;
  lock_acquire (&read_ahead_lock);
  while (!is_read_ahead_done)
  {
    cond_wait (&read_ahead_cond, &read_ahead_lock);
    while (!list_empty (&read_ahead))
    {
      target = list_entry (list_pop_front (&read_ahead),
                           struct read_ahead_elem, elem);
      lock_release (&read_ahead_lock);
      sector = target->sector;
      free (target);
      /* Read ahead daemon does not perform any modification on cache,
       * so it does not need to acquire lock on cache. */
      buffer_cache_find (sector, false);
      lock_acquire (&read_ahead_lock);
    }
  }
  /* Signal buffer_cache_done that it has completed its process. */
  lock_release (&read_ahead_lock);
  sema_up (&read_daemon_sema);
}

/* Most of the time this thread just sleeps. When it is awaken by some
 * other thread(usually timer daemon) it writes dirty blocks to disk. */
static void
write_behind_daemon (void * aux UNUSED)
{
  struct buf_elem * curr;
  struct list_elem * e;
  lock_acquire (&buffer_cache_lock);
  while (!is_write_behind_done)
  {
    /* buffer_cache_lock acquired before traversing the list. */
    e = list_begin (&buffer_cache);
    while (e != list_end (&buffer_cache))
    {
      curr = list_entry (e, struct buf_elem, elem);
      lock_acquire (&curr->mutex);
      lock_release (&buffer_cache_lock);
      if (curr->is_ready && curr->is_dirty)
      {
        curr->is_dirty = false;
        lock_acquire (&curr->write_lock);
        lock_release (&curr->mutex);
        disk_write (filesys_disk, curr->sector, &curr->data[0]);
        lock_release (&curr->write_lock);
      }
      else
        lock_release (&curr->mutex);
      lock_acquire (&buffer_cache_lock);
      e = list_next (e);
    }
    cond_wait (&write_daemon_cond, &buffer_cache_lock);
  }
  /* Signal buffer_cache_done that it has completed its process. */
  lock_release (&buffer_cache_lock);
  sema_up (&write_daemon_sema);
}

/* Create an initialize a buffer cache element associated with sector
 * number SECTOR. If HOLD is set to true, the new buffer cache element's
 * holder will be set to 1. */
static struct buf_elem *
buf_elem_init (disk_sector_t sector, bool hold)
{
  struct buf_elem * new = malloc (sizeof (struct buf_elem));
  if (new == NULL) return NULL;
  new->sector = sector;
  new->is_dirty = false;
  new->is_ready = false;
  new->is_removed = false;
  lock_init (&new->mutex);
  lock_init (&new->write_lock);
  cond_init (&new->data_ready);
  new->holders = hold ? 1 : 0;
  ++buffer_cache_cnt;
  list_push_back (&buffer_cache, &new->elem);
  return new;
}

void
buffer_cache_done (void)
{
  struct list_elem * e;
  /* Signal read ahead daemon to terminate. */
  lock_acquire (&read_ahead_lock);
  is_read_ahead_done = true;
  cond_signal (&read_ahead_cond, &read_ahead_lock);
  lock_release (&read_ahead_lock);
  /* Wait for read ahead daemon to terminate. */
  sema_down (&read_daemon_sema);
  lock_acquire (&read_ahead_lock);
  /* Just in case there are some elements read ahead daemon did not
   * free. */
  while (!list_empty (&read_ahead))
  {
    e = list_pop_front (&read_ahead);
    struct read_ahead_elem * curr;
    curr = list_entry (e, struct read_ahead_elem, elem);
    free (curr);
  }
  lock_release (&read_ahead_lock);
  /* Signal write behind daemon to terminate. */
  lock_acquire (&buffer_cache_lock);
  is_write_behind_done = true;
  cond_signal (&write_daemon_cond, &buffer_cache_lock);
  lock_release (&buffer_cache_lock);
  /* Wait for write behind daemon to terminate. */
  sema_down (&write_daemon_sema);
  /* Write all the dirty cache elements to disk. */
  lock_acquire (&buffer_cache_lock);
  while (!list_empty (&buffer_cache))
  {
    e = list_pop_front (&buffer_cache);
    struct buf_elem * curr = list_entry (e, struct buf_elem, elem);
    if (curr->is_dirty)
      disk_write (filesys_disk, curr->sector, &curr->data[0]);
  }
  lock_release (&buffer_cache_lock);
}

/* Mark the cache element corresponding to sector number SECTOR. It is
 * actually removed from list when it is evicted. */
void
buffer_cache_remove (disk_sector_t sector)
{
  struct buf_elem * target;
  struct list_elem * e;
  lock_acquire (&buffer_cache_lock);
  for (e = list_begin (&buffer_cache); e != list_end (&buffer_cache);
       e = list_next (e))
  {
    target = list_entry (e, struct buf_elem, elem);
    lock_acquire (&target->mutex);
    if (target->sector == sector)
    {
      /* Just marks. */
      target->is_removed = true;
      lock_release (&target->mutex);
      break;
    }
    lock_release (&target->mutex);
  }
  lock_release (&buffer_cache_lock);
}

/* Decrement the holder value of TARGET. */
static void
buffer_cache_epilogue (struct buf_elem * target)
{
  ASSERT (target != NULL);
  lock_acquire (&target->mutex);
  ASSERT (--target->holders >= 0)
  lock_release (&target->mutex);
}


/* Reads the buffer cache corresponding to sector number SECTOR and
 * copies LENGTH bytes starting from OFFSET to BUFFER. If NEXT is not
 * END_OF_FILE, it will signal to read ahead daemon to read ahead that
 * sector. Returns false if read fails. */
bool
buffer_cache_read (disk_sector_t sector, disk_sector_t next, off_t offset,
                   size_t length, void * buffer)
{
  ASSERT (offset + length <= DISK_SECTOR_SIZE);
  struct buf_elem * cache = buffer_cache_find (sector, true);
  if (cache == NULL) return false;
  lock_release (&cache->mutex);     /* Acquired by buffer_cache_find. */
  memcpy (buffer, &cache->data[offset], length);
  buffer_cache_epilogue (cache);
  lock_acquire (&read_ahead_lock);
  if ((next != END_OF_FILE) && !is_read_ahead_done)
  {
    struct read_ahead_elem * new = malloc (sizeof (struct read_ahead_elem));
    if (new != NULL)
    {
      new->sector = next;
      list_push_back (&read_ahead, &new->elem);
      /* Signal read_ahead_daemon that there is a block to read. */
      cond_signal (&read_ahead_cond, &read_ahead_lock);
    }
  }
  lock_release (&read_ahead_lock);
  return true;
}

/* Writes LENGTH bytes of BUFFER to the cache corresponding to sector
 * number SECTOR starting from OFFSET bytes. If ZERO is set to true,
 * all the reamining data will be set to zero. Returns false when an
 * error occurs. */
bool
buffer_cache_write (disk_sector_t sector, off_t offset, size_t length,
                    const void * buffer, bool zero)
{
  ASSERT (offset + length <= DISK_SECTOR_SIZE);
  struct buf_elem * cache = buffer_cache_find (sector, true);
  if (cache == NULL) return false;
  cache->is_dirty = true;           /* Write makes cache dirty. */
  lock_acquire (&cache->write_lock);
  lock_release (&cache->mutex);     /* Acquried by buffer_cache_find. */
  memcpy (&cache->data[offset], buffer, length);
  off_t zero_off = offset + length;
  if (zero)
    memset (&cache->data[zero_off], 0, DISK_SECTOR_SIZE - zero_off);
  lock_release (&cache->write_lock);
  buffer_cache_epilogue (cache);
  return true;
}

/* Adds a struct buf_elem corresponding to SECTOR to buffer_cache. Must
 * be called after acquiring buffer_cache_lock. The lock is released by
 * this function. Returns the element added to buffer cache list. */
static struct buf_elem *
buffer_cache_add (disk_sector_t sector, bool hold)
{
  struct buf_elem * new;
  if (buffer_cache_cnt < BUFFER_CACHE_LIMIT)
  {
    new = buf_elem_init (sector, hold);
    lock_release (&buffer_cache_lock);
  }
  else
  {
    /* buffer_cache_lock released by buffer_cache_evict. */
    new = buffer_cache_evict (sector, hold);
  }
  disk_read (filesys_disk, sector, &new->data[0]);
  lock_acquire (&new->mutex);
  new->is_ready = true;
  /* There may be multiple processes waiting for this sector. */
  cond_broadcast (&new->data_ready, &new->mutex);
  if (!hold)
    lock_release (&new->mutex);
  return new;
}

/* Evicts a buffer cache element from buffer cache list and change that
 * element to have sector number SECTOR. If HOLD is set to true, set
 * holder to 1 and acquire mutex. Returns the element. */
static struct buf_elem *
buffer_cache_evict (disk_sector_t sector, bool hold)
{
  struct buf_elem * victim;
  disk_sector_t old_sector;
  bool is_dirty;
  while (true)
  {
    /* Circular list implementation. */
    if (buffer_cache_curr == list_end (&buffer_cache))
      buffer_cache_curr = list_begin (&buffer_cache);
    victim = list_entry (buffer_cache_curr, struct buf_elem, elem);
    buffer_cache_curr = list_next (buffer_cache_curr);
    lock_acquire (&victim->mutex);
    if (victim->holders == 0 && victim->is_ready)
    {
      old_sector = victim->sector;
      victim->holders = hold ? 1 : 0;
      victim->sector = sector;
      victim->is_ready = false;
      is_dirty = victim->is_dirty;
      victim->is_dirty = false;
      if (victim->is_removed)
      {
        victim->is_removed = false;
        lock_release (&buffer_cache_lock);
        lock_release (&victim->mutex);
        return victim;
      }
      break;
    }
    lock_release (&victim->mutex);
  }
  lock_release (&buffer_cache_lock);
  if (is_dirty)
  {
    /* Write if dirty. */
    lock_acquire (&victim->write_lock);
    lock_release (&victim->mutex);
    disk_write (filesys_disk, old_sector, &victim->data[0]);
    lock_release (&victim->write_lock);
  }
  else
    lock_release (&victim->mutex);
  return victim;
}

/* Retruns the buf_elem corresponding to SECTOR. If HOLD is true,
 * increments the buf_elem's holders value and acquires the correponding
 * mutex. Acquires BUFER_CACHE_LOCK and corresponding MUTEX. */
static struct buf_elem *
buffer_cache_find (disk_sector_t sector, bool hold)
{
  struct buf_elem * target;
  struct list_elem *e;
  lock_acquire (&buffer_cache_lock);
  for (e = list_begin (&buffer_cache); e != list_end (&buffer_cache);
       e = list_next (e))
  {
    target = list_entry (e, struct buf_elem, elem);
    lock_acquire (&target->mutex);
    lock_release (&buffer_cache_lock);
    if (target->sector == sector)
    {
      if (hold)
        ++target->holders;
      while (!target->is_ready)
        cond_wait (&target->data_ready, &target->mutex);
      if (!hold)
        lock_release (&target->mutex);
      return target;
    }
    lock_release (&target->mutex);
    lock_acquire (&buffer_cache_lock);
  }
  /* BUFFER_CACHE_ADD requires BUFFER_CACHE_LOCK to be acquired. It
   * releases the lock. */
  return buffer_cache_add (sector, hold);
}

