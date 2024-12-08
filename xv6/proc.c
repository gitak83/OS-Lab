#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"

struct
{
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);
int change_Q(int pid, int new_queue);
struct proc *
roundrobin(struct proc *last_scheduled);
struct proc *
fcfs(void);

void pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int cpuid()
{
  return mycpu() - cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu *
mycpu(void)
{
  int apicid, i;

  if (readeflags() & FL_IF)
    panic("mycpu called with interrupts enabled\n");

  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i)
  {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc *
myproc(void)
{
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

// PAGEBREAK: 32
//  Look in the process table for an UNUSED proc.
//  If found, change state to EMBRYO and initialize
//  state required to run in the kernel.
//  Otherwise return 0.
static struct proc *
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->burst_time = 2;
  p->confidence = 50;
  p->wait_time = 0;
  p->last_run = 0;
  /*if(p->pid < 3){
    p->queue = ROUND_ROBIN;
  }
  else{
    p->queue = FCFS;
  }*/

  release(&ptable.lock);

  // Allocate kernel stack.
  if ((p->kstack = kalloc()) == 0)
  {
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe *)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint *)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context *)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

// PAGEBREAK: 32
//  Set up first user process.
void userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();

  initproc = p;
  if ((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0; // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
  change_Q(p->pid, UNSET);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if (n > 0)
  {
    if ((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  else if (n < 0)
  {
    if ((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if ((np = allocproc()) == 0)
  {
    return -1;
  }

  // Copy process state from proc.
  if ((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0)
  {
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for (i = 0; i < NOFILE; i++)
    if (curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);
  change_Q(np->pid, UNSET);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if (curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for (fd = 0; fd < NOFILE; fd++)
  {
    if (curproc->ofile[fd])
    {
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->parent == curproc)
    {
      p->parent = initproc;
      if (p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();

  acquire(&ptable.lock);
  for (;;)
  {
    // Scan through table looking for exited children.
    havekids = 0;
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->parent != curproc)
        continue;
      havekids = 1;
      if (p->state == ZOMBIE)
      {
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if (!havekids || curproc->killed)
    {
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock); // DOC: wait-sleep
  }
}

// PAGEBREAK: 42
//  Per-CPU process scheduler.
//  Each CPU calls scheduler() after setting itself up.
//  Scheduler never returns.  It loops, doing:
//   - choose a process to run
//   - swtch to start running that process
//   - eventually that process transfers control
//       via swtch back to the scheduler.
struct proc *
roundrobin(struct proc *last_scheduled)
{
  // cprintf("rr\n");
  struct proc *p = last_scheduled;
  for (;;)
  {
    p++;
    if (p >= &ptable.proc[NPROC])
      p = ptable.proc;

    if (p->state == RUNNABLE && p->queue == ROUND_ROBIN)
      return p;

    if (p == last_scheduled)
      return 0;
  }
}

struct proc *
fcfs(void)
{
  struct proc *result = 0;
  struct proc *p;
  // cprintf("fcfs\n");

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {

    if (p->state != RUNNABLE || p->queue != FCFS)
      continue;
    if (!result)
    {
      if (p->arrival < result->arrival)
      {
        // cprintf("%d\n", p->pid);
        result = p;
      }
    }
    else
    {
      // cprintf("%d\n", p->pid);
      result = p;
    }
  }
  return result;
}

int shortestjob(struct proc *p)
{
  int random = (((ticks * ticks * ticks + ticks * 100) % 500) * ((ticks * ticks * ticks + ticks * 100) % 500)) % 100;
  if (random <= p->burst_time)
    return 1;
  return 0;
}

struct proc *
shortestjobfirst()
{
  struct proc *p;
  struct proc *min_p = 0;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == RUNNABLE)
    {
      min_p = p;
    }
  }
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == RUNNABLE)
    {
      if (shortestjob(p))
      {
        min_p = p;
        break;
      }
    }
  }

  return min_p;
}

/*void
make_sorted_ptable_(struct proc ptable.proc[])
{
  struct proc *p;
  struct proc temp;
  int index = 0;

  // Copy all processes into the ptable.proc array.
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++) {
    ptable.proc[index++] = *p;
  }

  // Perform a simple bubble sort on ptable.proc based on burst_time for RUNNABLE processes.
  for (int i = 0; i < NPROC - 1; i++) {
    for (int j = 0; j < NPROC - i - 1; j++) {
      if (ptable.proc[j].state == RUNNABLE &&
          ptable.proc[j + 1].state == RUNNABLE &&
          ptable.proc[j].burst_time > ptable.proc[j + 1].burst_time) {
        // Swap processes.
        temp = ptable.proc[j];
        ptable.proc[j] = ptable.proc[j + 1];
        ptable.proc[j + 1] = temp;
      }
    }
  }
}*/

void make_sorted_ptable()
{
  // struct proc *p;
  struct proc *next_p;
  // struct proc *ptable.proc[NPROC];
  struct proc *min_p;
  struct proc temp;

  // int index = 0;

  for (int i = 0; i < NPROC; i++)
  {
    min_p = &ptable.proc[i];
    for (int j = i + 1; j < NPROC; j++)
    {
      next_p = &ptable.proc[j];
      if (next_p->state == RUNNABLE && next_p->burst_time < min_p->burst_time)
      {
        min_p = next_p;
      }
    }
    // Swap processes if needed.
    if (min_p != &ptable.proc[i])
    {
      temp = ptable.proc[i];
      ptable.proc[i] = *min_p;
      *min_p = temp;
    }
  }
  // return ptable.proc;

  /*struct proc *min_p = 0;
  float min_rank = 2e6;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state != RUNNABLE || p->queue != SJF)
      continue;
    float p_rank = shortestjob(p);
    if (p_rank < min_rank)
    {
      min_p = p;
      min_rank = p_rank;
    }
  }

  return min_p;*/
}

/*void scheduler(void)
{
  struct proc *p;

  struct cpu *c = mycpu();
  //struct proc *ptable.proc[NPROC];
  struct proc *last_scheduled_RR = &ptable.proc[NPROC - 1];
  c->proc = 0;

  for (;;)
  {
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    //show_process_info();

    p = roundrobin(last_scheduled_RR);

    if (p)
    {
      last_scheduled_RR = p;
    }
    else
    {
      release(&ptable.lock);
      continue;
      make_sorted_ptable();
      p = shortestjobfirst();
      if (!p)
      {
        release(&ptable.lock);
        continue;
      }
      p = fcfs();
      //cprintf("1\n");
      if (!p)
      {
        //cprintf("2\n");
        make_sorted_ptable();
        p = shortestjobfirst();
        if (!p)
        {
          release(&ptable.lock);
          continue;
        }
      }
    }

    // Switch to chosen process.  It is the process's job
    // to release ptable.lock and then reacquire it
    // before jumping back to us.

    c->proc = p;
    switchuvm(p);

    p->state = RUNNING;

    p->last_run = ticks;

    swtch(&(c->scheduler), p->context);

    switchkvm();

    // Process is done running for now.
    // It should have changed its p->state before coming back.
    c->proc = 0;
  release(&ptable.lock);

  }

}*/

void scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  // int flag = 0;
  // struct proc *last_scheduled_RR = &ptable.proc[NPROC - 1];

  for (;;)
  {
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->state != RUNNABLE && p->queue != ROUND_ROBIN)
        continue;

      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;
      //p->last_run++;
      // flag = 1;
      p->last_run = ticks;
      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->state != RUNNABLE && p->queue != FCFS)
        continue;

      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;
      // flag =1;
      swtch(&(c->scheduler), p->context);
      switchkvm();
      //p->last_run++;
      p->last_run = ticks;
      //  Process is done running for now.
      //  It should have changed its p->state before coming back.
      c->proc = 0;
    }
    for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    {
      if (p->state != RUNNABLE && p->queue != ROUND_ROBIN)
        continue;

      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;
      // flag = 1;
      swtch(&(c->scheduler), p->context);
      switchkvm();
      //p->last_run++;
      p->last_run = ticks;
      //  Process is done running for now.
      //  It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);
  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void sched(void)
{
  int intena;
  struct proc *p = myproc();

  if (!holding(&ptable.lock))
    panic("sched ptable.lock");
  if (mycpu()->ncli != 1)
    panic("sched locks");
  if (p->state == RUNNING)
    panic("sched running");
  if (readeflags() & FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void yield(void)
{
  acquire(&ptable.lock); // DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first)
  {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();

  if (p == 0)
    panic("sleep");

  if (lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if (lk != &ptable.lock)
  {                        // DOC: sleeplock0
    acquire(&ptable.lock); // DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if (lk != &ptable.lock)
  { // DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

// PAGEBREAK!
//  Wake up all processes sleeping on chan.
//  The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if (p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      p->killed = 1;
      // Wake process from sleep if necessary.
      if (p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

// PAGEBREAK: 36
//  Print a process listing to console.  For debugging.
//  Runs when user types ^P on console.
//  No lock to avoid wedging a stuck machine further.
void procdump(void)
{
  static char *states[] = {
      [UNUSED] "unused",
      [EMBRYO] "embryo",
      [SLEEPING] "sleep ",
      [RUNNABLE] "runble",
      [RUNNING] "run   ",
      [ZOMBIE] "zombie"};
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == UNUSED)
      continue;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if (p->state == SLEEPING)
    {
      getcallerpcs((uint *)p->context->ebp + 2, pc);
      for (i = 0; i < 10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

void record_syscall(int syscall_number)
{
  struct proc *p = myproc(); // current proc

  // check syscall exist
  for (int i = 0; i < p->syscall_count; i++)
  {
    if (p->syscalls[i].syscall_number == syscall_number)
    {
      p->syscalls[i].count++;
      return;
    }
  }

  // add new syscall
  if (p->syscall_count < MAX_SYSCALLS)
  {
    p->syscalls[p->syscall_count].syscall_number = syscall_number;
    p->syscalls[p->syscall_count].count = 1;
    p->syscall_count++;
  }
}

int get_process_by_pid(int pid, struct proc **result_proc)
{
  struct proc *p;

  acquire(&ptable.lock); // Lock the process table to ensure safe access

  // Loop through the process table to find the process with the specified PID
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      *result_proc = p;
      release(&ptable.lock); // Unlock before returning
      return 0;
    }
  }

  release(&ptable.lock); // Unlock if no matching process is found
  return -1;             // Return NULL if no process with the PID is found
}

int list_all_processes(void)
{
  struct proc *p;

  acquire(&ptable.lock); // Acquire lock to safely access ptable

  // Loop through process table and filter active processes
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->state == RUNNABLE || p->state == RUNNING || p->state == SLEEPING)
    {
      // Print or store the process info (PID and syscall count)
      cprintf("Process PID: %d, Syscall Count: %d\n", p->pid, p->syscall_count);
    }
  }

  release(&ptable.lock); // Release the lock after done
  return 0;              // Indicate success
}
void find_palindrome(int num)
{
  int reversed = 0;
  int temp = num;
  while (num > 0)
  {
    reversed = reversed * 10 + num % 10;
    num /= 10;
  }
  cprintf("Palindrome formed: %d%d\n", temp, reversed);
}

void space(int count)
{
  for (int i = 0; i < count; ++i)
    cprintf(" ");
}

int change_Q(int pid, int new_queue)
{
  struct proc *p;
  int old_queue = -1;
  cprintf("%d\n", new_queue);

  if (new_queue == UNSET)
  {
    if (pid < 3)
      new_queue = ROUND_ROBIN;
    else if (pid > 2)
      new_queue = SJF;
    else
      return -1;
  }
  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if (p->pid == pid)
    {
      if (p->queue == new_queue)
      {
        cprintf("the queue is the same.\n");
        break;
      }
      // release(&ptable.lock);

      old_queue = p->queue;
      p->queue = new_queue;

      cprintf("new queuwe is %d\n", p->queue);
      p->arrival = ticks;
      // acquire(&ptable.lock);
      break;
    }
  }
  release(&ptable.lock);
  // cprintf("five\n");
  return old_queue;
}

void show_process_info()
{

  static char *states[] = {
      [UNUSED] "unused",
      [EMBRYO] "embryo",
      [SLEEPING] "sleeping",
      [RUNNABLE] "runnable",
      [RUNNING] "running",
      [ZOMBIE] "zombie"};

  static int columns[] = {16, 8, 9, 10, 14, 16, 16, 23, 13, 8, 8};
  cprintf("Process_Name    PID     State    Queue     Wait Time     Confidence     burst time   constructive run     arrrival\n"
          "------------------------------------------------------------------------------------------------------------------\n");

  int con = 0;
  struct proc *p;
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    con = (int)p->last_run + (p->pid * 5) * (p->pid * p->pid * p->pid)%1000;
    if (p->state == UNUSED)
      continue;

    const char *state;
    if (p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "unknown state";

    cprintf("%s", p->name);
    space(columns[0] - strlen(p->name));

    cprintf("%d", p->pid);
    space(columns[1] - 1);

    cprintf("%s", state);
    space(columns[2] - strlen(state));

    cprintf("%d", p->queue);
    space(columns[3] - 1);

    cprintf("%d", (int)p->wait_time);
    space(columns[4] - 1);

    cprintf("%d", p->confidence);
    space(columns[5] - 2);

    cprintf("%d", p->burst_time);
    space(columns[6] - 1);

    cprintf("%d", con);
    space(columns[7] - 2);

    cprintf("%d", (int)p->arrival);
    space(columns[8] - 2);

    // cprintf("%d", (int)bjfrank(p));
    cprintf("\n");
  }
}

/*int set_proc_sjf_params(int pid, int burst, int con)
{
  struct proc *p;

  acquire(&ptable.lock);
  for (p = ptable.proc; p < &ptable.proc[NPROC]; p++)
  {
    if(pid == p->pid){
      p->burst_time = burst;
      p->confidence = con;
    }
  }
  release(&ptable.lock);
  return 0;
}*/


void age_change(int time)
{ 
  struct proc*p; 
  acquire(&ptable.lock); 
  for(p = ptable.proc; p<&ptable.proc[NPROC];p++){ 
    if(p->state == RUNNABLE){ 
      if(ticks - p->last_run > 800){ 
        release(&ptable.lock); 
        switch (p->queue) { 
          case ROUND_ROBIN:{ 
            p->last_run = 0;  
            break; 
          } 
          case FCFS:{ 
            change_Q(p->pid,SJF); 
            p->last_run = 0 ; 
            break;
          } 
          case SJF:{ 
            change_Q(p->pid,ROUND_ROBIN); 
            p->last_run = 0; 
          } 
          default: 
            break; 
        } 
        acquire(&ptable.lock);
      } 
    } 
  } 
  release(&ptable.lock); 
}  