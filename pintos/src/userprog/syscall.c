#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"

#define WRITEBATCH 1024

struct fd_elem
{
  int fd;
  off_t pos;
  struct file * file;
  struct list_elem elem;
};

static struct lock filesys_lock;

typedef int pid_t;

static void epilogue (int status);
static struct fd_elem * find_fd (int fd);
static void syscall_handler (struct intr_frame *);
static void syscall_halt (void);
static uint32_t syscall_exec (const char * cmd_line);
static uint32_t syscall_wait (pid_t pid);
static uint32_t syscall_create (const char * file, size_t initial_size);
static uint32_t syscall_remove (const char * file);
static uint32_t syscall_open (const char * file);
static uint32_t syscall_filesize (int fd);
static uint32_t syscall_read (int fd, void * buffer, size_t size);
static uint32_t syscall_write (int fd, const void * buffer, size_t size);
static void syscall_seek (int fd, size_t position);
static uint32_t syscall_tell (int fd);
static void syscall_close (int fd);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

/* Reads 4 bytes from user virtual address uaddr. Returns the int value
 * if successful, -1 if segfault occurred. */
static int
get_long (const int * uaddr)
{
  int result;
  if ((void *)uaddr >= PHYS_BASE) return -1;
  asm ("movl $1f, %0; movl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}

/* Reads a byte at user virtual address uaddr. Returns false if segfault
 * occurred. */
static bool 
is_valid (const uint8_t * uaddr)
{
  int result;
  if ((void *)uaddr >= PHYS_BASE) return false;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return (result != -1);
}

/* Writes byte to user address udst. Returns true if successful, false
 * if a segfault occurred. */
/*static bool
put_user (uint8_t *udst, uint8_t byte)
{
  int error_code;
  if ((void *)udst >= PHYS_BASE) return false;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
       : "=&a" (error_code), "=m" (*udst) : "r" (byte));
  return error_code != -1;
}*/

static void
syscall_handler (struct intr_frame *f) 
{
  int * esp = (int *)f->esp;
  int syscall_num = get_long(esp++);
  int arg1, arg2, arg3;
  if (syscall_num == -1)
  {
    epilogue (EXIT_FAILURE);
  }
  else if (syscall_num == SYS_HALT)
  {
    syscall_halt ();
  }
  else
  {
    if ((arg1 = get_long(esp++)) == -1) epilogue (EXIT_FAILURE);
    switch (syscall_num)
    {
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
      default:
        if ((arg2 = get_long(esp++)) == -1) epilogue (EXIT_FAILURE);
        switch (syscall_num)
        {
          case SYS_CREATE:
            f->eax = syscall_create ((const char *)arg1, (size_t)arg2);
            break;
          case SYS_SEEK:
            syscall_seek ((int)arg1, (size_t)arg2);
            break;
          default:
            if ((arg3 = get_long(esp)) == -1) epilogue (EXIT_FAILURE);
            switch (syscall_num)
            {
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

static void
epilogue (int status)
{
  printf("%s: exit(%d)\n", thread_current ()->name, status);
  thread_exit ();
  NOT_REACHED ();
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

static void
syscall_halt (void)
{
  power_off ();
  NOT_REACHED ();
}

void
syscall_exit (int status)
{
  struct thread * curr = thread_current ();
  struct fd_elem * fd_elem;
  lock_acquire (&filesys_lock);
  lock_acquire (&curr->fd_lock);
  while (!list_empty (&curr->open_fds))
  {
    struct list_elem * e = list_pop_back (&curr->open_fds);
    fd_elem = list_entry (e, struct fd_elem, elem);
    file_close (fd_elem->file);
    free (fd_elem);
  } 
  lock_release (&curr->fd_lock);
  lock_release (&filesys_lock);
  epilogue(status);
  NOT_REACHED ();
}

static uint32_t
syscall_exec (const char * cmd_line)
{
  int success;
  if (cmd_line == NULL || !(is_valid((uint8_t *)cmd_line)))
    epilogue (-1);
  lock_acquire (&filesys_lock);
  success = process_execute (cmd_line);
  lock_release (&filesys_lock);
  return success;
}

static uint32_t
syscall_wait (pid_t pid)
{
  return process_wait (pid);
}

static uint32_t
syscall_create (const char * file, size_t initial_size)
{
  bool success;
  if (file == NULL || !is_valid(file))
    epilogue (EXIT_FAILURE);
  lock_acquire (&filesys_lock);
  success = filesys_create (file, initial_size);
  lock_release (&filesys_lock);
  return success;
}

static uint32_t
syscall_remove (const char * file)
{
  bool success;
  if (file == NULL)
    epilogue (EXIT_FAILURE);
  lock_acquire (&filesys_lock);
  success = filesys_remove (file);
  lock_release (&filesys_lock);
  return success;
}

static uint32_t
syscall_open (const char * file_name)
{
  struct thread * curr = thread_current ();
  struct file * file;
  struct fd_elem * elem;
  int fd = -1;
  if (file_name == NULL || !is_valid(file_name))
    epilogue (EXIT_FAILURE);
  lock_acquire (&filesys_lock);
  file = filesys_open (file_name);
  if (file != NULL)
  {
    elem = malloc (sizeof (struct fd_elem));
    if (elem == NULL)
    {
      lock_release (&filesys_lock);
      epilogue (EXIT_FAILURE);
    }
    lock_acquire (&curr->fd_lock);
    fd = ++curr->max_fd;
    elem->fd = fd;
    elem->file = file;
    elem->pos = 0;
    list_push_back (&curr->open_fds, &elem->elem);
    lock_release (&curr->fd_lock);
  }
  lock_release (&filesys_lock);
  return fd;
}

static uint32_t
syscall_filesize (int fd) 
{
  struct thread * curr = thread_current ();
  struct fd_elem * fd_elem;
  int size = -1;
  lock_acquire (&filesys_lock);
  lock_acquire (&curr->fd_lock);
  fd_elem = find_fd (fd);
  if (fd_elem != NULL)
    size = file_length (fd_elem->file);
  else
    size = -1;
  lock_release (&curr->fd_lock);
  lock_release (&filesys_lock);
  return size;
}

static uint32_t
syscall_read (int fd, void * buffer, size_t size)
{
  char * usrbuf = (char *)buffer;
  int nread = 0;
  struct fd_elem * fd_elem;
  if (!is_valid(buffer))
    epilogue (EXIT_FAILURE);
  lock_acquire (&filesys_lock);
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
    else
    {
      nread = file_read_at (fd_elem->file, buffer, size, fd_elem->pos);
      fd_elem->pos += nread;
    }
  }
  lock_release (&filesys_lock);
  return nread;
}

static uint32_t
syscall_write (int fd, const void * buffer, size_t size)
{
  size_t to_write = size;
  const char * usrbuf = (const char *)buffer;
  struct fd_elem * fd_elem;
  int nwrite = 0;
  if (!is_valid(buffer))
    epilogue (EXIT_FAILURE);
  lock_acquire (&filesys_lock);
  if (fd == STDIN_FILENO)
  {
    lock_release (&filesys_lock);
    return -1;
  }
  else if (fd == STDOUT_FILENO)
  {
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
    nwrite = size;
  }
  else
  {
    fd_elem = find_fd (fd);
    if (fd_elem == NULL)
      nwrite = -1;
    else
    {
      nwrite = file_write_at (fd_elem->file, buffer, size, fd_elem->pos);
      fd_elem->pos += nwrite;
    }
  }
  lock_release (&filesys_lock);
  return nwrite;
}

static void
syscall_seek (int fd, size_t position)
{
  struct thread * curr = thread_current ();
  struct fd_elem * fd_elem;
  lock_acquire (&filesys_lock);
  lock_acquire (&curr->fd_lock);
  fd_elem = find_fd (fd);
  if (fd_elem != NULL)
    fd_elem->pos = position;
  lock_release (&curr->fd_lock);
  lock_release (&filesys_lock);
}

static uint32_t
syscall_tell (int fd)
{
  struct thread * curr = thread_current ();
  struct fd_elem * fd_elem;
  int pos = -1;
  lock_acquire (&filesys_lock);
  lock_acquire (&curr->fd_lock);
  fd_elem = find_fd (fd);
  if (fd_elem != NULL)
    pos = fd_elem->pos;
  lock_release (&curr->fd_lock);
  lock_release (&filesys_lock);
  return pos;
}

static void
syscall_close (int fd)
{
  struct thread * curr = thread_current ();
  struct fd_elem * fd_elem;
  lock_acquire (&filesys_lock);
  lock_acquire (&curr->fd_lock);
  fd_elem = find_fd (fd);
  if (fd_elem != NULL)
  {
    file_close (fd_elem->file);
    list_remove (&fd_elem->elem);
    free (fd_elem);
  }
  lock_release (&curr->fd_lock);
  lock_release (&filesys_lock);
}

