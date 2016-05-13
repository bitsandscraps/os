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

/* List elements used to manage open file descriptors. */
struct fd_elem
{
  int fd;                   /* assigned fd number */
  off_t pos;                /* offset for read and write */
  struct file * file;       /* struct file for the fd */
  struct list_elem elem;    /* list element of open_fds in struct thread */
};

/* Lock to avoid race conditions on file system manipulations. */
static struct lock filesys_lock;

typedef int pid_t;

static struct fd_elem * find_fd (int fd);
static void syscall_handler (struct intr_frame *);
static void syscall_halt (void) NO_RETURN;
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

/* Writes a byte at user virtual address uaddr. Returns false if segfault
 * occurred. */
static bool
is_valid_write (uint8_t * udst)
{
  int error_code;
  uint8_t byte = 0;
  if ((void *)udst >= PHYS_BASE) return false;
  asm ("movl $1f, %0; movb %b2, %1; 1:"
       : "=&a" (error_code), "=m" (*udst) : "r" (byte));
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
  curr->stack = f->esp;
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
            syscall_seek ((int)arg1, (size_t)arg2);
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

/* Terminates Pintos. */
static void
syscall_halt (void)
{
  power_off ();
  NOT_REACHED ();
}

/* Terminates the current user program. Reports its status to kernel. */
void
syscall_exit (int status)
{
  struct thread * curr = thread_current ();
  struct fd_elem * fd_elem;
  curr->exit_status = status;
  lock_acquire (&filesys_lock);
  lock_acquire (&curr->fd_lock);
  /* Close all the open file descriptors. */
  while (!list_empty (&curr->open_fds))
  {
    struct list_elem * e = list_pop_back (&curr->open_fds);
    fd_elem = list_entry (e, struct fd_elem, elem);
    file_close (fd_elem->file);
    free (fd_elem);
  } 
  lock_release (&curr->fd_lock);
  lock_release (&filesys_lock);
  printf("%s: exit(%d)\n", curr->name, status);
  thread_exit ();
  NOT_REACHED ();
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
  lock_acquire (&filesys_lock);
  success = process_execute (cmd_line);
  lock_release (&filesys_lock);
  return success;
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

/* Creates a new file named file with initial size of initial_size
 * bytes. */
static uint32_t
syscall_create (const char * file, size_t initial_size)
{
  bool success;
  if (!is_valid(file))
    syscall_exit (KERNEL_TERMINATE);
  lock_acquire (&filesys_lock);
  success = filesys_create (file, initial_size);
  lock_release (&filesys_lock);
  return success;
}

/* Deletes the file named file. Returns true if succeeded. */
static uint32_t
syscall_remove (const char * file)
{
  bool success;
  if (!is_valid((uint8_t *)file))
    syscall_exit (KERNEL_TERMINATE);
  lock_acquire (&filesys_lock);
  success = filesys_remove (file);
  lock_release (&filesys_lock);
  return success;
}

/* Opens the file named file. Returns the file descriptor of the opened
 * file, or -1 if the file could not be opened. */
static uint32_t
syscall_open (const char * file_name)
{
  struct thread * curr = thread_current ();
  struct file * file;
  struct fd_elem * elem;
  int fd = -1;
  if (!is_valid((uint8_t *)file_name))
    syscall_exit (KERNEL_TERMINATE);
  lock_acquire (&filesys_lock);
  file = filesys_open (file_name);
  if (file != NULL)
  {
    elem = malloc (sizeof (struct fd_elem));
    if (elem == NULL)
    {
      lock_release (&filesys_lock);
      syscall_exit (KERNEL_TERMINATE);
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

/* Returns the size of the fd in bytes. */
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

/* Changes the next byte to be read or written in fd to position,
 * expressed in bytes from the beginning of the file. */
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

/* Returns the position of the next byte to read or written in fd,
 * expressed in bytes from the beginning of the file. */
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

/* Closes the fd. */
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

