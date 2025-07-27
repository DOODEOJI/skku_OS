#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "kalloc.h"

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

// Allocate a page for each process's kernel stack.
// Map it high in memory, followed by an invalid
// guard page.

int weight_arr[40] = {
	/*  0 */  88761, 71755, 56483, 46273, 36291,
	/*  5 */  29154, 23254, 18705, 14949, 11916,
	/* 10 */   9548,  7620,  6100,  4904,  3906,
	/* 15 */   3121,  2501,  1991,  1586,  1277,
	/* 20 */   1024,   820,   655,   526,   423,
	/* 25 */    335,   272,   215,   172,   137,
	/* 30 */    110,    87,    70,    56,    45,
	/* 35 */     36,    29,    23,    18,    15,};

int global_tick = 0;
int wsum = 0;

struct mmap_area mmap_area[64];

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
  
  p->nice_value = 20;
  p->running_ticks = 0;
  p->vruntime = 0;
  p->vdeadline = p->vruntime + TIMESLICE * 1000 * weight_arr[20] / p->nice_value;
  p->is_eligible = 1;
  p->time_slice = TIMESLICE - 1;
  
  if (!holding(&p->lock)) printf("already released lock\n");
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
  p->nice_value = 0;
  p->running_ticks = 0;
  p->vdeadline = 0;
  p->is_eligible = 0;
  p->time_slice = 0;
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

// a user program that calls exec("/init")
// assembled from ../user/initcode.S
// od -t xC ../user/initcode
uchar initcode[] = {
  0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
  0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
  0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
  0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
  0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
  0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
  0x00, 0x00, 0x00, 0x00
};

