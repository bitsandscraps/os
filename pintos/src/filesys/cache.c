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

struct buf_elem
  {
    disk_sector_t sector;
    bool is_dirty;
    bool is_ready;
    bool is_removed;
    int holders;
    struct lock mutex;
    struct lock write_lock;
    struct condition data_ready;
    uint8_t data[DISK_SECTOR_SIZE];
    struct list_elem elem;
  };

struct read_ahead_elem
  {
    disk_sector_t sector;
    struct list_elem elem;
  };

static unsigned int buffer_cache_cnt;
static struct list buffer_cache;
static struct lock buffer_cache_lock;
static struct list_elem * buffer_cache_curr;

static struct list read_ahead;
static struct lock read_ahead_lock;
static struct condition read_ahead_cond;
static bool is_read_ahead_done;
static struct semaphore read_daemon_sema;

static bool is_write_behind_done;
static struct condition write_daemon_cond;
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
  thread_create ("read_ahead_daemon", PRI_DEFAULT, read_ahead_daemon, NULL);
  thread_create ("write_behind_daemon", PRI_DEFAULT, write_behind_daemon, NULL);
  thread_create ("timer_daemon", PRI_DEFAULT, timer_daemon, NULL);
}

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

static void
read_ahead_daemon (void * aux UNUSED)
{
  struct read_ahead_elem * target;
  disk_sector_t sector;
  lock_acquire (&read_ahead_lock);
  while (!is_read_ahead_done)
  {
    cond_wait (&read_ahead_cond, &read_ahead_lock);
    if (!list_empty (&read_ahead))
    {
      target = list_entry (list_pop_front (&read_ahead),
                           struct read_ahead_elem, elem);
      lock_release (&read_ahead_lock);
      sector = target->sector;
      free (target);
      buffer_cache_find (sector, false);
      lock_acquire (&read_ahead_lock);
    }
  }
  /* Signal buffer_cache_done that it has completed its process. */
  lock_release (&read_ahead_lock);
  sema_up (&read_daemon_sema);
}

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
  lock_acquire (&read_ahead_lock);
  is_read_ahead_done = true;
  cond_signal (&read_ahead_cond, &read_ahead_lock);
  lock_release (&read_ahead_lock);
  sema_down (&read_daemon_sema);
  lock_acquire (&read_ahead_lock);
  while (!list_empty (&read_ahead))
  {
    e = list_pop_front (&read_ahead);
    struct read_ahead_elem * curr;
    curr = list_entry (e, struct read_ahead_elem, elem);
    free (curr);
  }
  lock_release (&read_ahead_lock);
  lock_acquire (&buffer_cache_lock);
  is_write_behind_done = true;
  cond_signal (&write_daemon_cond, &buffer_cache_lock);
  lock_release (&buffer_cache_lock);
  sema_down (&write_daemon_sema);
  lock_acquire (&buffer_cache_lock);
  while (!list_empty (&buffer_cache))
  {
    e = list_pop_front (&buffer_cache);
    struct buf_elem * curr = list_entry (e, struct buf_elem, elem);
    disk_write (filesys_disk, curr->sector, &curr->data[0]);
  }
  lock_release (&buffer_cache_lock);
}

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
      target->is_removed = true;
      lock_release (&target->mutex);
      break;
    }
    lock_release (&target->mutex);
  }
  lock_release (&buffer_cache_lock);
}

static void
buffer_cache_epilogue (struct buf_elem * target)
{
  ASSERT (target != NULL);
  lock_acquire (&target->mutex);
  ASSERT (--target->holders >= 0)
  lock_release (&target->mutex);
}


bool
buffer_cache_read (disk_sector_t sector, disk_sector_t next, off_t offset,
                   size_t length, void * buffer)
{
  ASSERT (offset + length <= DISK_SECTOR_SIZE);
  struct buf_elem * cache = buffer_cache_find (sector, true);
  if (cache == NULL) return false;
  lock_release (&cache->mutex);
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

bool
buffer_cache_write (disk_sector_t sector, off_t offset, size_t length,
                    const void * buffer, bool zero)
{
  ASSERT (offset + length <= DISK_SECTOR_SIZE);
  struct buf_elem * cache = buffer_cache_find (sector, true);
  if (cache == NULL) return false;
  cache->is_dirty = true;
  lock_acquire (&cache->write_lock);
  lock_release (&cache->mutex);
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
 * this function. */
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
  cond_broadcast (&new->data_ready, &new->mutex);
  if (!hold)
    lock_release (&new->mutex);
  return new;
}

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

