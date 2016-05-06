#include "vm/page.h"
#include "threads/malloc.h"

struct page
  {
    void * address;       /* user virtual address of the page. */
    bool isswap;          /* true if it is in swap space. false if code. */
    uint32_t offset;
    struct hash_elem elem;
  };


static unsigned
page_hash (const struct hash_elem * elem, void * aux UNUSED)
{
  const struct page * elem_pg = hash_entry (elem, struct page, elem);
  return hash_bytes (&elem_pg->address, sizeof (elem_pg->address));
}

static bool
page_less (const struct hash_elem * a, const struct hash_elem * b,
           void * aux UNUSED)
{
  const struct page * a_pg = hash_entry (a, struct page, elem);
  const struct page * b_pg = hash_entry (b, struct page, elem);
  return a_pg->address < b_pg->address;
}

static void
page_free (struct hash_elem * elem, void * aux UNUSED)
{
  struct page * elem_pg = hash_entry (elem, struct page, elem);
  free (elem_pg);
}

bool
init_suppl_page_table (struct hash * spt)
{
  return hash_init (spt, page_hash, page_less, NULL);
}

void
delete_suppl_page_table (struct hash * spt, struct lock * mutex)
{
  lock_acquire (mutex);
  hash_destroy (spt, page_free);
  lock_release (mutex);
}

bool
add_suppl_page (struct hash * spt, struct lock * mutex, void * address,
                uint32_t offset, bool isswap)
{
  struct page * spg = (struct page *)(malloc (sizeof (struct page)));
  struct hash_elem * elem;
  bool success;
  if (spg == NULL) return false;
  spg->address = uaddr;
  spg->offset = offset;
  spg->isswap = isswap;
  lock_acquire (mutex);
  elem = hash_insert (spt, &spg->elem);
  ASSERT (elem == NULL);
  lock_release (mutex);
  return success;
}

void
delete_suppl_page (struct hash * spt, struct lock * mutex, void * uaddr)
{
  struct page pg;
  struct hash_elem * elem;
  pg.address = address;
  lock_acquire (&frame_lock);
  elem = hash_delete (&frame_table, &fr.elem);
  ASSERT (elem != NULL);
  lock_release (&frame_lock);
  free (hash_entry (elem, struct frame, elem));
}
