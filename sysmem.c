#include "types.h"
#include "x86.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "swtlb.h"

// dump kphysframe_info to physframe_info struct in user space
// it returns nubmer of returned physframe_info elements
int sys_dump_physmem_info()
{
    char *ptr;
    struct proc *p;
    struct kphysframe_info kinfo;
    struct kphysframe_info info;
    int max_entries;
    int count = 0;
    if (argint(1, &max_entries) < 0)
        return -1;

    if (max_entries <= 0)
    {
        return -1;
    }

    p = myproc();

    if (!p)
    {
        return -1;
    }

    if (argptr(0, &ptr, sizeof(struct kphysframe_info) * max_entries) < 0)
        return -1;

    // lock
    acquire(&pflock);
    for (int i = 0; i < PFNNUM; i++)
    {
        if (count >= max_entries)
            break;

        kinfo = pf_info[i];
        info.allocated = kinfo.allocated;
        info.frame_index = kinfo.frame_index;
        info.pid = kinfo.pid;
        info.start_tick = kinfo.start_tick;

        // copy from kernel space to user space, given start address of array ptr plus count multiplied by size of kphysframe_info(which has same size of physframe_info)
        if (copyout(p->pgdir, (uint)ptr + (uint)sizeof(struct kphysframe_info) * (uint)count, (void *)&info, sizeof(info)) < 0)
        {
            return -1;
        }

        count++;
    }

    // unlock
    release(&pflock);

    return count;
}

int sys_vtop(void)
{
    char *va;
    char *pa_out;
    char *flags_out;
    int ret;
    uint32_t pa;
    uint32_t flags;
    if (argptr(0, &va, sizeof(void *)) < 0)
        return -1;
    if (argptr(1, &pa_out, sizeof(uint32_t)) < 0)
        return -1;
    if (argptr(2, &flags_out, sizeof(uint32_t)) < 0)
        return -1;

    struct proc *p = myproc();

    if (p == 0)
        return -1;

    if ((ret = sw_vtop(p->pgdir, (const void *)va, &pa, &flags)) < 0)
    {
        return ret;
    }

    // PTE_T flag must be deleted when it comes to user program
    // because it is only used for TLB caching
    if (flags & PTE_T)
    {
        flags &= ~PTE_T;
        flags |= PTE_P;
    }

    if (copyout(p->pgdir, (uint)pa_out, (void *)&pa, sizeof(uint32_t)) < 0)
        return -1;
    if (copyout(p->pgdir, (uint)flags_out, (void *)&flags, sizeof(uint32_t)) < 0)
        return -1;

    return ret;
}

int sys_phys2virt(void)
{
    uint32_t pa;
    char *addr;
    int max;
    int i;
    struct ipt_entry *e;
    struct kvlist t;
    if (arguint(0, &pa) < 0)
        return -1;

    if (argint(2, &max) < 0)
        return -1;

    if (argptr(1, &addr, sizeof(struct kvlist) * max) < 0)
        return -1;

    struct proc *p = myproc();

    if (p == 0)
        return -1;

    acquire(ipt_lock);

    // from the head of ipt entry, loops all available(non-null) ipt entries and copyout to user space
    for (i = 0, e = ipt_head(pa); e && i < max; e = e->next, i++)
    {
        t.flags = e->flags;
        t.pid = e->pid;
        t.va = e->va;

        if (t.flags & PTE_T)
        {
            t.flags &= ~PTE_T;
            t.flags |= PTE_P;
        }
        t.flags &= 0x1F;

        if (copyout(p->pgdir, (uint)addr + (uint)sizeof(struct kvlist) * (uint)i, &t, sizeof(struct kvlist)) < 0)
            return -1;
    }

    release(ipt_lock);
    return i;
}

// used for vtop - showing miss and hit count of TLB
int sys_tlbinfo(void)
{
    char *hits_out;
    char *misses_out;
    struct proc *p;
    uint32_t hits, misses;
    if (argptr(0, &hits_out, sizeof(uint32_t)) < 0)
        return -1;

    if (argptr(1, &misses_out, sizeof(uint32_t) < 0))
        return -1;
    if ((p = myproc()) == 0)
        return -1;
    gettlbinfo(&hits, &misses);

    if (copyout(p->pgdir, (uint)hits_out, &hits, sizeof(uint32_t)) < 0)
        return -1;
    if (copyout(p->pgdir, (uint)misses_out, &misses, sizeof(uint32_t)) < 0)
        return -1;

    return 0;
}