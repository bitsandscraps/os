#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/thread.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "devices/disk.h"

/* The disk that contains the file system. */
struct disk *filesys_disk;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  filesys_disk = disk_get (0, 1);
  if (filesys_disk == NULL)
    PANIC ("hd0:1 (hdb) not present, file system initialization failed");

  inode_init ();
  free_map_init ();

  buffer_cache_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  buffer_cache_done ();
}

static bool
check_path (const char * path)
{
  char c, d = '\0';
  bool only_slash = true;
  if (*path == '\0') return false;          /* Reject empty name. */
  while ((c = *path++) != '\0')
  {
    d = c;
    if (c == '/') continue;
    only_slash = false;
  }
  return (d != '/') || only_slash;
}

static struct dir *
filesys_create_routine (const char * path_, char name[NAME_MAX + 1])
{
  ASSERT (path_ != NULL);
  if (!check_path (path_)) return NULL;          /* Not a valid path. */

  /* Initialization for strtok. */
  size_t length = strlen (path_) + 1;
  char * path = malloc (length);
  if (path == NULL) return NULL;
  strlcpy (path, path_, length);

  /* Starting directory. */
  struct inode * inode;
  if (*path_ == '/')
    inode = inode_open (ROOT_DIR_SECTOR);
  else
    inode = inode_open (thread_current ()->curr_dir_sector);
  if (inode == NULL)
  {
    free (path);
    return NULL;
  }

  char * token;
  char * saveptr;
  const char * delim = "/";
  bool is_dir = true;
  struct dir * dir = NULL;
  for (token = strtok_r (path, delim, &saveptr); token != NULL;
       token = strtok_r (NULL, delim, &saveptr))
  {
    if (is_dir)
      dir = dir_open (inode);
    else
      inode_close (inode);
    if (dir == NULL)
    {
      free (path);
      return NULL;
    }
    /* Two possibilities. A directory in the path does not exist. Or
     * we have arrived at the target. */
    if (!dir_lookup (dir, token, &inode, &is_dir))
      break;
    dir_close (dir);
    dir = NULL;
  }

  /* The file already exists. */
  if (token == NULL)
  {
    free (path);
    return NULL;
  }

  /* Check length of the target file name. */
  if (strlen (token) <= NAME_MAX)
    strlcpy (name, token, NAME_MAX + 1);      /* Name of the target file. */
  else
  {
    dir_close (dir);
    dir = NULL;
  }
  free (path);
  return dir;
}

/* Opens the inode with the given NAME. Retruns the new inode if
 * successful or a null pointer otherwise. Fails if no inode name
 * NAME exists, or if an internal memory allocation fails. If the
 * target is a directory, IS_DIR is set to true. */
struct inode *
filesys_find (const char * path_, bool * is_dir)
{
  ASSERT (path_ != NULL);
  if (!check_path (path_)) return NULL;          /* Not a valid path. */

  /* Initialization for strtok. */
  size_t length = strlen (path_) + 1;
  char * path = malloc (length);
  if (path == NULL) return NULL;
  strlcpy (path, path_, length);

  /* Starting directory. */
  struct inode * inode;
  if (*path_ == '/')
    inode = inode_open (ROOT_DIR_SECTOR);
  else
    inode = inode_open (thread_current ()->curr_dir_sector);

  char * token;
  char * saveptr;
  const char * delim = "/";
  *is_dir = true;
  struct dir * dir = NULL;
  for (token = strtok_r (path, delim, &saveptr); token != NULL;
       token = strtok_r (NULL, delim, &saveptr))
  {
    if (*is_dir)
      dir = dir_open (inode);
    else
      inode_close (inode);
    /* If lookup or open fails INODE is NULL so dir_open should fail. */
    if (dir == NULL)
    {
      inode = NULL;
      break;
    }
    dir_lookup (dir, token, &inode, is_dir);
    dir_close (dir);
    dir = NULL;
  }
  free (path);
  return inode;
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name_, off_t initial_size) 
{
  char name[NAME_MAX + 1];          /* Place to store name of file. */
  struct dir * dir = filesys_create_routine (name_, name);
  disk_sector_t inode_sector = 0;
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, TYPE_FILE)
                  && dir_add (dir, name, false, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  bool is_dir;
  return file_open (filesys_find (name, &is_dir));
}

bool
filesys_chdir (const char * name)
{
  bool is_dir;
  struct inode * inode = filesys_find (name, &is_dir);
  if (inode == NULL) return false;
  if (is_dir)
    thread_current ()->curr_dir_sector = inode_get_inumber (inode);
  inode_close (inode);
  return is_dir;
}

bool
filesys_mkdir (const char * name_)
{
  char name[NAME_MAX + 1];
  struct dir * dir = filesys_create_routine (name_, name);
  disk_sector_t inode_sector = 0;
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && dir_create (inode_sector,
                                 inode_get_inumber(dir_get_inode(dir)), 16)
                  && dir_add (dir, name, true, inode_sector));
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);
  dir_close (dir);
  return success;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char * name_) 
{
  ASSERT (name_ != NULL);
  if (!check_path (name_)) return NULL;          /* Not a valid path. */

  /* Initialization for strtok. */
  size_t length = strlen (name_) + 1;
  char * path = malloc (length);
  if (path == NULL) return NULL;
  strlcpy (path, name_, length);

  /* Starting directory. */
  struct inode * inode;
  if (*path == '/')
    inode = inode_open (ROOT_DIR_SECTOR);
  else
    inode = inode_open (thread_current ()->curr_dir_sector);

  char * token;
  char * prevtok = NULL;
  char * saveptr;
  char name[NAME_MAX + 1] = ".";
  const char * delim = "/";
  struct dir * dir = NULL;
  bool is_dir = true;
  disk_sector_t psector = -1;
  for (token = strtok_r (path, delim, &saveptr); token != NULL;
       token = strtok_r (NULL, delim, &saveptr))
  {
    prevtok = token;
    if (is_dir)
      dir = dir_open (inode);
    else
      break;
    if (dir == NULL) break;
    psector = inode_get_inumber (inode);
    dir_lookup (dir, token, &inode, &is_dir);
    dir_close (dir);
  }

  inode_close (inode);

  if (prevtok != NULL)
    strlcpy (name, prevtok, NAME_MAX + 1);
  free (path);

  if (token != NULL) return false;                /* Break in for loop. */

  dir = dir_open (inode_open (psector));          /* Open parent directory. */
  bool success = dir_remove (dir, name);
  dir_close (dir);
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}

