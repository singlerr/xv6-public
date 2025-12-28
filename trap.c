#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "swtlb.h"
// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[]; // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void tvinit(void)
{
  int i;

  for (i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE << 3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE << 3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void idtinit(void)
{
  lidt(idt, sizeof(idt));
}

// PAGEBREAK: 41
void trap(struct trapframe *tf)
{
  if (tf->trapno == T_SYSCALL)
  {
    if (myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if (myproc()->killed)
      exit();
    return;
  }

  switch (tf->trapno)
  {
  case T_IRQ0 + IRQ_TIMER:
    if (cpuid() == 0)
    {
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE + 1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;
  case T_PGFLT:
    // at this page fault handler, it will handle two situations, first Copy-On-Write and SW TLB
    // all these will be handled if and only if myproc() returns non-null object
    // it checks first the fault was caused by absense of PTE_W flag which is part of COW
    // and second checks the fault was caused by absense of PTE_P flag which is part of SW TLB
    uint va = rcr2();
    uint va_pg = PGROUNDDOWN(va);
    struct proc *p = myproc();
    uint32_t pa, flags, rpa;
    int refcnt;
    pte_t *pte;
    if (p == 0)
    {
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("page fault trap");
    }

    // get PTE first and validate before dereference
    pte = vamap(p->pgdir, va_pg);
    if (!pte)
    {
      cprintf("page fault - pid %d %s: trap %d err %d on cpu %d "
              "eip 0x%x addr 0x%x flags <no pte>--kill proc\n",
              myproc()->pid, myproc()->name, tf->trapno,
              tf->err, cpuid(), tf->eip, va);
      myproc()->killed = 1;
      break;
    }

    // if PTE lacks PTE_W, then check if the process just called fork()
    // implement COW
    // PTE_C flag means the process forked child process and one of them tries to access that page
    if (tf->err & 0x2 && (*pte & PTE_C))
    {
      uint32_t pa = PTE_ADDR(*pte);
      uint32_t idx = pa / PGSIZE;
      char *mem;
      if (idx >= PFNNUM)
      {
        panic("out of range");
      }
      acquire(&pflock);
      struct kphysframe_info *info = &pf_info[idx];
      refcnt = info->refcnt;
      release(&pflock);
      // if child process tries to write the page, allocate new page = copy on write
      if (refcnt > 1)
      {
        if ((mem = kalloc(1)) == 0)
        {
          cprintf("COW: Out of memory\n -- kill proc %s with pid %d\n", p->name, p->pid);
          p->killed = 1;
        }
        else
        {

          // create new mapping
          memmove(mem, (char *)P2V(pa), PGSIZE);
          *pte = V2P(mem) | PTE_FLAGS(*pte);
          *pte &= ~PTE_C;
          *pte |= PTE_W;
          // remove ipt entry because now it points another physical page
          ipt_remove(va, pa, p->pid);
          ipt_insert(va, V2P(mem), PTE_FLAGS(*pte), p->pid);
          acquire(&pflock);
          info->refcnt = refcnt - 1;
          release(&pflock);
        }
      }

      // remove COW flag and recover PTE_W flag
      *pte &= ~PTE_C;
      *pte |= PTE_W;

      lcr3(V2P(p->pgdir)); // flush HW TLB to make changes
    }

    // If PTE exists but lacks both M and P, rescue by marking as managed (M)
    if (((*pte & (PTE_T | PTE_P)) == 0))
    {
      uint32_t f = PTE_FLAGS(*pte);
      // ensure it's a user mapping
      if (va_pg < KERNBASE)
      {
        f |= PTE_T;
        f |= PTE_U; // keep user accessible
        modifyflags(p->pgdir, va_pg, PTE_ADDR(*pte), f);
        lcr3(V2P(p->pgdir));
      }
      else
      {
        cprintf("page fault - pid %d %s: trap %d err %d on cpu %d "
                "eip 0x%x addr 0x%x flags %d--kill proc\n",
                myproc()->pid, myproc()->name, tf->trapno,
                tf->err, cpuid(), tf->eip, va, PTE_FLAGS(*pte));
        myproc()->killed = 1;
        break;
      }
    }

    pa = PTE_ADDR(*pte);
    flags = PTE_FLAGS(*pte);

    // compute SW TLB
    // this simulates HW TLB in the software manner
    if (!(*pte & PTE_P) && (*pte & PTE_T))
    {
      // hit!
      if (tlblookup(p->pid, va_pg, &rpa, 0))
      {
        // if pa does not match with real pa, then update
        if (pa != rpa)
        {
          tlballoc(p->pid, va_pg, pa, flags);
        }
      }
      else
      {
        // not hit
        tlballoc(p->pid, va_pg, pa, flags);
      }
      // track va
      track_va(p, va_pg);
      // temporarily map
      flags &= ~PTE_T;
      flags |= PTE_P;
      modifyflags(p->pgdir, va_pg, pa, flags);
      lcr3(V2P(p->pgdir)); // flush tlb to make changes
      lapiceoi();
    }
    break;
  // PAGEBREAK: 13
  default:
    if (myproc() == 0 || (tf->cs & 3) == 0)
    {
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if (myproc() && myproc()->state == RUNNING &&
      tf->trapno == T_IRQ0 + IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if (myproc() && myproc()->killed && (tf->cs & 3) == DPL_USER)
    exit();
}
