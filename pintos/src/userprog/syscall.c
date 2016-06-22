#include "userprog/syscall.h"
#include <round.h>
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "vm/page.h"

#define WRITEBATCH 1024

/* Map region identifier. */
typedef int mapid_t;
#define MAP_FAILED ((mapid_t) -1)

/* List elements used to manage open file descriptors. */
struct fd_elem
{
  int fd;                   /* assigned fd number */
  int type;                 /* FILE or DIRECTORY? */
  union
    {
      struct file * file;   /* struct file for the fd */
      struct dir * dir;     /* struct dir for the directory */
    } ptr;
  /* Memory mapped to the file. If not mapped, it is set to NULL.
   * It is initialized as NULL and set back to NULL at munmap. */
  struct mapid_elem * mapid;
  struct list_elem elem;    /* list element of open_fds in struct thread */
};

/* List elements used to manage open mmap descriptors. */
struct mapid_elem
{
  int mapid;                /* assigned mapid number */
  void * address;
  size_t pagenum;
  struct fd_elem * fd;      /* fd for the mapped file. */
  struct list_elem elem;    /* list element of open_mapids in struct thread */
};

/* Lock to avoid race conditions on writing to stdout. */
static struct lock console_lock;

typedef int pid_t;

static struct fd_elem * find_fd (int fd);
static struct mapid_elem * find_mapid (mapid_t mapid);
static void munmap_loop (struct mapid_elem * elem);
static void syscall_handler (struct intr_frame *);

static uint32_t syscall_chdir (const char * dir);
static void syscall_close (int fd);
static uint32_t syscall_create (const char * file, size_t initial_size);
static uint32_t syscall_exec (const char * cmd_line);
static uint32_t syscall_filesize (int fd);
static void syscall_halt (void) NO_RETURN;
static uint32_t syscall_inumber (int fd);
static uint32_t syscall_isdir (int fd);
static uint32_t syscall_mkdir (const char * dir);
static uint32_t syscall_mmap (int fd, void * addr);
static void syscall_munmap (mapid_t mapping);
static uint32_t syscall_open (const char * file);
static uint32_t syscall_read (int fd, void * buffer, size_t size);
static uint32_t syscall_readdir (int fd, char * name);
static uint32_t syscall_remove (const char * file);
static void syscall_seek (int fd, size_t position);
static uint32_t syscall_tell (int fd);
static uint32_t syscall_wait (pid_t pid);
static uint32_t syscall_write (int fd, const void * buffer, size_t size);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&console_lock);
}

/* Reads 4 bytes from user virtual address uaddr. Returns the int value
 * if successful, -1 if segfault occurred. */
static int
get_long (const int * uaddr)
{
  int result;
  struct thread * curr = thread_current ();
  if ((void *)uaddr >= PHYS_BASE) return -1;
  curr->mem_check = true;
  asm ("movl $1f, %0; movl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  curr->mem_check = false;
  return result;
}

/* Reads a byte at user virtual address uaddr. Returns false if segfault
 * occurred. */
static bool 
is_valid (const uint8_t * uaddr)
{
  int result;
  struct thread * curr = thread_current ();
  if ((void *)uaddr >= PHYS_BASE) return false;
  curr->mem_check = true;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  curr->mem_check = false;
  return (result != -1);
}

/* Writes a byte at user virtual address uaddr. Returns false if segfault
 * occurred. */
static bool
is_valid_write (uint8_t * udst)
{
  int error_code;
  uint8_t byte = 0;
  struct thread * curr = thread_current ();
  if ((void *)udst >= PHYS_BASE) return false;
  curr->mem_check = true;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
       : "=&a" (error_code), "=m" (*udst) : "r" (byte));
  curr->mem_check = false;
  return error_code != -1;
}

/* Determine whether size virtual addresses starting from uaddr are
 * valid. */
static bool
is_valid_range (const uint8_t * uaddr, size_t size)
{
  size_t i = 0;
  /* If we check every uaddr + N * PGSIZE, we can confirm that every
   * page from uaddr to uaddr + size - 1 is valid. */
  while (i < size)
  {
    if (!is_valid (uaddr + i)) return false;
    i += PGSIZE;
  }
  return is_valid (uaddr + size - 1);
}

/* Determine whether size virtual addresses starting from uaddr are
 * not valid. */
