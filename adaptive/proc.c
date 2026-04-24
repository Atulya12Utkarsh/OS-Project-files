#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"

struct cpu cpus[NCPU];

struct proc proc[NPROC];

struct proc *initproc;

int nextpid = 1;
struct spinlock pid_lock;

extern void forkret(void);
static void freeproc(struct proc *p);

extern char trampoline[]; // trampoline.S

// helps ensure that wakeups of wait()ing
// parents are not lost. helps obey the
// memory model when using p->parent.
// must be acquired before any p->lock.
struct spinlock wait_lock;

// ============================================================
// Adaptive Scheduler Constants
//
// All priority values are integers — no floats used anywhere.
// This keeps the code compatible with xv6's kernel environment.
//
// Conceptual mapping to the project pseudocode:
//   estimated_runtime  ↔  task.weight (initial priority)
//   priority           ↔  task.effective_priority (decreases with aging)
//   age_start          ↔  task.arrival_tick
//   ticks - age_start  ↔  wait_ticks
//
// AGING_RATE controls how fast priority decays while waiting.
// At 10ms per tick and AGING_RATE = 10:
//   After 500 ticks (~5 seconds) a process at PRIORITY_DEFAULT
//   reaches PRIORITY_MIN and gets maximum scheduling urgency.
// ============================================================

#define PRIORITY_DEFAULT   5000   // Starting priority for all new processes
#define PRIORITY_MAX       10000  // Highest priority value (least urgent)
#define PRIORITY_MIN       0      // Lowest priority value (most urgent — runs first)
#define AGING_RATE         10     // Priority reduction per tick of waiting
                                  // Effective priority = priority - (wait * AGING_RATE)


// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.
void
proc_mapstacks(pagetable_t kpgtbl)
{
  struct proc *p;
  
  for(p = proc; p < &proc[NPROC]; p++) {
    char *pa = kalloc();
    if(pa == 0)
      panic("kalloc");
    uint64 va = KSTACK((int) (p - proc));
    kvmmap(kpgtbl, va, (uint64)pa, PGSIZE, PTE_R | PTE_W);
  }
}

// initialize the proc table.
void
procinit(void)
{
  struct proc *p;
  
  initlock(&pid_lock, "nextpid");
  initlock(&wait_lock, "wait_lock");
  for(p = proc; p < &proc[NPROC]; p++) {
      initlock(&p->lock, "proc");
      p->state = UNUSED;
      p->kstack = KSTACK((int) (p - proc));
  }
}

// Must be called with interrupts disabled,
// to prevent race with process being moved
// to a different CPU.
int
cpuid()
{
  int id = r_tp();
  return id;
}

// Return this CPU's cpu struct.
// Interrupts must be disabled.
struct cpu*
mycpu(void)
{
  int id = cpuid();
  struct cpu *c = &cpus[id];
  return c;
}

// Return the current struct proc *, or zero if none.
struct proc*
myproc(void)
{
  push_off();
  struct cpu *c = mycpu();
  struct proc *p = c->proc;
  pop_off();
  return p;
}

int
allocpid()
{
  int pid;
  
  acquire(&pid_lock);
  pid = nextpid;
  nextpid = nextpid + 1;
  release(&pid_lock);

  return pid;
}

// Look in the process table for an UNUSED proc.
// If found, initialize state required to run in the kernel,
// and return with p->lock held.
// If there are no free procs, or a memory allocation fails, return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    acquire(&p->lock);
    if(p->state == UNUSED) {
      goto found;
    } else {
      release(&p->lock);
    }
  }
  return 0;

