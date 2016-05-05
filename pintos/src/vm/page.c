#include "vm/page.h"
#include "threads/malloc.h"

struct page
  {
    void * address;       /* user virtual address of the page. */
    disk_sector_t index;
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

bool
init_suppl_page_table (struct hash * spt)
{
  return hash_init (spt, page_hash, page_less, NULL);
}

bool
add_suppl_page (struct hash * spt, struct lock * mutex,
                void * uaddr, disk_sector_t index)
{
  struct page * spg = (struct page *)(malloc (sizeof (struct page)));
  struct hash_elem * elem;
  if (spg == NULL) return false;
  spg->address = uaddr;
  spg->index = index;
  lock_acquire (mutex);
  elem = hash_insert (spt, &spg->elem);
  lock_release (mutex);
  ASSERT (elem == NULL);
  return true;
}

void
delete_suppl_page (struct hash * spt, struct lock * mutex, void * uaddr)
{
  struct page spg;
  struct hash_elem * elem;
  spg.address = uaddr;
  lock_acquire (mutex);
  elem = hash_delete (spt, &spg.elem);
  lock_release (mutex);
  ASSERT (elem != NULL);
  free (hash_entry (elem, struct page, elem));
}

