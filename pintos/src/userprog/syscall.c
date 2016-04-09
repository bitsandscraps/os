#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"

static struct lock filesys_lock;

typedef int pid_t;

static void epilogue(int status);
static void syscall_handler (struct intr_frame *);
static void syscall_halt (void);
static void syscall_exit (int status);
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

/* Reads a byte at user virtual address uaddr. Returns the byte value
 * if successful, -1 if segfault occurred. */
static int
get_byte (const uint8_t * uaddr)
{
  int result;
  if ((void *)uaddr >= PHYS_BASE) return -1;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
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
  if (syscall_num == SYS_HALT)
  {
    syscall_halt ();
  }
  else
  {
    arg1 = get_long(esp++);
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
      default:
        arg2 = get_long(esp++);
        switch (syscall_num)
        {
          case SYS_CREATE:
            f->eax = syscall_create ((const char *)arg1, (size_t)arg2);
            break;
          case SYS_SEEK:
            syscall_seek ((int)arg1, (size_t)arg2);
            break;
          default:
            arg3 = get_long(esp);
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
epilogue(int status)
{
  printf("%s: exit(%d)\n", thread_current ()->name, status);
  thread_exit ();
  NOT_REACHED ();
}

static void
syscall_halt (void)
{
  power_off ();
  NOT_REACHED ();
}

static void
syscall_exit (int status)
{
  epilogue(status);
  NOT_REACHED ();
}

static uint32_t
syscall_exec (const char * cmd_line)
{
  int success;
  if (cmd_line == NULL || !(get_byte((uint8_t *)cmd_line)))
    epilogue (-1);
  lock_acquire (&filesys_lock);
  success = process_execute (cmd_line);
  lock_release (&filesys_lock);
  return success;
}

static uint32_t
syscall_wait (pid_t pid UNUSED)
{
  return true;
}

static uint32_t
syscall_create (const char * file, size_t initial_size)
{
  bool success;
  if (file == NULL)
    return false;
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
    return false;
  lock_acquire (&filesys_lock);
  success = filesys_remove (file);
  lock_release (&filesys_lock);
  return success;
}

static uint32_t
syscall_open (const char * file UNUSED)
{
  return 0;
}

static uint32_t
syscall_filesize (int fd UNUSED) 
{
  return 0;
}

static uint32_t
syscall_read (int fd UNUSED, void * buffer UNUSED, size_t size UNUSED)
{
  return 0;
}

static uint32_t
syscall_write (int fd UNUSED, const void * buffer, size_t size)
{
  putbuf (buffer, size);
  return size;
}

static void
syscall_seek (int fd UNUSED, size_t position UNUSED)
{
}

static uint32_t
syscall_tell (int fd UNUSED)
{
  return 0;
}

static void
syscall_close (int fd UNUSED)
{
}

