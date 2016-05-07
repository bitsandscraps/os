#include "vm/page.h"
#include <stdio.h>
#include "threads/malloc.h"

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
init_suppl_page_table (struct thread * holder)
{
  return hash_init (&holder->suppl_page_table, page_hash, page_less, NULL);
}

void
delete_suppl_page_table (struct thread * holder)
{
  lock_acquire (&holder->suppl_page_table_lock);
  hash_destroy (&holder->suppl_page_table, page_free);
  lock_release (&holder->suppl_page_table_lock);
}

bool
add_suppl_page (struct thread * holder, void * address,
                uint32_t offset, size_t read_bytes, bool isswap)
{
  struct page * spg = (struct page *)(malloc (sizeof (struct page)));
  struct hash_elem * elem;
  if (spg == NULL) return false;
  spg->address = address;
  spg->offset = offset;
  spg->read_bytes = read_bytes;
  spg->isswap = isswap;
  lock_acquire (&holder->suppl_page_table_lock);
  elem = hash_insert (&holder->suppl_page_table, &spg->elem);
  ASSERT (elem == NULL);
  lock_release (&holder->suppl_page_table_lock);
  return true;
}

void
delete_suppl_page (struct thread * holder, void * address)
{
  struct page pg;
  struct hash_elem * elem;
  pg.address = address;
  lock_acquire (&holder->suppl_page_table_lock);
  elem = hash_delete (&holder->suppl_page_table, &pg.elem);
  ASSERT (elem != NULL);
  lock_release (&holder->suppl_page_table_lock);
  free (hash_entry (elem, struct page, elem));
}

struct page *
search_suppl_page (struct thread * holder, void * address)
{
  struct page spg;
  spg.address = address;
  struct hash_elem * elem = hash_find (&holder->suppl_page_table, &spg.elem);
  if (elem == NULL) return NULL;
  return hash_entry (elem, struct page, elem);
}