found:
  p->pid = allocpid();
  p->state = USED;
  acquire(&tickslock);
  p->ctime = ticks;
  release(&tickslock);
  p->rtime = 0;
  p->etime = 0;
  p->nswtch=0;
  // ---- Initialise adaptive scheduling fields ----
  //
  // estimated_runtime: default medium priority.
  // A process that the kernel or user knows is short should have a
  // lower value here so it gets scheduled sooner (SJF principle).
  p->estimated_runtime = PRIORITY_DEFAULT;

  // priority: starts equal to estimated_runtime.
  // The scheduler reduces this via aging the longer the process waits.
  p->priority = PRIORITY_DEFAULT;

  // age_start: records when this process first became ready to run.
  // We read ticks directly (without tickslock) for an approximate value —
  // a one-tick inaccuracy is fine for scheduling decisions.
  p->age_start = (uint64)ticks;

  // affinity_cpu: -1 means no CPU preference (work-stealing default).
  p->affinity_cpu = -1;
  // ---- End scheduling fields ----

  // Allocate a trapframe page.
  if((p->trapframe = (struct trapframe *)kalloc()) == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // An empty user page table.
  p->pagetable = proc_pagetable(p);
  if(p->pagetable == 0){
    freeproc(p);
    release(&p->lock);
    return 0;
  }

  // Set up new context to start executing at forkret,
  // which returns to user space.
  memset(&p->context, 0, sizeof(p->context));
  p->context.ra = (uint64)forkret;
  p->context.sp = p->kstack + PGSIZE;

  return p;
}

// free a proc structure and the data hanging from it,
// including user pages.
// p->lock must be held.
static void
freeproc(struct proc *p)
{
  if(p->trapframe)
    kfree((void*)p->trapframe);
  p->trapframe = 0;
  if(p->pagetable)
    proc_freepagetable(p->pagetable, p->sz);
  p->pagetable = 0;
  p->sz = 0;
  p->pid = 0;
  p->parent = 0;
  p->name[0] = 0;
  p->chan = 0;
  p->killed = 0;
  p->xstate = 0;
  p->state = UNUSED;
}

// Create a user page table for a given process, with no user memory,
// but with trampoline and trapframe pages.
pagetable_t
proc_pagetable(struct proc *p)
{
  pagetable_t pagetable;

  // An empty page table.
  pagetable = uvmcreate();
  if(pagetable == 0)
    return 0;

  // map the trampoline code (for system call return)
  // at the highest user virtual address.
  // only the supervisor uses it, on the way
  // to/from user space, so not PTE_U.
  if(mappages(pagetable, TRAMPOLINE, PGSIZE,
              (uint64)trampoline, PTE_R | PTE_X) < 0){
    uvmfree(pagetable, 0);
    return 0;
  }

  // map the trapframe page just below the trampoline page, for
  // trampoline.S.
  if(mappages(pagetable, TRAPFRAME, PGSIZE,
              (uint64)(p->trapframe), PTE_R | PTE_W) < 0){
    uvmunmap(pagetable, TRAMPOLINE, 1, 0);
    uvmfree(pagetable, 0);
    return 0;
  }

  return pagetable;
}

// Free a process's page table, and free the
// physical memory it refers to.
void
proc_freepagetable(pagetable_t pagetable, uint64 sz)
{
  uvmunmap(pagetable, TRAMPOLINE, 1, 0);
  uvmunmap(pagetable, TRAPFRAME, 1, 0);
  uvmfree(pagetable, sz);
}

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  initproc = p;
  
  p->cwd = namei("/");

  // Mark as runnable and record when it became runnable for aging.
  p->age_start = (uint64)ticks;
  p->state = RUNNABLE;

  release(&p->lock);
}

// Grow or shrink user memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint64 sz;
  struct proc *p = myproc();

  sz = p->sz;
  if(n > 0){
    if(sz + n > TRAPFRAME) {
      return -1;
    }
    if((sz = uvmalloc(p->pagetable, sz, sz + n, PTE_W)) == 0) {
      return -1;
    }
  } else if(n < 0){
    sz = uvmdealloc(p->pagetable, sz, sz + n);
  }
  p->sz = sz;
  return 0;
}

// Create a new process, copying the parent.
// Sets up child kernel stack to return as if from fork() system call.
int
kfork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *p = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy user memory from parent to child.
  if(uvmcopy(p->pagetable, np->pagetable, p->sz) < 0){
    freeproc(np);
    release(&np->lock);
    return -1;
  }
  np->sz = p->sz;

  // copy saved user registers.
  *(np->trapframe) = *(p->trapframe);

  // Cause fork to return 0 in the child.
  np->trapframe->a0 = 0;

  // increment reference counts on open file descriptors.
  for(i = 0; i < NOFILE; i++)
    if(p->ofile[i])
      np->ofile[i] = filedup(p->ofile[i]);
  np->cwd = idup(p->cwd);

  safestrcpy(np->name, p->name, sizeof(p->name));

  pid = np->pid;

  // ---- Inherit / reset scheduling fields for the child ----
  //
  // The child inherits estimated_runtime from the parent as a hint
  // about expected workload. Priority and age_start are reset so the
  // child starts fresh in the scheduler rather than inheriting the
  // parent's current waiting position.
  np->estimated_runtime = p->estimated_runtime;
  np->priority          = np->estimated_runtime; // fresh priority
  np->age_start         = (uint64)ticks;          // reset wait timer
  np->affinity_cpu      = -1;                     // no CPU affinity by default
  // ---- End scheduling fields ----

  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  acquire(&np->lock);
  np->state = RUNNABLE;
  release(&np->lock);

  return pid;
}