static bool
isnot_valid_range (const uint8_t * uaddr, size_t size)
{
  size_t i = 0;
  /* If we check every uaddr + N * PGSIZE, we can confirm that every
   * page from uaddr to uaddr + size - 1 is not valid. */
  while (i < size)
  {
    if (is_valid (uaddr + i)) return false;
    i += PGSIZE;
  }
  return !is_valid (uaddr + size - 1);
}

/* Determine whether size virtual addresses starting from uaddr are
 * valid to write. */
static bool
is_valid_range_write (uint8_t * uaddr, size_t size)
{
  size_t i = 0;
  /* If we check every uaddr + N * PGSIZE, we can confirm that every
   * page from uaddr to uaddr + size - 1 is valid. */
  while (i < size)
  {
    if (!is_valid_write (uaddr + i)) return false;
    i += PGSIZE;
  }
  return is_valid_write (uaddr + size - 1);
}

/* Handler of the system call. Find out what system call is called and
 * what the arguments are. Pass those arguments and execute the
 * appropriate system call. */
static void
syscall_handler (struct intr_frame *f) 
{
  int * esp = (int *)f->esp;
  int arg1, arg2, arg3;
  struct thread * curr = thread_current ();
  /* Save esp to inform the page fault handler about the user esp. */
  /* Must be done before any operations. */
  curr->stack_ = f->esp;
  int syscall_num = get_long(esp++);
  /* Stack pointer is invalid. */
  if (syscall_num == -1)
  {
    syscall_exit (KERNEL_TERMINATE);
  }
  else if (syscall_num == SYS_HALT)
  {
    syscall_halt ();
  }
  else
  {
    /* Check validity of the first argument. */
    if ((arg1 = get_long(esp++)) == -1) syscall_exit (KERNEL_TERMINATE);
    switch (syscall_num)
    {
      /* Following are the system calls that requires
       * only one argument. */
      case SYS_EXIT:
        syscall_exit (arg1);
        break;
      case SYS_EXEC:
        f->eax = syscall_exec ((const char *)arg1);
        break;
      case SYS_WAIT:
        f->eax = syscall_wait ((pid_t)arg1);
        break;
      case SYS_REMOVE:
        f->eax = syscall_remove ((const char *)arg1);
        break;
      case SYS_OPEN:
        f->eax = syscall_open ((const char *)arg1);
        break;
      case SYS_FILESIZE:
        f->eax = syscall_filesize (arg1);
        break;
      case SYS_TELL:
        f->eax = syscall_tell (arg1);
        break;
      case SYS_CLOSE:
        syscall_close (arg1);
        break;
      case SYS_MUNMAP:
        syscall_munmap ((mapid_t)arg1);
        break;
      case SYS_CHDIR:
        f->eax = syscall_chdir ((const char *)arg1);
        break;
      case SYS_MKDIR:
        f->eax = syscall_mkdir ((const char *)arg1);
        break;
      case SYS_ISDIR:
        f->eax = syscall_isdir (arg1);
        break;
      case SYS_INUMBER:
        f->eax = syscall_inumber (arg1);
        break;
      default:
        /* Check validity of the second argument. */
        if ((arg2 = get_long(esp++)) == -1) syscall_exit (KERNEL_TERMINATE);
        switch (syscall_num)
        {
          /* Following are the system calls that require two
           * arguments. */
          case SYS_CREATE:
            f->eax = syscall_create ((const char *)arg1, (size_t)arg2);
            break;
          case SYS_SEEK:
            syscall_seek (arg1, (size_t)arg2);
            break;
          case SYS_MMAP:
            f->eax = syscall_mmap (arg1, (void *)arg2);
            break;
          case SYS_READDIR:
            f->eax = syscall_readdir (arg1, (char *)arg2);
            break;
          default:
            /* Check validity of third argument. */
            if ((arg3 = get_long(esp)) == -1) syscall_exit (KERNEL_TERMINATE);
            switch (syscall_num)
            {
              /* Following are the system calls that require three
               * arguments. */
              case SYS_READ:
                f->eax = syscall_read (arg1, (void *)arg2, (size_t)arg3);
                break;
              case SYS_WRITE:
                f->eax = syscall_write (arg1, (const void *)arg2, (size_t)arg3);
                break;
              default:
                ASSERT (false);
            }
        }
    }
  }
}

/* Searches for a file with the given fd in the open_fds list of current
 * thread. If there is no file with such fd, returns NULL. Otherwise,
 * returns the pointer to the struct fd_elem of the fd. Assumes that the
 * caller had already acquired required locks. */
