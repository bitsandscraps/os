#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "devices/timer.h"
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "threads/fixed-point.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* Random value for basic thread
   Do not modify this value. */
#define THREAD_BASIC 0xd42df210

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* List of processes that are sleeping. */
static struct list waiting_list;

/* List of processes. it includes running, blocked, and ready states.
 * threads are included when init_thread called
 * and threads are excluded when thread_exit called*/
static struct list thread_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* value of load_avg used wehn mlfqs is 1 */
fixed_point load_avg;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;
static void kernel_thread (thread_func *, void *aux);
static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool higher_priority(const struct list_elem *a,
                            const struct list_elem *b,
                            void *aux UNUSED);
static bool lesser_wakeup_tick (const struct list_elem *a,
                                const struct list_elem *b,
                                void *aux UNUSED); 
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
int thread_get_nice(void);
void thread_set_nice(int new_nice);

//void priority_recalculate(void);
void recent_cpu_incr(void);
void recent_cpu_recalculate(void);
void priority_recalculate_indiv(struct thread *);
void priority_yield(void);
void recent_cpu_recalculate_indiv(struct thread *);

/* yeild current thread if it's priority is no more the highest */
void
priority_yield(void)
{
  if(list_empty(&ready_list)) return;
  struct thread * thr=list_entry(list_front(&ready_list), struct thread, elem);
  if(thread_current()->priority < thr->priority)
    thread_yield();
  
}


/* increase running thread's recent_cpu */
void
recent_cpu_incr(void)
{
  ASSERT(thread_mlfqs);
  if(thread_current()==idle_thread) return;
  struct thread * thr=thread_current();
  thr->recent_cpu=fp_add_int(thr->recent_cpu, 1);
}
void priority_recalculate(void)
{
  ASSERT(thread_mlfqs);
  struct list_elem * e;
  struct thread * thr;
  enum intr_level old_level=intr_disable();
  if(list_empty(&thread_list)) return;
  for(e=list_begin(&thread_list);e!=list_end(&thread_list);e=list_next(e))
  {
    thr=list_entry(e, struct thread, elem_);
    priority_recalculate_indiv(thr);
  }
  list_sort(&ready_list, higher_priority, NULL);
  intr_set_level(old_level);
}
/* recalculate priority of given thread*/
void
priority_recalculate_indiv(struct thread * thr)
{ 
  ASSERT(thread_mlfqs);
  if(thr==idle_thread) return;
  ASSERT(thr!=idle_thread);
  int new_pri;
  new_pri=
    PRI_MAX-fp_round(fp_divide_int(thr->recent_cpu,4))-(thr->nice)*2;
  thr->priority=new_pri;
  if(thr->priority>PRI_MAX) thr->priority=PRI_MAX;
  if(thr->priority<PRI_MIN) thr->priority=PRI_MIN;
}

/* recalculate priority of every threads in thread_list */
void
recent_cpu_recalculate_indiv(struct thread * thr)
{
  ASSERT(thread_mlfqs);
  if(thr==idle_thread) return;
  ASSERT(thr!=idle_thread);
  fixed_point cpu;
  cpu=fp_divide(fp_multiply_int(load_avg,2), 
                fp_add_int(fp_multiply_int(load_avg,2),1));
  thr->recent_cpu=fp_add_int(fp_multiply(thr->recent_cpu, cpu),
                             thr->nice);
}

/* recalculate recent_cpu of every threads in thread_list */
void
recent_cpu_recalculate(void)
{
  ASSERT(thread_mlfqs);
  ASSERT(intr_context());
  struct list_elem *a;
  struct thread * thr;
  int size_thread;
  enum intr_level old_level=intr_disable();
  size_thread=list_size(&ready_list);
  if(thread_current()!=idle_thread)
    size_thread++;

  load_avg=fp_add(fp_divide_int(fp_multiply_int(load_avg,59),60),
                  fp_divide_int(fp(size_thread),60));
  for(a=list_begin(&thread_list);a!=list_end(&thread_list);a=list_next(a))
  {
    thr=list_entry(a, struct thread, elem_);
    recent_cpu_recalculate_indiv(thr);
  }
  intr_set_level(old_level);
  priority_recalculate();
  
}

/*returns the current thread's nice value */

int
thread_get_nice(void)
{
  return thread_current()->nice;
}


/* sets the current thread's nice value to new_value
 * and recalcultae the thrads' priority.
 * if it's priority is no more the highes, yield it.
 * */

void
thread_set_nice(int new_nice)
{
  enum intr_level old_level=intr_disable();
  struct thread * thr;
  thr=thread_current ();
  thr->nice=new_nice;
  recent_cpu_recalculate_indiv(thr);
  priority_recalculate_indiv(thr);
  if(thread_current()!=idle_thread)
    priority_yield(); 
  intr_set_level(old_level);
  
}
/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void) 
{
  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&ready_list);
  list_init (&waiting_list);
  list_init (&thread_list);
  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void) 
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);
  
  /* initialize load_avg */
  load_avg=fp(0);
  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. */
void
thread_tick (void) 
{
  struct thread *t = thread_current ();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();
}

/* Donate priority of donor to the holder of the lock
 * the thread is trying to acquire. */
void
donate_priority (struct thread * donor)
{
  if(thread_mlfqs) return;
  struct lock * lock = donor->lock_trying_acquire;
  ASSERT (lock);
  while (lock)
  {
    struct thread * donee = lock->holder;
    ASSERT (is_thread (donor) && is_thread (donee));
    if (donor->priority > donee->priority)
    {
      donee->priority = donor->priority;
      lock->priority = donor->priority;
    }
    lock = donee->lock_trying_acquire;
  }
}