// Pass p's abandoned children to init.
// Caller must hold wait_lock.
void
reparent(struct proc *p)
{
  struct proc *pp;

  for(pp = proc; pp < &proc[NPROC]; pp++){
    if(pp->parent == p){
      pp->parent = initproc;
      wakeup(initproc);
    }
  }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait().
void
kexit(int status)
{
  struct proc *p = myproc();
  acquire(&tickslock);
  p->etime=ticks;
  release(&tickslock);
  if(p == initproc)
    panic("init exiting");

  // Close all open files.
  for(int fd = 0; fd < NOFILE; fd++){
    if(p->ofile[fd]){
      struct file *f = p->ofile[fd];
      fileclose(f);
      p->ofile[fd] = 0;
    }
  }

  begin_op();
  iput(p->cwd);
  end_op();
  p->cwd = 0;

  acquire(&wait_lock);

  // Give any children to init.
  reparent(p);

  // Parent might be sleeping in wait().
  wakeup(p->parent);
  
  acquire(&p->lock);

  p->xstate = status;
  p->state = ZOMBIE;

  release(&wait_lock);

  // Jump into the scheduler, never to return.
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
kwait(uint64 addr)
{
  struct proc *pp;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(pp = proc; pp < &proc[NPROC]; pp++){
      if(pp->parent == p){
        // make sure the child isn't still in exit() or swtch().
        acquire(&pp->lock);

        havekids = 1;
        if(pp->state == ZOMBIE){
          // Found one.
          pid = pp->pid;
          if(addr != 0 && copyout(p->pagetable, addr, (char *)&pp->xstate,
                                  sizeof(pp->xstate)) < 0) {
            release(&pp->lock);
            release(&wait_lock);
            return -1;
          }
          freeproc(pp);
          release(&pp->lock);
          release(&wait_lock);
          return pid;
        }
        release(&pp->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || killed(p)){
      release(&wait_lock);
      return -1;
    }
    
    // Wait for a child to exit.
    sleep(p, &wait_lock);  //DOC: wait-sleep
  }
}

// ============================================================
// scheduler()  —  Adaptive SJF + Aging Scheduler
//
// Replaces the original Round-Robin with our algorithm.
//
// Algorithm on each iteration:
//   1. Scan all processes in proc[].
//   2. For each RUNNABLE process, compute its effective priority:
//        effective = priority - (wait_ticks * AGING_RATE)
//        clamped to [PRIORITY_MIN, PRIORITY_MAX]
//      A lower effective priority means higher scheduling urgency.
//   3. Select the process with the lowest effective priority (most urgent).
//   4. Run it.
//
// Why this implements adaptive SJF:
//   - Processes with low estimated_runtime (short jobs) start with
//     low priority values and get scheduled first — SJF behaviour.
//   - As a process waits longer, AGING_RATE * wait_ticks reduces its
//     priority further — starvation prevention.
//   - When the adaptive monitor (user-space component) adjusts
//     AGING_RATE via a syscall, the kernel scheduler automatically
//     changes how aggressively it promotes waiting processes.
//
// Locking protocol:
//   We scan proc[] acquiring one lock at a time.
//   When we find a better candidate we release the old best's lock
//   and keep the new best's lock — we NEVER hold two proc locks
//   simultaneously, which prevents deadlock.
//
//   After selecting a process:
//     - Set state = RUNNING with lock held
//     - swtch() to the process (it releases p->lock internally via sched())
//     - On return, release chosen->lock
// ============================================================
void
scheduler(void)
{
  struct proc *p;
  struct proc *chosen;       // best candidate found so far
  struct cpu  *c = mycpu();
  int curr_pri;              // effective priority of current candidate
  int best_pri;              // effective priority of best candidate so far
  uint64 wait;               // ticks this process has been waiting

  c->proc = 0;

  for(;;) {
    // Enable interrupts briefly so devices can make progress,
    // then disable before the scheduling decision to avoid races.
    intr_on();
    intr_off();

    chosen   = 0;
    best_pri = PRIORITY_MAX + 1;   // sentinel: worse than any real priority

    // ---- Scan all processes to find the most urgent one ----
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);

      if(p->state == RUNNABLE) {

        // Compute how long this process has been waiting since it last
        // became RUNNABLE (either via yield, wakeup, or initial creation).
        wait = (uint64)ticks - p->age_start;

        // Effective priority decreases (becomes more urgent) the longer
        // the process waits. Integer arithmetic — no floats.
        //   curr_pri = p->priority - (wait * AGING_RATE)
        // Clamped at PRIORITY_MIN so it cannot go negative.
        curr_pri = p->priority - (int)((uint)wait * AGING_RATE);
        if(curr_pri < PRIORITY_MIN)
          curr_pri = PRIORITY_MIN;

        // Is this process more urgent than our current best candidate?
        if(curr_pri < best_pri) {
          // Release the previous best's lock before taking this one.
          // This ensures we never hold two proc locks at once.
          if(chosen != 0)
            release(&chosen->lock);

          chosen   = p;
          best_pri = curr_pri;
          // Keep p->lock held — it now protects 'chosen'

        } else {
          // Not better — release immediately and move on
          release(&p->lock);
        }

      } else {
        // Not RUNNABLE — not a candidate
        release(&p->lock);
      }
    }

    // ---- Run the chosen process (if any) ----
    if(chosen != 0) {
      // chosen->lock is still held from the scan above.

      // When this process gets the CPU, reset its priority to
      // estimated_runtime so it starts fresh next time it becomes
      // RUNNABLE. This prevents a process from permanently monopolising
      // the CPU just because aging drove its priority to zero.
      chosen->priority = chosen->estimated_runtime;

      // Transition to RUNNING and context-switch into the process.
      chosen->state = RUNNING;
      c->proc       = chosen;
      swtch(&c->context, &chosen->context);

      // Control returns here after the process yields, sleeps, or exits.
      // The process changed its own state before calling sched().
      c->proc = 0;

      release(&chosen->lock);

    } else {
      // No runnable process found — halt this CPU until the next interrupt.
      // wfi (wait for interrupt) is the RISC-V low-power idle instruction.
      asm volatile("wfi");
    }
  }
}

// Switch to scheduler.  Must hold only p->lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->noff, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&p->lock))
    panic("sched p->lock");
  if(mycpu()->noff != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched RUNNING");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  p->nswtch++;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
// Modified to reset the adaptive scheduling fields when a process yields,
// so the next time it competes for the CPU it starts with a fresh priority
// and a fresh age_start timer.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);

  // Reset priority to estimated_runtime so this process is treated as
  // a fresh competitor next round rather than carrying over the aged
  // (artificially low) priority from its previous wait.
  p->priority = p->estimated_runtime;

  // Record the tick at which this process returned to the ready queue.
  // The scheduler will compute (ticks - age_start) to determine wait time.
  p->age_start = (uint64)ticks;

  p->state = RUNNABLE;
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  extern char userret[];
  static int first = 1;
  struct proc *p = myproc();

  // Still holding p->lock from scheduler.
  release(&p->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();

    // We can invoke kexec() now that file system is initialized.
    // Put the return value (argc) of kexec into a0.
    p->trapframe->a0 = kexec("/init", (char *[]){ "/init", 0 });
    if (p->trapframe->a0 == -1) {
      panic("exec");
    }
  }

  // return to user space, mimicing usertrap()'s return.
  prepare_return();
  uint64 satp = MAKE_SATP(p->pagetable);
  uint64 trampoline_userret = TRAMPOLINE + (userret - trampoline);
  ((void (*)(uint64))trampoline_userret)(satp);
}

// Sleep on channel chan, releasing condition lock lk.
// Re-acquires lk when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  // Must acquire p->lock in order to
  // change p->state and then call sched.
  // Once we hold p->lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup locks p->lock),
  // so it's okay to release lk.

  acquire(&p->lock);  //DOC: sleeplock1
  release(lk);

  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  release(&p->lock);
  acquire(lk);
}

