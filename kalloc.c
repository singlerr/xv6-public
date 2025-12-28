// Physical memory allocator, intended to allocate
// memory for user processes, kernel stacks, page table pages,
// and pipe buffers. Allocates 4096-byte pages.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "spinlock.h"
#include "proc.h"
#include "swtlb.h"

void freerange(void *vstart, void *vend);
extern char end[]; // first address after kernel loaded from ELF file
                   // defined by the kernel linker script in kernel.ld
struct kphysframe_info pf_info[PFNNUM];
struct spinlock pflock;
struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock;
  int use_lock;
  struct run *freelist;
} kmem;

// Initialization happens in two phases.
// 1. main() calls kinit1() while still using entrypgdir to place just
// the pages mapped by entrypgdir on free list.
// 2. main() calls kinit2() with the rest of the physical pages
// after installing a full page table that maps them on all cores.
void kinit1(void *vstart, void *vend)
{
  initlock(&kmem.lock, "kmem");
  initlock(&pflock, "pflock");
  // memset(pf_info, 0, sizeof(pf_info));
  kmem.use_lock = 0;
  freerange(vstart, vend);
}

void kinit2(void *vstart, void *vend)
{
  freerange(vstart, vend);
  kmem.use_lock = 1;
}

void freerange(void *vstart, void *vend)
{
  char *p;
  uint idx;
  struct kphysframe_info *info;
  p = (char *)PGROUNDUP((uint)vstart);

  if (kmem.use_lock)
    acquire(&pflock);
  for (; p + PGSIZE <= (char *)vend; p += PGSIZE)
  {
    idx = PFX(p);
    if (idx >= PFNNUM)
    {
      panic("kfree: out of range");
    }

    // initialize physframe_info entries
    info = &pf_info[idx];
    info->frame_index = 0;
    info->allocated = 0;
    info->pid = -1;
    info->start_tick = 0;
    info->refcnt = 0;
    kfree(p);
  }

  if (kmem.use_lock)
    release(&pflock);
}
// PAGEBREAK: 21
//  Free the page of physical memory pointed at by v,
//  which normally should have been returned by a
//  call to kalloc().  (The exception is when
//  initializing the allocator; see kinit above.)
// to implement COW, we have to track reference count of physical page since two or more processes can point the same page
// only page is actaully freed when reference count becomes 0, before that, kfree() decreases reference count
void kfree(char *v)
{
  struct run *r;
  struct kphysframe_info *info;
  uint idx;
  if ((uint)v % PGSIZE || v < end || V2P(v) >= PHYSTOP)
    panic("kfree");

  idx = PFX((char *)v);

  if (idx >= PFNNUM)
  {
    panic("kfree: out of range");
  }

  if (kmem.use_lock)
    acquire(&pflock);

  info = &pf_info[idx];

  // decrease reference count
  if (info->refcnt > 0)
  {
    info->refcnt--;
  }

  // there is no more references to this, then free
  if (info->refcnt == 0)
  {
    // Fill with junk to catch dangling refs.
    memset(v, 1, PGSIZE);

    if (kmem.use_lock)
      acquire(&kmem.lock);
    r = (struct run *)v;
    r->next = kmem.freelist;
    kmem.freelist = r;

    if (kmem.use_lock)
      release(&kmem.lock);
    info->frame_index = 0;
    info->allocated = 0;
    info->pid = -1;
    info->start_tick = 0;
    info->refcnt = 0;
  }

  if (kmem.use_lock)
    release(&pflock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// if store_pid is 1, then it retrieves physframe_info entry for index computed by va / PGSIZE, thus set pid to that struct
char *kalloc(int store_pid)
{
  struct run *r;
  uint idx;
  uint curticks;
  struct kphysframe_info *info;

  if (kmem.use_lock)
  {
    acquire(&tickslock);
    curticks = ticks;
    release(&tickslock);
    acquire(&kmem.lock);
  }
  else
  {
    curticks = ticks;
  }

  r = kmem.freelist;
  if (r)
  {
    kmem.freelist = r->next;
    idx = PFX((char *)r);

    if (idx >= PFNNUM)
    {
      panic("kalloc: out of range");
    }
    if (kmem.use_lock)
      acquire(&pflock);

    // store pf info
    info = &pf_info[idx];
    info->allocated = 1;
    info->frame_index = idx;
    info->start_tick = curticks;
    info->refcnt = 1;
    if (procready() && store_pid)
    {
      struct proc *p = myproc();
      if (p)
        info->pid = p->pid;
    }

    if (kmem.use_lock)
      release(&pflock);
  }

  if (kmem.use_lock)
    release(&kmem.lock);

  return (char *)r;
}