static struct fd_elem *
find_fd (int fd)
{
  struct fd_elem * elem;
  struct list_elem * e;
  struct thread * curr = thread_current ();
  for (e = list_begin (&curr->open_fds); e != list_end (&curr->open_fds);
       e = list_next (e))
  {
    elem = list_entry (e, struct fd_elem, elem);
    if (elem->fd == fd) return elem;
  }
  return NULL;
}

/* Searches for a mapid_elem with the given mapid in the open_mapids
 * list of current thread. If there is no mapid_elem with such mapid,
 * returns NULL. Otherwise, returns the pointer to the mapid_elem of the
 * mapid. Assumes that the caller had already acquired required locks. */
static struct mapid_elem *
find_mapid (mapid_t mapid)
{
  struct mapid_elem * elem;
  struct list_elem * e;
  struct thread * curr = thread_current ();
  for (e = list_begin (&curr->open_mapids); e != list_end (&curr->open_mapids);
       e = list_next (e))
  {
    elem = list_entry (e, struct mapid_elem, elem);
    if (elem->mapid == mapid) return elem;
  }
  return NULL;
}

/* Changes the current working directory. */
static uint32_t
syscall_chdir (const char * dir)
{
  if (!is_valid ((uint8_t *)dir))
    syscall_exit (KERNEL_TERMINATE);
  return filesys_chdir (dir);
}

/* Closes the fd. */
static void
syscall_close (int fd)
{
  struct fd_elem * fd_elem;
  fd_elem = find_fd (fd);
  if (fd_elem != NULL)
  {
    list_remove (&fd_elem->elem);
    if (fd_elem->type == TYPE_DIR)
      {
        dir_close (fd_elem->ptr.dir);
        free (fd_elem);
      }
    else if (fd_elem->mapid == NULL)
      {
        file_close (fd_elem->ptr.file);
        free (fd_elem);
      }
  }
}

/* Creates a new file named file with initial size of initial_size
 * bytes. */
static uint32_t
syscall_create (const char * file, size_t initial_size)
{
  bool success;
  if (!is_valid(file))
    syscall_exit (KERNEL_TERMINATE);
  success = filesys_create (file, initial_size);
  return success;
}

/* Runs the executable with the name given in cmd_line. Returns the pid
 * of the new process. If the program cannot load or run for any reason,
 * returns -1. */
static uint32_t
syscall_exec (const char * cmd_line)
{
  int success;
  if (!(is_valid((uint8_t *)cmd_line)))
    syscall_exit (KERNEL_TERMINATE);
  success = process_execute (cmd_line);
  return success;
}

/* Terminates the current user program. Reports its status to kernel. */
void
syscall_exit (int status)
{
  struct thread * curr = thread_current ();
  struct fd_elem * fd_elem;
  curr->exit_status = status;
  /* Close all the open mmap descriptors. */
  while (!list_empty (&curr->open_mapids))
    {
      struct list_elem * e = list_pop_back (&curr->open_mapids);
      munmap_loop (list_entry (e, struct mapid_elem, elem));
    }
  /* Close all the open file descriptors. */
  while (!list_empty (&curr->open_fds))
    {
      struct list_elem * e = list_pop_back (&curr->open_fds);
      fd_elem = list_entry (e, struct fd_elem, elem);
      if (fd_elem->type == TYPE_DIR)
        dir_close (fd_elem->ptr.dir);
      else
        file_close (fd_elem->ptr.file);
      free (fd_elem);
    } 
  printf("%s: exit(%d)\n", curr->name, status);
  thread_exit ();
  NOT_REACHED ();
}

/* Returns the size of the fd in bytes. */
static uint32_t
syscall_filesize (int fd) 
{
  struct fd_elem * fd_elem;
  int size = -1;
  fd_elem = find_fd (fd);
  if (fd_elem != NULL)
    size = (fd_elem->type == TYPE_FILE) ? file_length (fd_elem->ptr.file) : -1;
  return size;
}

/* Terminates Pintos. */
static void
syscall_halt (void)
{
  power_off ();
  NOT_REACHED ();
}

static uint32_t
syscall_inumber (int fd)
{
  if (fd == STDIN_FILENO || fd == STDOUT_FILENO)
    return -1;
  struct fd_elem * felem = find_fd (fd);
  if (felem == NULL)
    return -1;
  struct inode * inode;
  if (felem->type == TYPE_DIR)
    inode = dir_get_inode (felem->ptr.dir);
  else
    inode = file_get_inode (felem->ptr.file);
  return inode_get_inumber (inode);
}