// Wake up all processes sleeping on channel chan.
// Caller should hold the condition lock.
// Modified: records age_start when a process transitions to RUNNABLE
// so the adaptive scheduler can correctly compute its wait time.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {

        // Record when this process entered the RUNNABLE state.
        // The scheduler will use (ticks - age_start) as the wait_ticks
        // for the aging calculation.
        p->age_start = (uint64)ticks;

        // Also reset priority to estimated_runtime so a just-woken
        // process starts with its base priority and competes fairly.
        p->priority = p->estimated_runtime;

        p->state = RUNNABLE;
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kkill(int pid)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++){
    acquire(&p->lock);
    if(p->pid == pid){
      p->killed = 1;
      if(p->state == SLEEPING){
        // Wake process from sleep().
        p->state = RUNNABLE;
      }
      release(&p->lock);
      return 0;
    }
    release(&p->lock);
  }
  return -1;
}

void
setkilled(struct proc *p)
{
  acquire(&p->lock);
  p->killed = 1;
  release(&p->lock);
}

int
killed(struct proc *p)
{
  int k;
  
  acquire(&p->lock);
  k = p->killed;
  release(&p->lock);
  return k;
}

// Copy to either a user address, or kernel address,
// depending on usr_dst.
// Returns 0 on success, -1 on error.
int
either_copyout(int user_dst, uint64 dst, void *src, uint64 len)
{
  struct proc *p = myproc();
  if(user_dst){
    return copyout(p->pagetable, dst, src, len);
  } else {
    memmove((char *)dst, src, len);
    return 0;
  }
}

