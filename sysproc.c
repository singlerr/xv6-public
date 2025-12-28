#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"

// to temporary store values of struct proc
// be careful to match its ordering with struct procinfo of user.h
struct k_procinfo
{
  int pid, ppid, state;
  uint sz;
  char name[16];
};

int sys_fork(void)
{
  return fork();
}

int sys_exit(void)
{
  exit();
  return 0; // not reached
}

int sys_wait(void)
{
  return wait();
}

int sys_kill(void)
{
  int pid;

  if (argint(0, &pid) < 0)
    return -1;
  return kill(pid);
}

int sys_getpid(void)
{
  return myproc()->pid;
}

int sys_sbrk(void)
{
  int addr;
  int n;

  if (argint(0, &n) < 0)
    return -1;
  addr = myproc()->sz;
  if (growproc(n) < 0)
    return -1;
  return addr;
}

int sys_sleep(void)
{
  int n;
  uint ticks0;

  if (argint(0, &n) < 0)
    return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n)
  {
    if (myproc()->killed)
    {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

// return how many clock tick interrupts have occurred
// since start.
int sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

// hello_number syscall
int sys_hello_number(void)
{
  int n;
  // fetch int param from user memory
  if (argint(0, &n) < 0)
    return -1;

  // print to console
  cprintf("Hello, xv6! Your number is %d\n", n);
  return n * 2;
}

// get proc info
int sys_get_procinfo(void)
{
  // pid of process from user memory
  int pid;
  // address of struct procinfo from user memory
  // will have same size with struct k_procinfo
  char *uaddr;
  struct proc *p;
  struct k_procinfo kinfo;
  if (argint(0, &pid) < 0)
    return -1;
  if (argptr(1, &uaddr, sizeof(struct k_procinfo)) < 0)
    return -1;

  // to access ptable, I created new function in proc.c and defs.h
  // and here we can find process by pid
  p = getproc(pid);
  // there is no available process with that pid, then return -1
  if (p == 0)
    return -1;

  // copy null-terminated string
  safestrcpy(kinfo.name, p->name, sizeof(kinfo.name));
  kinfo.pid = p->pid;
  kinfo.ppid = p->parent->pid;
  kinfo.state = (int)p->state;
  kinfo.sz = p->sz;

  // copy sequence of values of struct k_procinfo from kernel memory to user memory
  // both have same ordering of members
  if (copyout(myproc()->pgdir, (uint)uaddr, (void *)&kinfo, sizeof(kinfo)) < 0)
    return -1;

  return 0;
}