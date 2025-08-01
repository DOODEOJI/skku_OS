#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

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

#define MMAPBASE 0x40000000
struct mmap_area mmap_a[64];

extern int free_mem_count;

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

  for (int j = 0; j < 64; j++){
    if ((mmap_a[j].used == 1) && (mmap_a[j].p == p)){
      for (int k = 0; k < 64; k++){
        if (mmap_a[k].used == 0){
          mmap_a[k].f = mmap_a[j].f;
          mmap_a[k].addr = mmap_a[j].addr;
          mmap_a[k].length = mmap_a[j].length;
          mmap_a[k].offset = mmap_a[j].offset;
          mmap_a[k].prot = mmap_a[j].prot;
          mmap_a[k].flags = mmap_a[j].flags;
          mmap_a[k].p = np;
          mmap_a[k].used = mmap_a[j].used;
          
          int PTE_flags = PTE_U; // PTE_U -> user mode
          if (mmap_a[j].prot & PROT_READ) PTE_flags |= PTE_R;
          if (mmap_a[j].prot & PROT_WRITE) PTE_flags |= PTE_W; 
          
          uint64 addr = mmap_a[j].addr;
          for (uint64 i = addr; i < addr + mmap_a[j].length; i += PGSIZE){
            pte_t *pte = walk(p->pagetable, i, 0);
            if (pte && (*pte & PTE_V)){
              char *page = kalloc();
              uint64 pa = PTE2PA(*pte);
              memset(page, 0, PGSIZE);
              memmove(page, (char *)pa, PGSIZE);
              if (mappages(np->pagetable, i, PGSIZE, (uint64)page, PTE_flags) != 0){
                kfree(page);
                freeproc(np);
                release(&np->lock);
                return -1;
              }
            }
          }
          break; 
        }     
      }
    }
  }

  pid = np->pid;

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

  // unmap the remaining pages when program exits
  for (uint64 va = MMAPBASE; va < MMAPBASE + 64 * PGSIZE; va += PGSIZE){
    pte_t *pte = walk(p->pagetable, va, 0);
    if (pte && (*pte & PTE_V)){
      munmap(va);
    }
  }

  for (int i = 0; i < 64; i++){
    if (mmap_a[i].used && (mmap_a[i].p == p)){
      mmap_a[i].f = 0;
      mmap_a[i].addr = 0;
      mmap_a[i].length = 0;
      mmap_a[i].offset = 0;
      mmap_a[i].prot = 0;
      mmap_a[i].flags = 0;
      mmap_a[i].p = 0;
      mmap_a[i].used = 0;
    }
  }

  // traverse the higher directory and free the pages if not used

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
  struct proc *p;
  struct cpu *c = mycpu();

  c->proc = 0;
  for(;;){
    // The most recent process to run may have had interrupts
    // turned off; enable them to avoid a deadlock if all
    // processes are waiting.
    intr_on();

    int found = 0;
    for(p = proc; p < &proc[NPROC]; p++) {
      acquire(&p->lock);
      if(p->state == RUNNABLE) {
        // Switch to chosen process.  It is the process's job
        // to release its lock and then reacquire it
        // before jumping back to us.
        p->state = RUNNING;
        c->proc = p;
        swtch(&c->context, &p->context);

        // Process is done running for now.
        // It should have changed its p->state before coming back.
        c->proc = 0;
        found = 1;
      }
      release(&p->lock);
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

uint64
mmap(uint64 addr, int length, int prot, int flags, int fd, int offset)
{
	if ((addr % PGSIZE) != 0) return 0;
	if ((length % PGSIZE) != 0) return 0;
	if (length == 0) return 0;
	
  int is_populate = 0;
  if (flags & MAP_POPULATE) is_populate = 1;

  int is_anonymous = 0;
  if (flags & MAP_ANONYMOUS) is_anonymous = 1;

  int is_readable = 0;
  if (prot & PROT_READ) is_readable = 1;

  int is_writable = 0;
  if (prot & PROT_WRITE) is_writable = 1;

  int PTE_flags = PTE_U; // PTE_U -> user mode
  if (prot & PROT_READ) PTE_flags |= PTE_R;
  if (prot & PROT_WRITE) PTE_flags |= PTE_W; 

	struct proc *p = myproc();
  struct file *f = 0;

  if (fd != -1) f = p->ofile[fd];

  if (f)
  { 
    if (!f->readable && is_readable) return 0;
    if (!f->writable && is_writable) return 0;
  }

	addr += MMAPBASE;
  uint64 addr_updated = addr;

  int k = 0;
  while (mmap_a[k].used != 0) k++;
  mmap_a[k].f = f;
  mmap_a[k].addr = addr;
  mmap_a[k].length = length;
  mmap_a[k].offset = offset;
  mmap_a[k].prot = prot;
  mmap_a[k].flags = flags;
  mmap_a[k].p = p;
  mmap_a[k].used = 1;

  if (!is_populate && is_anonymous) // without MAP_POPULATE, with MAP_ANONYMOUS
  {
    if (fd != -1) return 0;
    if (offset != 0) return 0;
    return addr; //page fault
  }

  if(!is_populate && !is_anonymous) // without MAP_POPULATE, without MAP_ANONYMOUS
  {
    if (fd == -1) return 0;
    return addr; //page fault
  }
	
	if (is_populate && !is_anonymous) // with MAP_POPULATE, without MAP_ANONYMOUS
  {
    if (fd == -1) return 0;
		int i = length / PGSIZE;

		for (int j = 0; j < i; j++){
			char *physical_page = (char *)kalloc();
			if (!physical_page){
        printf("kalloc failed for arr[%d]",i);
        panic("kalloc failed");
      }

			ilock(f->ip);
			readi(f->ip, 0, (uint64) physical_page, offset, PGSIZE); // user_dst == 0 or 1
			iunlock(f->ip);

      if (mappages(p->pagetable, addr_updated, PGSIZE, (uint64)physical_page, PTE_flags) < 0){
        printf("mmap: mappages failed\n");
        kfree(physical_page);
        return 0;
      }
			
			offset += PGSIZE;
			addr_updated += PGSIZE;
		}
  }

  if (is_populate && is_anonymous) // with MAP_POPULATE, with MAP_ANONYMOUS
  {
    if (fd != -1) return 0;
    if (offset != 0) return 0;

		int i = length / PGSIZE;

		for (int j = 0; j < i; j++){
			char *physical_page = (char *)kalloc();
			if (!physical_page){
        printf("kalloc failed for arr[%d]",i);
        panic("kalloc failed");
      }
      
      memset(physical_page, 0, PGSIZE);

      if (mappages(p->pagetable, addr_updated, PGSIZE, (uint64)physical_page, PTE_flags) < 0){
        printf("mmap: mappages failed\n");
        kfree(physical_page);
        return 0;
      }
			
			offset += PGSIZE;
			addr_updated += PGSIZE;
    }

  }
  return addr;
}

int
munmap(uint64 addr)
{
  if (addr % PGSIZE != 0) return -1;
  
  struct proc *p = myproc();
  
  int i = 0;
  while (i < 64){
    if (mmap_a[i].used){
      if (mmap_a[i].p == p && mmap_a[i].addr == addr){
        for (int j = 0; j < mmap_a[i].length / PGSIZE; j++){
          pte_t *pte = walk(p->pagetable, addr + j * PGSIZE, 0);
          if (!pte) continue;
          if (!(*pte & PTE_V)) continue; // not populate case
          uvmunmap(p->pagetable, addr + j * PGSIZE, 1, 1); // do_free: 1 -> kfree(physical_mem)
        }
        mmap_a[i].f = 0;
        mmap_a[i].addr = 0;
        mmap_a[i].length = 0;
        mmap_a[i].offset = 0;
        mmap_a[i].prot = 0;
        mmap_a[i].flags = 0;
        mmap_a[i].p = 0;
        mmap_a[i].used = 0;
        return 1;
      }
    }
    i++;
  }
  return -1;
}

int
freemem(void)
{
  return free_mem_count;
}

int
pf_handler(uint64 addr, uint64 err)
{
  struct proc *p = myproc();

  int i = 0;
  while (i < 64){
    if (mmap_a[i].used){  
      if (mmap_a[i].p == p && mmap_a[i].addr == addr){
        
        struct file *f = mmap_a[i].f;
        int prot = mmap_a[i].prot;
        int flags = mmap_a[i].flags;
        uint64 addr_updated = mmap_a[i].addr;
        int offset = mmap_a[i].offset;
        int length = mmap_a[i].length;
        
        int is_readable = 0;
        if (prot & PROT_READ) is_readable = 1;

        int is_writable = 0;
        if (prot & PROT_WRITE) is_writable = 1;

        int is_anonymous = 0;
        if (flags & MAP_ANONYMOUS) is_anonymous = 1;
        
        int PTE_flags = PTE_U;
        if (prot & PROT_READ) PTE_flags |= PTE_R;
        if (prot & PROT_WRITE) PTE_flags |= PTE_W;
        
        if (!is_writable && (err == 15)){
          printf("Page fault: write access to a read-only page\n");
          return -1;
        }

        int i = length / PGSIZE;

        if (f)
        { 
          if (!f->readable && is_readable) return -1;
          if (!f->writable && is_writable) return -1;
        }
        
        if (is_anonymous){
          for (int j = 0; j < i; j++){
            char *physical_page = (char *)kalloc();
            if (!physical_page){
              printf("kalloc failed for arr[%d]",i);
              panic("kalloc failed");
            }
            
            memset(physical_page, 0, PGSIZE);
      
            if (mappages(p->pagetable, addr_updated, PGSIZE, (uint64)physical_page, PTE_flags) < 0){
              printf("mmap: mappages failed\n");
              kfree(physical_page);
              return 0;
            }
            
            offset += PGSIZE;
            addr_updated += PGSIZE;
          }
          return 1;
        }

        if (!is_anonymous){
          for (int j = 0; j < i; j++){
            char *physical_page = (char *)kalloc();
            if (!physical_page){
              printf("kalloc failed for arr[%d]",i);
              panic("kalloc failed");
            }

            ilock(f->ip);
            readi(f->ip, 0, (uint64) physical_page, offset, PGSIZE); // user_dst == 0 or 1
            iunlock(f->ip);
      
            if (mappages(p->pagetable, addr_updated, PGSIZE, (uint64)physical_page, PTE_flags) < 0){
              printf("mmap: mappages failed\n");
              kfree(physical_page);
              return 0;
            }
            
            offset += PGSIZE;
            addr_updated += PGSIZE;
          }
          return 1;
        }
      }

    }
    i++;
  }
  return -1;
}