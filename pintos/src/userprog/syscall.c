#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"

typedef int pid_t;

static void syscall_handler (struct intr_frame *);
static void halt (void);
static void exit (int status);
static uint32_t exec (const char * cmd_line);
static uint32_t wait (pid_t pid);
static uint32_t create (const char * file, size_t initial_size);
static uint32_t remove (const char * file);
static uint32_t open (const char * file);
static uint32_t filesize (int fd);
static uint32_t read (int fd, void * buffer, size_t size);
static uint32_t write (int fd, const void * buffer, size_t size);
static void seek (int fd, size_t position);
static uint32_t tell (int fd);
static void close (int fd);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  int * esp = (int *)f->esp;
  int syscall_num = *esp++;
  int arg1, arg2, arg3;
  if (syscall_num == SYS_HALT)
  {
    halt ();
  }
  else
  {
    arg1 = *esp++;
    switch (syscall_num)
    {
      case SYS_EXIT:
        exit (arg1);
        break;
      case SYS_EXEC:
        f->eax = exec ((const char *)arg1);
        break;
      case SYS_WAIT:
        f->eax = wait ((pid_t)arg1);
        break;
      case SYS_REMOVE:
        f->eax = remove ((const char *)arg1);
        break;
      case SYS_OPEN:
        f->eax = open ((const char *)arg1);
        break;
      case SYS_FILESIZE:
        f->eax = filesize (arg1);
        break;
      case SYS_TELL:
        f->eax = tell (arg1);
        break;
      case SYS_CLOSE:
        close (arg1);
      default:
        arg2 = *esp++;
        switch (syscall_num)
        {
          case SYS_CREATE:
            f->eax = create ((const char *)arg1, (size_t)arg2);
            break;
          case SYS_SEEK:
            seek ((int)arg1, (size_t)arg2);
            break;
          default:
            arg3 = *esp;
            switch (syscall_num)
            {
              case SYS_READ:
                f->eax = read (arg1, (void *)arg2, (size_t)arg3);
                break;
              case SYS_WRITE:
                f->eax = write (arg1, (const void *)arg2, (size_t)arg3);
                break;
              default:
                ASSERT (false);
            }
        }
    }
  }
  thread_exit ();
}

static void
halt (void)
{
}

static void
exit (int status UNUSED)
{
}

static uint32_t
exec (const char * cmd_line UNUSED)
{
  return -1;
}

static uint32_t
wait (pid_t pid UNUSED)
{
  return true;
}

static uint32_t
create (const char * file UNUSED, size_t intitial_size UNUSED)
{
  return true;
}

static uint32_t
remove (const char * file UNUSED)
{
  return true;
}

static uint32_t
open (const char * file UNUSED)
{
  return 0;
}

static uint32_t
filesize (int fd UNUSED) 
{
  return 0;
}

static uint32_t
read (int fd UNUSED, void * buffer UNUSED, size_t size UNUSED)
{
  return 0;
}

static uint32_t
write (int fd UNUSED, const void * buffer UNUSED, size_t size UNUSED)
{
  return 0;
}

static void
seek (int fd UNUSED, size_t position UNUSED)
{
}

static uint32_t
tell (int fd UNUSED)
{
  return 0;
}

static void
close (int fd UNUSED)
{
}

