#include "vm/frame.h"
#include <hash.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

/* Data structure to store information about frames. */
struct frame
  {
    void * address;   /* the kernel virtual address of the frame */
    struct thread * holder;
    struct hash_elem elem;
  };

static struct hash frame_table;
static struct lock frame_lock;

static bool add_frame (void * address);
static void delete_frame (void * address);

static unsigned
frame_hash (const struct hash_elem * elem, void * aux UNUSED)
{
  const struct frame * elem_fr = hash_entry (elem, struct frame, elem);
  return hash_bytes (&elem_fr->address, sizeof (elem_fr->address));
}

static bool
frame_less (const struct hash_elem * a, const struct hash_elem * b,
            void * aux UNUSED)
{
  const struct frame * a_fr = hash_entry (a, struct frame, elem);
  const struct frame * b_fr = hash_entry (b, struct frame, elem);
  return a_fr->address < b_fr->address;
}

void
init_frame (void)
{
  ASSERT (hash_init (&frame_table, frame_hash, frame_less, NULL));
  lock_init (&frame_lock);
}

static bool
add_frame (void * address)
{
  struct frame * fr = (struct frame *)(malloc (sizeof (struct frame)));
  struct hash_elem * result;
  if (fr == NULL) return false;
  fr->address = address;
  fr->holder = thread_current ();
  lock_acquire (&frame_lock);
  result = hash_insert (&frame_table, &fr->elem);
  ASSERT (result == NULL);
  lock_release (&frame_lock);
  return true;
}

static void
delete_frame (void * address)
{
  struct frame fr;
  struct hash_elem * elem;
  fr.address = address;
  lock_acquire (&frame_lock);
  elem = hash_delete (&frame_table, &fr.elem);
  ASSERT (elem != NULL);
  lock_release (&frame_lock);
  free (hash_entry (elem, struct frame, elem));
}

bool
add_frames (void * address_, size_t cnt)
{
  size_t i;
  uint8_t * address = address_;
  bool result;
  for (i = 0; i < cnt; ++i)
    {
      result = add_frame (address + i * PGSIZE);
      if (!result) break;
    }
  if (i < cnt)
    {
      /* Error occurred. Delete all the frames we have inserted in the
      * table. */
      delete_frame (address + i * PGSIZE);
      while (i > 0)
      {
        --i;
        delete_frame (address + i * PGSIZE);
      }
      return false;
    }
  return true;
}

void
delete_frames (void * address_, size_t cnt)
{
  size_t i;
  uint8_t * address = address_;
  for (i = 0; i < cnt; ++i)
    delete_frame (address + i * PGSIZE);
}