/* Returns true if fd represents a directory. */
static uint32_t
syscall_isdir (int fd)
{
  if (fd == STDIN_FILENO || fd == STDOUT_FILENO)
    return false;
  struct fd_elem * felem = find_fd (fd);
  if (felem == NULL)
    return false;
  return (felem->type == TYPE_DIR);
}

/* Make directory DIR. */
static uint32_t
syscall_mkdir (const char * dir)
{
  if (!is_valid ((uint8_t *)dir))
    syscall_exit (KERNEL_TERMINATE);
  return filesys_mkdir (dir);
}

/* Map file descriptor FD to address ADDR. */
static uint32_t syscall_mmap (int fd, void * addr)
{
  /* Check whether fd is mappable. */
  if (fd == STDIN_FILENO || fd == STDOUT_FILENO)
    return MAP_FAILED;
  /* Check whether address is valid. */
  if (addr == 0)
    return MAP_FAILED;
  /* Check whether the address is page-aligned. */
  if (pg_ofs (addr) != 0)
    return MAP_FAILED;
  struct fd_elem * felem = find_fd (fd);
  if (felem == NULL || felem->type != TYPE_FILE)
    return MAP_FAILED;
  size_t size = (size_t)syscall_filesize(fd);
  if (size == 0)
    return MAP_FAILED;
  /* Check whether there are any already mapped pages within the range
   * ADDR to ADDR + filesize(fd). */
  if (!isnot_valid_range (addr, size))
    return MAP_FAILED;
  struct thread * curr = thread_current ();
  struct mapid_elem * melem = malloc (sizeof (struct mapid_elem));
  felem->mapid = melem;
  if (melem == NULL)
    return MAP_FAILED;
  melem->mapid = ++curr->max_mapid;
  list_push_back (&curr->open_mapids, &melem->elem);
  melem->address = addr;
  melem->pagenum = DIV_ROUND_UP (size, PGSIZE);
  melem->fd = felem;
  struct page src;
  src.status = IN_FILE;
  src.type = TO_FILE;
  src.file = felem->ptr.file;
  src.address = addr;
  src.offset = 0;
  lock_suppl_page_table (curr);
  while (size > 0)
    {
      src.read_bytes = (size > PGSIZE) ? PGSIZE : size;
      add_suppl_page (&src);
      src.address += src.read_bytes;
      src.offset += src.read_bytes;
      size -= src.read_bytes;
    }
  unlock_suppl_page_table (curr);
  return melem->mapid;
}

static void munmap_loop (struct mapid_elem * elem)
{
  ASSERT (elem != NULL);
  list_remove (&elem->elem);
  size_t i;
  /* Munmap may write some files. */
  for (i = 0; i < elem->pagenum; ++i)
    delete_suppl_page (elem->address + i * PGSIZE);
  if (elem->fd->mapid != NULL)
    elem->fd->mapid = NULL;
  else
    {
      file_close (elem->fd->ptr.file);
      free (elem->fd);
    }
  free (elem);
}

static void syscall_munmap (mapid_t mapping)
{
  struct mapid_elem * elem = find_mapid (mapping);
  if (elem != NULL)
    munmap_loop (elem);
  else
    syscall_exit (KERNEL_TERMINATE);
}

/* Opens the file named file. Returns the file descriptor of the opened
 * file, or -1 if the file could not be opened. */
static uint32_t
syscall_open (const char * file_name)
{
  struct thread * curr = thread_current ();
  struct fd_elem * elem;
  int fd = -1;
  if (!is_valid((uint8_t *)file_name))
    syscall_exit (KERNEL_TERMINATE);
  bool is_dir;
  struct inode * inode = filesys_find (file_name, &is_dir);
  if (inode == NULL) return -1;
  elem = malloc (sizeof (struct fd_elem));
  if (elem == NULL)
    syscall_exit (KERNEL_TERMINATE);
  fd = ++curr->max_fd;
  elem->fd = fd;
  elem->type = is_dir ? TYPE_DIR : TYPE_FILE;
  if (is_dir)
  {
    elem->ptr.dir = dir_open (inode);
    if (elem->ptr.dir == NULL)
    {
      free (elem);
      return -1;
    }
  }
  else
  {
    elem->ptr.file = file_open (inode);
    if (elem->ptr.file == NULL)
    {
      free (elem);
      return -1;
    }
  }
  elem->mapid = NULL;
  list_push_back (&curr->open_fds, &elem->elem);
  return fd;
}

