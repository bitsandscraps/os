#include "vm/frame.h"
#include <hash.h>
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static struct hash frame_table;
static struct lock frame_lock;

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
  hash_init (&frame_table, frame_hash, frame_less, NULL);
  lock_init (&frame_lock);
}

bool
add_frame (struct thread * holder, void * address, void * vaddr)
{
  struct frame * fr = (struct frame *)(malloc (sizeof (struct frame)));
  struct hash_elem * result;
  if (fr == NULL) return false;
  fr->address = address;
  fr->holder = holder;
  fr->vaddr = vaddr;
  lock_acquire (&frame_lock);
  result = hash_insert (&frame_table, &fr->elem);
  ASSERT (result == NULL);
  lock_release (&frame_lock);
  return true;
}

void
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

struct frame *
frame_to_evict (void)
{
  struct hash_iterator i;
  struct frame * victim;
  hash_first (&i, &frame_table);
  while (hash_next (&i))
    {
      victim = hash_entry (hash_cur (&i), struct frame, elem);
    }
}