// Copy from either a user address, or kernel address,
// depending on usr_src.
// Returns 0 on success, -1 on error.
int
either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
  struct proc *p = myproc();
  if(user_src){
    return copyin(p->pagetable, dst, src, len);
  } else {
    memmove(dst, (char*)src, len);
    return 0;
  }
}

// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [USED]      "used",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  struct proc *p;
  char *state;

  printf("\n");
  for(p = proc; p < &proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    printf("%d %s %s", p->pid, state, p->name);
    printf("\n");
  }
}

// --- CUSTOM METRIC: CPU TIME TRACKER ---
// This will be called on every timer interrupt
void update_process_times() {
  struct proc *p=myproc();
  if (p !=0) {
    acquire(&p->lock);
    if(p->state == RUNNING) {
      p->rtime++;
    }
    release(&p->lock);
  }
}

// --- CUSTOM METRIC: WAITX SYSTEM CALL ---
// Functions exactly like wait(), but calculates and returns our metrics
int waitx(uint64 addr_wtime, uint64 addr_rtime,uint64 addr_nswtch) {
  struct proc *np;
  int havekids, pid;
  struct proc *p = myproc();

  acquire(&wait_lock);

  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(np = proc; np < &proc[NPROC]; np++){
      if(np->parent == p){
        acquire(&np->lock);
        havekids = 1;
        if(np->state == ZOMBIE){
          // Found an exited child
          pid = np->pid;

          // CALCULATE METRICS
          int turnaround_time = np->etime - np->ctime;
          int run_time = np->rtime;
          int wait_time = turnaround_time - run_time;
          int num_switches=np->nswtch;
          // Copy the metrics back to user-space pointers
          if(addr_wtime != 0){
            copyout(p->pagetable, addr_wtime, (char *)&wait_time, sizeof(wait_time));
          }
          if(addr_rtime != 0){
            copyout(p->pagetable, addr_rtime, (char *)&run_time, sizeof(run_time));
          }
          if(addr_nswtch != 0){
            copyout(p->pagetable, addr_nswtch, (char *)&num_switches, sizeof(num_switches));
          }
          freeproc(np);
          release(&np->lock);
          release(&wait_lock);
          return pid;
        }
        release(&np->lock);
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || p->killed){
      release(&wait_lock);
      return -1;
    }
    // Wait for a child to exit.
    sleep(p, &wait_lock);  
  }
}
