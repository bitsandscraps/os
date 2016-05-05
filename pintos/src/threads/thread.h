#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <hash.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"
#include "threads/fixed-point.h"

/* States in a thread's life cycle. */
enum thread_status
  {
    THREAD_RUNNING,     /* Running thread. */
    THREAD_READY,       /* Not running but ready to run. */
    THREAD_BLOCKED,     /* Waiting for an event to trigger. */
    THREAD_DYING        /* About to be destroyed. */
  };

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)  /* Error value for tid_t. */

/* Thread priorities. */
#define PRI_MIN 0               /* Lowest priority. */
#define PRI_DEFAULT 31          /* Default priority. */
#define PRI_MAX 63              /* Highest priority. */


/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */

struct thread
  {
    /* Owned by thread.c. */
    tid_t tid;                          /* Thread identifier. */
    enum thread_status status;          /* Thread state. */
    char name[16];                      /* Name (for debugging purposes). */
    uint8_t *stack;                     /* Saved stack pointer. */
    int priority;                       /* Current priority. */
    int initial_priority;               /* Initial priority. */

    /* Parameters for advanced scheduler */
    /* Integer value that determines how nice the thread should be to
     * other threads. */
    int nice;
    /* Metric of how much CPU time the thread has received recently. */
    fixed_point recent_cpu;

    /* Lock that the thread is trying to acquire. */
    struct lock * lock_trying_acquire;
    struct list locks_holding;          /* Locks the thread is holding. */
    int64_t wakeup_tick;                /* Timer tick to wake up. */

    /* Shared between thread.c and synch.c. */
    struct list_elem elem;              /* List element. */
    /* Element in all_list of thread.c. */
    struct list_elem elem_all;          /* List element. */

#ifdef USERPROG
    struct file * executable;     /* The executable file of itself. */

    /* Data Structures for Managing File Descriptors. */
    struct list open_fds;         /* List of open files. */
    /* max_fd is always greater or equal to any files in open_fds.
     * This value is used to assign fd values to new files. */
    int max_fd;
    /* fd_lock must be acquired before modifying file descriptors. */
    struct lock fd_lock;

    /* Data Structues for Implementing Wait. */
    int exit_status;              /* Status of exit. */
    /* The value is 'up'ed when everything is over. */
    struct semaphore is_done;
    /* The value is 'up'ed when parent calls wait. */
    struct semaphore wait_parent;
    /* The value is 'up'ed when the data is ready for its parent. */
    struct semaphore wait_process;
    struct list children;         /* Children of the current process. */

    /* Owned by userprog/process.c. */
    uint32_t *pagedir;            /* Page directory. */
#ifdef VM
    struct lock suppl_page_table_lock;
    struct hash suppl_page_table;
#endif
#endif

    /* Owned by thread.c. */
    unsigned magic;                     /* Detects stack overflow. */
  };

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);
void wake_threads (int64_t current_tick);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_sleep (int64_t wakeup_tick);
void thread_yield (void);

void donate_priority (struct thread * donor);
void restore_priority (struct thread * thr);

int thread_get_priority (void);
void thread_set_priority (int);

void priority_yield(void);
void recent_cpu_recalculate(void);
void recent_cpu_incr(void);
void priority_recalculate(void);
int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

#endif /* threads/thread.h */