/* Restores initial priority of thr. */
void
restore_priority (struct thread * thr)
{
  if(thread_mlfqs) return;
  ASSERT (is_thread (thr));
  int priority = thr->initial_priority;
  struct list * lock_list = &thr->locks_holding;
  struct list_elem * e;
  for (e = list_begin (lock_list); e != list_end (lock_list);
       e = list_next (e))
  {
    struct lock * lock = list_entry (e, struct lock, elem);
    if (priority < lock->priority)
      priority = lock->priority;
  }
  thr->priority = priority;
}

/* Prints thread statistics. */
void
thread_print_stats (void) 
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;

  /* Add to run queue. */
  thread_unblock (t);
  
  
  /* Yield the current thread if the new thread has higher priority. */
  priority_yield();
  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void) 
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t) 
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_insert_ordered(&ready_list, &t->elem, higher_priority, NULL);
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void) 
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void) 
{
  struct thread *t = running_thread ();
  
  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void) 
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */

static inline bool
is_interior(struct list_elem * elem)
{
  return elem !=NULL && elem->prev !=NULL && elem->next !=NULL;
}
void
thread_exit (void) 
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Just set our status to dying and schedule another process.
     We will be destroyed during the call to schedule_tail(). */
  intr_disable ();
  list_remove(&thread_current()->elem_);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Puts the current thread to sleep.  It will not be scheduled
 * again until the given timer tick. */
void
thread_sleep (int64_t wakeup_tick)
{
  struct thread *curr = thread_current ();
  enum intr_level old_level;
  ASSERT (!intr_context ());
  old_level = intr_disable ();
  curr->wakeup_tick = wakeup_tick;
  list_insert_ordered (&waiting_list, &curr->elem, lesser_wakeup_tick, NULL);
  thread_block();
  intr_set_level (old_level);
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void) 
{
  struct thread *curr = thread_current ();
  enum intr_level old_level;
  
  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (curr != idle_thread) 
    list_insert_ordered(&ready_list, &curr->elem, higher_priority, NULL);
  curr->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void
thread_set_priority (int new_priority) 
{
  if(thread_mlfqs) return;
  struct thread * curr = thread_current ();
  int old_priority = curr->priority;
  if (curr->priority == curr->initial_priority)
    curr->priority = new_priority;
  curr->initial_priority = new_priority;
  if (!list_empty(&ready_list) && old_priority > new_priority)
    thread_yield();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void) 
{
  return thread_current ()->priority;
}



/* Returns 100 times the system load average. */
int
thread_get_load_avg (void) 
{
  return fp_round(fp_multiply_int(load_avg,100));
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void) 
{
  return fp_round(fp_multiply_int(thread_current()->recent_cpu,100));
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED) 
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;) 
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);
                    
  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void) 
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Since `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Wakes threads with wakeup_tick less or equal to the current_tick
 * in the waiting_list. */
void
wake_threads (int64_t current_tick)
{
  struct list_elem *e, *f;
  for (e = list_begin (&waiting_list); e != list_end (&waiting_list); )
  {
    struct thread *thr = list_entry (e, struct thread, elem);
    ASSERT (is_thread (thr));
    if (thr->wakeup_tick > current_tick)
      break;
    ASSERT (thr->status == THREAD_BLOCKED);
    f = list_remove(e);
    thread_unblock (thr);
    e = f;
  }
}

/* Returns true if wakeup_tick of a is less than wake_tick of b. */
static bool
lesser_wakeup_tick (const struct list_elem *a,
                const struct list_elem *b, void *aux UNUSED)
{
  const struct thread *a_thr = list_entry (a, struct thread, elem);
  const struct thread *b_thr = list_entry (b, struct thread, elem);
  return a_thr->wakeup_tick < b_thr->wakeup_tick;
}

/* Returns true if priority of a is higher than priority of b. */
static bool
higher_priority (const struct list_elem *a,
               const struct list_elem *b, void *aux UNUSED)
{
  const struct thread *a_thr = list_entry (a, struct thread, elem);
  const struct thread *b_thr = list_entry (b, struct thread, elem);
  return a_thr->priority > b_thr->priority;
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  list_push_back(&thread_list, &t->elem_);
  t->initial_priority = priority;
  list_init (&t->locks_holding);
  t->lock_trying_acquire = NULL;
  if(thread_mlfqs) {
    if(t==initial_thread) {
      t->recent_cpu=0;
      t->nice=0;
      t->priority;
    }
    else {
      t->recent_cpu=thread_current()->recent_cpu;
      t->nice=thread_current()->nice;
      priority_recalculate_indiv(t);
    }
  }
  t->magic = THREAD_MAGIC;
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size) 
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread *
next_thread_to_run (void) 
{
  if (list_empty (&ready_list))
    return idle_thread;
  else
    return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
schedule_tail (struct thread *prev) 
{
  struct thread *curr = running_thread ();
  
  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  curr->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) 
    {
      ASSERT (prev != curr);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.
   
   It's not safe to call printf() until schedule_tail() has
   completed. */
static void
schedule (void) 
{
  struct thread *curr = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (curr->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (curr != next)
    prev = switch_threads (curr, next);
  schedule_tail (prev); 
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void) 
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