// Set up first user process.
void
userinit(void)
{
  struct proc *p;

  p = allocproc();
  if (p==0) printf("allocproc failed");
  if (!holding(&p->lock)) printf("already released lock\n");
  initproc = p;
  
  // allocate one user page and copy initcode's instructions
  // and data into it.
  uvmfirst(p->pagetable, initcode, sizeof(initcode));
  p->sz = PGSIZE;

  // prepare for the very first "return" from kernel to user.
  p->trapframe->epc = 0;      // user program counter
  p->trapframe->sp = PGSIZE;  // user stack pointer

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  p->state = RUNNABLE;
  if (!holding(&p->lock)) {
    printf("Not holding p->lock in userinit before release!\n");
  }	 
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
fork(void)
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
  np->nice_value = p->nice_value;

  np->vruntime = p->vruntime;
  np->vdeadline = np->vruntime + TIMESLICE * weight_arr[20] / weight_arr[np->nice_value];

  pid = np->pid;
  np->state = RUNNABLE;
  
  if (!holding(&np->lock)) printf("no lock in pid : %d/n",np->pid);
  release(&np->lock);

  acquire(&wait_lock);
  np->parent = p;
  release(&wait_lock);

  //acquire(&np->lock);
  //np->state = RUNNABLE;
  //release(&np->lock);

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
exit(int status)
{
  struct proc *p = myproc();

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
wait(uint64 addr)
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

// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run.
//  - swtch to start running that process.
//  - eventually that process transfers control
//    via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p0;
  struct proc *p1;
  struct proc *p;
  struct proc *min_p = 0;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    intr_on();

    int min_vruntime = 1 << 30;
    int min_vdeadline = 1 << 30;
    int vsum = 0;

    wsum = 0;
    for(p0 = proc; p0 < &proc[NPROC]; p0++){
      if (p0->state != RUNNABLE) continue;

      wsum += weight_arr[p0->nice_value];

      if (p0->vruntime < min_vruntime) min_vruntime = p0->vruntime;
    }
    for(p1 = proc; p1 < &proc[NPROC]; p1++){
      if (p0->state != RUNNABLE) continue;
      vsum += (p1->vruntime - min_vruntime) * weight_arr[p1->nice_value];
    }

    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting.

    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      if (p->state != RUNNABLE){
        continue;
      }
      
      acquire(&p->lock);

      if (vsum < (p->vruntime - min_vruntime) * wsum){
        p->is_eligible = 0;
        release(&p->lock);
	      continue;
      }

      p->is_eligible = 1;

      if (p->vdeadline < min_vdeadline){
        min_p = p;
	      min_vdeadline = p->vdeadline;
      }
      release(&p->lock);
    }
    // Switch to chosen process.  It is the process's job
    // to release its lock and then reacquire it
    // before jumping back to us.
    if (min_p){
      acquire(&min_p->lock);
      min_p->state = RUNNING;
      c->proc = min_p;

      swtch(&c->context, &min_p->context);

      // Process is done running for now.
      // // It should have changed its p->state before coming back.
      c->proc = 0;
      found = 1;
      release(&min_p->lock);
    }

    if(found == 0) {
      // nothing to run; stop running on this core until an interrupt.
      intr_on();
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
    panic("sched running");
  if(intr_get())
    panic("sched interruptible");

  intena = mycpu()->intena;
  swtch(&p->context, &mycpu()->context);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  struct proc *p = myproc();
  acquire(&p->lock);
  p->state = RUNNABLE;
  p->vdeadline = p->vruntime + TIMESLICE * 1000 * weight_arr[20] / weight_arr[p->nice_value]; 
  sched();
  release(&p->lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch to forkret.
void
forkret(void)
{
  static int first = 1;

  // Still holding p->lock from scheduler.
  release(&myproc()->lock);

  if (first) {
    // File system initialization must be run in the context of a
    // regular process (e.g., because it calls sleep), and thus cannot
    // be run from main().
    fsinit(ROOTDEV);

    first = 0;
    // ensure other cores see first=0.
    __sync_synchronize();
  }

  usertrapret();
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
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

// Wake up all processes sleeping on chan.
// Must be called without any p->lock.
void
wakeup(void *chan)
{
  struct proc *p;

  for(p = proc; p < &proc[NPROC]; p++) {
    if(p != myproc()){
      acquire(&p->lock);
      if(p->state == SLEEPING && p->chan == chan) {
        p->state = RUNNABLE;
	      p->time_slice = TIMESLICE - 1;
        p->vdeadline = p->vruntime + TIMESLICE * 1000 * weight_arr[20] / weight_arr[p->nice_value];
      }
      release(&p->lock);
    }
  }
}

// Kill the process with the given pid.
// The victim won't exit until it tries to return
// to user space (see usertrap() in trap.c).
int
kill(int pid)
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

void
ps(int pid)
{
  static char *states[] ={
  [UNUSED]    "UNUSED",
  [USED]      "USED",
  [SLEEPING]  "SLEEPING",
  [RUNNABLE]  "RUNNABLE",
  [RUNNING]   "RUNNING",
  [ZOMBIE]    "ZOMBIE"
  };
  
  int lens(char* s){
    int len = 0;
    while(*s){
      len++;
      s++;
    }
    return len;
  }

  int leni(int i){
    int len = 0;
    if (i == 0) return 1;
    while (i > 0){
      len++;
      i/=10;
    }
    return len;
  }

  void print_header(int name_r, int rt_r, int vrt_r, int vdl_r){
    printf("name  ");
    for(int i=0;i<(name_r-4);i++) printf(" ");
    printf("pid      ");
    printf("state       ");
    printf("priority    ");
    printf("runtime/weight    ");
    printf("runtime");
    for(int i=0;i<(rt_r-7);i++) printf(" ");
    printf("vruntime");
    for(int i=0;i<(vrt_r-8);i++) printf(" ");
    printf("vdeadline");
    for(int i=0;i<(vdl_r-9);i++) printf(" ");
    printf("is_eligible");
    for(int i=0;i<2;i++) printf(" ");
    printf("tick %d",global_tick*1000);
    printf("\n");  
  }

  void print_proc_info(int pid){
    struct proc *p;

    int max_name_length = 0;
    int max_runtime_length = 0;
    int max_vruntime_length = 0;
    int max_vrdeadline_length = 0;

    for (p=proc; p < &proc[NPROC]; p++){
      if (p->state == 0) continue;
      if (lens(p->name)>max_name_length) max_name_length = lens(p->name);
      if (leni(p->running_ticks * 1000)>max_runtime_length) max_runtime_length = leni(p->running_ticks * 1000);
      if (leni(p->vruntime)>max_vruntime_length) max_vruntime_length = leni(p->vruntime);
      if (leni(p->vdeadline)>max_vrdeadline_length) max_vrdeadline_length = leni(p->vdeadline);
      

    max_runtime_length = max_runtime_length < 6 ? 6 : max_runtime_length;
    max_vruntime_length = max_vruntime_length < 6 ? 6 : max_vruntime_length;
    max_vrdeadline_length = max_vrdeadline_length < 6 ? 6 : max_vrdeadline_length;

    #define ROUND_UP6(x) ((((x) / 6) + 1) * 6)

    int name_range = ROUND_UP6(max_name_length);
    int rt_range   = ROUND_UP6(max_runtime_length);
    int vrt_range  = ROUND_UP6(max_vruntime_length);
    int vdl_range  = ROUND_UP6(max_vrdeadline_length);

    print_header(name_range, rt_range, vrt_range, vdl_range);
    
    for (p=proc; p < &proc[NPROC]; p++){
      if (pid == 0 || pid == p->pid){
        if (p->state == UNUSED) continue;

        printf("%s",p->name);
        int l1 = lens(p->name);
        for(int i=0;i<(name_range-l1+2);i++) printf(" ");
          
        printf("%d",p->pid);
        int l2 = leni(p->pid);
        for(int i=0;i<(9-l2);i++) printf(" ");

        printf("%s",states[p->state]);
        int l3 = lens(states[p->state]);
        for(int i=0;i<(12-l3);i++) printf(" ");

        printf("%d",p->nice_value);
        int l4 = leni(p->nice_value);
        for(int i=0;i<(12-l4);i++) printf(" ");

        printf("%d",p->running_ticks * 1000/weight_arr[p->nice_value]); 
        int l5 = leni(p->running_ticks * 1000/weight_arr[p->nice_value]);
        for(int i=0;i<(18-l5);i++) printf(" ");

        printf("%d",p->running_ticks * 1000);
        int l6 = leni(p->running_ticks * 1000);
        for(int i=0;i<(rt_range-l6);i++) printf(" ");

        printf("%d",p->vruntime);
        int l7 = leni(p->vruntime);
        for(int i=0;i<(vrt_range-l7);i++) printf(" ");

        printf("%d",p->vdeadline);
        int l8 = leni(p->vdeadline);
        for(int i=0;i<(vdl_range-l8);i++) printf(" ");

        if (p->is_eligible == 1) printf("True");
        else printf("False");
        printf("\n");
        }
      }
    }
  }

  if (pid < 0){
    printf("Don't input negative value\n");
  }

  else if(pid ==0) {
    print_proc_info(pid);
  }
  else{
    print_proc_info(pid);
  }
}

int
getnice(int pid)
{
  struct proc *p;
  for (p=proc; p < &proc[NPROC]; p++){
    if (p -> pid == pid) return p->nice_value;
  }
  return -1;
}

int
setnice(int pid, int value)
{
  struct proc *p;

  if (value < 0 || value > 39){
	  printf("nice_value는 0~39 사이의 값이어야합니다.\n");
	  return -1;
  }

  for (p=proc; p < &proc[NPROC]; p++){
    if (p -> pid == pid){
      wsum -= p->nice_value;
      p->nice_value = value;
      wsum += p->nice_value;

      p->vdeadline = p->vruntime + TIMESLICE * 1000 * 1024 / weight_arr[p->nice_value];

      return 0;
    }
  }
  printf("입력하신 pid가 존재하지 않습니다.\n");
  return -1;
}

uint64
meminfo(void)
{
  uint64 avail;
  // avail_memory()는 kalloc.c에 존재
  avail = avail_memory();
  return avail;
}

int
waitpid(int pid)
{
  struct proc *p = myproc();
  struct proc *c = proc;

  for (; c<&proc[NPROC]; c++){
    if (c->pid == pid && c->parent == p){
      while (c->state != ZOMBIE){
	acquire(&tickslock);
        sleep(&ticks, &tickslock);
	release(&tickslock);
      }
      return 0;
    }
  }
  return -1;
} 

uint64
mmap(uint64 addr, int length, int prot, int flags, int fd, int offset)
{
	if ((addr % PGSIZE) != 0) panic("mmap: addr not aligned");
	if ((length % PGSIZE) != 0) panic("mmap: page size not aligned");
	if (length == 0) panic("mmap: size zero");
	
  int is_populate = flags & MAP_POPULATE;
  int is_anonymous = flags & MAP_ANONYMOUS;
  int is_readable = prot & PROT_READ;
  int is_writable = prot & PROT_WRITE;

	struct proc *p = myproc();
	struct file *f = p->ofile[fd];

  if (f)
  { 
    if (!f->readable && is_readable) return 0;
    if (!f->writable && is_writable) return 0;
  }

	addr += MMAPBASE;

  int k = 0;
  while (mmap_area[k]->used != 0) k++;
  mmap_area[k]->f = f;
  mmap_area[k]->addr = addr;
  mmap_area[k]->length = length;
  mmap_area[k]->offset = offset;
  mmap_area[k]->prot = prot;
  mmap_area[k]->flags = flags;
  mmap_area[k]->p = p;
  mmap_area[k]->used = 1;
	
	if (is_populate && !is_anonymous) // with MAP_POPULATE, without MAP_ANONYMOUS
  {
		int i = length / PGSIZE;

		for (int j = 0; j < i; j++){
			char *physical_page = (char *)kalloc();
			if (!physical_page) panic("kalloc failed for arr[%d]",i);

			mappages(p->pagetable, addr, PGSIZE, (uint64)physical_page, PTE_W | PTE_R | PTE_U);
			
			ilock(f->ip);
			readi(f->ip, 1, addr, offset, PGSIZE); // user_dst == 0 or 1
			iunlock(f->ip);
			
			offset += PGSIZE;
			addr += PGSIZE;
		}
    return addr;
  }
  if (!is_populate && is_anonymous) // without MAP_POPULATE, with MAP_ANONYMOUS
  {
    return addr; //page fault
  }

  if(!is_populate && !is_anonymous) // without MAP_POPULATE, without MAP_ANONYMOUS
  {
    return addr; //page fault
  }

  else if (is_populate && is_anonymous) // with MAP_POPULATE, with MAP_ANONYMOUS
  {
    if (fd != -1) return 0;
    if (offset != 0) return 0;

		int i = length / PGSIZE;

		for (int j = 0; j < i; j++){
			char *physical_page = (char *)kalloc();
			if (!physical_page) panic("kalloc failed for arr[%d]",i);
      memset(physical_page, 0, PGSIZE);

			mappages(p->pagetable, addr, PGSIZE, (uint64)physical_page, PTE_W | PTE_R | PTE_U);
			
			offset += PGSIZE;
			addr += PGSIZE;
    }
  return addr;
  }
}

		