/* Reads size bytes from fd to buffer. Returns the number of bytes
 * actually read, or -1 if error occured. */
static uint32_t
syscall_read (int fd, void * buffer, size_t size)
{
  uint8_t * usrbyte = buffer;
  char * usrbuf = buffer;
  int nread = 0;
  struct fd_elem * fd_elem;
  if (!is_valid_range_write (usrbyte, size))
    syscall_exit (KERNEL_TERMINATE);
  if (fd == STDIN_FILENO)
  {
    while (size-- > 0)
      *usrbuf++ = input_getc();
    nread = (int)size;
  }
  else
  {
    fd_elem = find_fd (fd);
    if (fd_elem == NULL)
      nread = -1;
    else if (fd_elem->type != TYPE_FILE)
      nread = -1;
    else
      nread = file_read (fd_elem->ptr.file, buffer, size);
  }
  return nread;
}

static uint32_t
syscall_readdir (int fd, char * name)
{
  if (!is_valid_range_write ((uint8_t *) name, NAME_MAX + 1))
    syscall_exit (KERNEL_TERMINATE);
  if (fd == STDIN_FILENO || fd == STDOUT_FILENO)
    return false;
  struct fd_elem * felem = find_fd (fd);
  if (felem == NULL)
    return false;
  if (felem->type != TYPE_DIR)
    return false;
  return dir_readdir(felem->ptr.dir, name);
}

/* Deletes the file named file. Returns true if succeeded. */
static uint32_t
syscall_remove (const char * file)
{
  bool success;
  if (!is_valid((uint8_t *)file))
    syscall_exit (KERNEL_TERMINATE);
  success = filesys_remove (file);
  return success;
}

/* Changes the next byte to be read or written in fd to position,
 * expressed in bytes from the beginning of the file. */
static void
syscall_seek (int fd, size_t position)
{
  struct fd_elem * fd_elem;
  fd_elem = find_fd (fd);
  if (fd_elem != NULL)
    if (fd_elem->type != TYPE_DIR)
      file_seek (fd_elem->ptr.file, position);
}

/* Returns the position of the next byte to read or written in fd,
 * expressed in bytes from the beginning of the file. */
static uint32_t
syscall_tell (int fd)
{
  struct fd_elem * fd_elem = find_fd (fd);
  if (fd_elem != NULL)
    if (fd_elem->type != TYPE_DIR)
      return file_tell (fd_elem->ptr.file);
  return -1;
}

/* Waits for pid to die and returns its status. Returns -1 if the pid
 * was terminated by the kernel. Returns -1 without waiting if the pid
 * is not a child of the process, or wait has already been successfully
 * called for the pid. */
static uint32_t
syscall_wait (pid_t pid)
{
  return process_wait (pid);
}

/* Writes size bytes from buffer to fd. Returns the number of bytes
 * actually written, or -1 if error occurred. */
static uint32_t
syscall_write (int fd, const void * buffer, size_t size)
{
  size_t to_write = size;
  const uint8_t * usrbyte = buffer;
  const char * usrbuf = buffer;
  struct fd_elem * fd_elem;
  int nwrite = 0;
  if (!is_valid_range (usrbyte, size))
    syscall_exit (KERNEL_TERMINATE);
  if (fd == STDIN_FILENO)
    return -1;
  else if (fd == STDOUT_FILENO)
  {
    lock_acquire (&console_lock);
    while (to_write > 0)
    {
      if (to_write > WRITEBATCH)
      {
        putbuf (usrbuf, WRITEBATCH);
        buffer += WRITEBATCH;
        to_write -= WRITEBATCH;
      }
      else
      {
        putbuf (usrbuf, to_write);
        to_write = 0;
      }
    }
    lock_release (&console_lock);
    nwrite = size;
  }
  else
  {
    fd_elem = find_fd (fd);
    if (fd_elem == NULL)
      nwrite = -1;
    else if (fd_elem->type != TYPE_FILE)
      nwrite = -1;
    else
    {
      off_t pos = file_tell (fd_elem->ptr.file);
      nwrite = file_write (fd_elem->ptr.file, buffer, size);
      /* If this file is mapped to a certain memory, edit that memory too. */
      if (fd_elem->mapid != NULL)
        memcpy (fd_elem->mapid->address + pos, buffer, nwrite);
    }
  }
  return nwrite;
}

