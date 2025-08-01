#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  exit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return fork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return wait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int n;

  argint(0, &n);
  addr = myproc()->sz;
  if(growproc(n) < 0)
    return -1;
  return addr;
}

uint64
sys_sleep(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_ps(void)
{ 
  int pid;
  argint(0, &pid); 
  ps(pid);
  return 0;
}

uint64
sys_getnice(void)
{
  int pid;
  argint(0, &pid);
  return getnice(pid);
}

uint64
sys_setnice(void)
{
  int pid, value;
  argint(0, &pid);
  argint(1, &value);
  return setnice(pid, value);
}

uint64
sys_meminfo(void)
{  
  return meminfo();
}

uint64
sys_waitpid(void){

  int pid;
  argint(0, &pid);
  return waitpid(pid);
}

uint64
sys_mmap(void){
	uint64 addr;
	int length;
	int prot;
	int flags;
	int fd;
	int offset;

	argint(0, &addr);
	argint(1, &length);
	argint(2, &prot);
	argint(3, &flags);
	argint(4, &fd);
        argint(5, &offset);

	return mmap(addr, length, prot, flags, fd, offset);
}
