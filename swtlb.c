#include "types.h"
#include "defs.h"
#include "spinlock.h"
#include "param.h"
#include "mmu.h"
#include "proc.h"
#include "swtlb.h"

/***
 * implementations of ipt table(Inverted Page Table) and tlb(Translation Lookaside Buffer)
 */

#define IPT_BUCKETS 60000
#define NUMTLB 128
#define TX(pid, page) ((pid) ^ (page))

struct ipt_entry *ipt_hash[IPT_BUCKETS] = {0};

struct ipttable
{
    int use_lock;
    struct spinlock lock;
    struct spinlock tablelock;
    struct ipt_entry *head;
    struct ipt_entry *list;
} iptcache;

struct tlb
{
    int use_lock;
    struct tlb_entry entries[NUMTLB];
    struct spinlock lock;
    uint32_t hits;
    uint32_t misses;
} tlb;

struct spinlock *ipt_lock;

// vatracker will keep track of virutal address which grabbed on PGFLT of trap.c
// ideally, since all page tables marked not present, every access to va will cause page fault
// and page fault will be handled in trap.c, which causes swtlb to lookup and caches, mapping temporarily va - pa to
// original page table.
// also if we keep page table's entry forever, we will not be able to get hits/misses rate successfully
// so we need to keep track of va and unmap it immediately if another page fault occurs
// --
// drop all trackers of that process, resulting in all virtual address in that tracker will cause page fault again, making TLB can simulate HW TLB
void drop_trackers(struct proc *p)
{
    struct vatracker *t = p->tracked;
    int perm;
    pte_t *pte;
    for (int i = 0; i < MAX_TRACKERS; i++)
    {
        if (t[i].valid)
        {
            t[i].valid = 0;
            if ((pte = vamap(p->pgdir, t[i].va)) == 0)
                continue;

            perm = PTE_FLAGS(*pte);
            // remove present bit - this will cause page fault again
            perm &= ~PTE_P;
            perm |= PTE_T;
            // modify PTE
            modifyflags(p->pgdir, t[i].va, PTE_ADDR(*pte), perm);
        }
    }

    p->tracked_idx = 0;
}

// called from page fault handler of trap
// track virtual address which marked PTE_P
// we need this because we have to remove PTE_P bit after giving PTE_P to making that bit temporarily exists
// this allows our system to cause page fault again with that same virtual address, making SW TLB simulates HW TLB
// if this was not implemented, in the near future, all PTE's will be marked PTE_P and any more page fault will be raised, thus our SW TLB will be useless
void track_va(struct proc *p, uint32_t va)
{
    struct vatracker *t;
    // normalize to page boundary and avoid duplicates
    va = PGROUNDDOWN(va);
    for (int i = 0; i < p->tracked_idx; i++)
    {
        if (p->tracked[i].valid && p->tracked[i].va == va)
            return;
    }

    // if we have full of VA trackers, then drop all
    if (p->tracked_idx >= MAX_TRACKERS)
    {
        drop_trackers(p);
        p->tracked_idx = 0;
    }

    t = &p->tracked[p->tracked_idx++];
    t->va = va;
    t->valid = 1;
}

void drop_trackers_except(struct proc *p, uint32_t va)
{
    struct vatracker *t;
    pte_t *pte;
    int perm;
    va = PGROUNDDOWN(va);
    for (t = p->tracked; t < p->tracked + p->tracked_idx; t++)
    {
        if (t->valid && t->va == va)
            continue;
        t->valid = 0;
        if ((pte = vamap(p->pgdir, t->va)) == 0)
            continue;

        perm = PTE_FLAGS(*pte);
        perm &= ~PTE_P;
        perm |= PTE_T;

        modifyflags(p->pgdir, t->va, PTE_ADDR(*pte), perm);
    }
}

int gettlbinfo(uint32_t *hits_out, uint32_t *misses_out)
{
    *hits_out = tlb.hits;
    *misses_out = tlb.misses;
    return 0;
}

// since we cannot figure out how many ipt entries will be allocated, allocation of ipt entry must be dynamic
// so everytime kernel requested ipt entry, it checks if there is any ipt entry left in the pool and pop one from the list(pool).
// if no more ipt entry exists, then create new page for offering more entries to the pool
struct ipt_entry *iptalloc()
{
    struct ipt_entry *p;
    char *ap;
    if (iptcache.use_lock)
        acquire(&iptcache.lock);

    p = iptcache.list;

    if (p)
    {
        iptcache.list = p->cnext;
    }
    if (iptcache.use_lock)
        release(&iptcache.lock);

    if (p)
    {
        return p;
    }
    if (iptcache.use_lock)
        acquire(&iptcache.lock);

    char *r = kalloc(0); // prevent kalloc() storing pid info

    if (!r)
    {
        if (iptcache.use_lock)
            release(&iptcache.lock);
        return 0;
    }

    iptcache.head = (struct ipt_entry *)r;

    // create new empty ipt entry and offer it to the pool(list)
    for (ap = r; ap + sizeof(struct ipt_entry) <= r + PGSIZE; ap += sizeof(struct ipt_entry))
    {
        p = (struct ipt_entry *)ap;
        p->cnext = iptcache.list;
        iptcache.list = p;
    }

    p = iptcache.list;
    iptcache.list = p->cnext;
    if (iptcache.use_lock)
        release(&iptcache.lock);

    return p;
}

// release ipt entry to the pool
// if this is the last entry left of all entries, then remove page
void iptrelse(struct ipt_entry *i)
{
    memset((void *)i, 0, sizeof(struct ipt_entry));
    acquire(&iptcache.lock);

    i->cnext = iptcache.list;
    iptcache.list = i;

    if (iptcache.head == i)
    {
        kfree((char *)i);
    }
    release(&iptcache.lock);
}

// look up the TLB given pid and va
// it computes index for tlb entry by hashing pid ^ (va >> 12) and & (NUMTLB - 1) operation to prevent index exceeds range
// then it look up for entry given index and check if that is valid
// if cache hits, then it returns 1 and physcial address and its PTE flags are saved to given pointer
// or else returns 0
// if cache hits, it increments hit count, or not, it increments miss count to compute hit/miss rate
int tlblookup(uint32_t pid, uint32_t va, uint32_t *pa_out, uint32_t *flags_out)
{
    uint32_t vp = va >> 12;
    uint32_t idx = TX(pid, vp) & (NUMTLB - 1);
    struct tlb_entry *e;
    if (tlb.use_lock)
        acquire(&tlb.lock);

    e = &tlb.entries[idx];
    if (e->valid && e->pid == pid && e->va == vp)
    {
        if (pa_out)
            *pa_out = (e->pa << 12) | (va & 0xFFF);
        if (flags_out)
            *flags_out = e->flags;

        tlb.hits++;
        if (tlb.use_lock)
            release(&tlb.lock);
        return 1;
    }

    tlb.misses++;
    if (tlb.use_lock)
        release(&tlb.lock);
    return 0;
}

// if tlblookup does not hit that va and pid, we need to cache it
int tlballoc(uint32_t pid, uint32_t va, uint32_t pa, uint32_t flags)
{
    uint32_t vp = va >> 12;
    uint32_t pp = pa >> 12;
    uint32_t idx = TX(pid, vp) & (NUMTLB - 1);
    struct tlb_entry *e;

    if (tlb.use_lock)
        acquire(&tlb.lock);
    e = &tlb.entries[idx];

    e->valid = 1;
    e->pid = pid;
    e->va = vp;
    e->pa = pp;
    e->flags = flags;

    if (tlb.use_lock)
        release(&tlb.lock);
    return 0;
}

// invaliate all entries with that pid
// this happens when the process exits
void tlbivlt(uint32_t pid)
{
    struct tlb_entry *e;

    if (tlb.use_lock)
        acquire(&tlb.lock);

    for (e = tlb.entries; e < tlb.entries + NUMTLB; e++)
    {
        if (e->valid && e->pid == pid)
        {
            e->valid = 0;
        }
    }

    if (tlb.use_lock)
        release(&tlb.lock);
}

// invalidate specific tlb entry with pid and va
// this happens when remapping of page table to change mapping data
void tlbivltp(uint32_t pid, uint32_t va)
{
    uint32_t vp = va >> 12;
    uint32_t idx = TX(pid, vp) & (NUMTLB - 1);
    struct tlb_entry *e;

    if (tlb.use_lock)
        acquire(&tlb.lock);
    e = &tlb.entries[idx];

    if (e->valid && e->pid == pid && e->va == vp)
    {
        e->valid = 0;
    }
    if (tlb.use_lock)
        release(&tlb.lock);
}

void tlbflsh()
{
    struct tlb_entry *e;

    if (tlb.use_lock)
        acquire(&tlb.lock);
    for (e = tlb.entries; e < tlb.entries + NUMTLB; e++)
    {
        if (e->valid)
        {
            e->valid = 0;
        }
    }
    if (tlb.use_lock)
        release(&tlb.lock);
}

// init tlb
// create lock and initialize all entries to zero
void tlbinit1()
{
    initlock(&tlb.lock, "tlb");
    tlb.use_lock = 0;

    struct tlb_entry *e;
    for (e = tlb.entries; e < tlb.entries + NUMTLB; e++)
    {
        e->valid = 0;
        e->pa = 0;
        e->va = 0;
        e->flags = 0;
        e->pa = 0;
    }

    tlb.hits = 0;
    tlb.misses = 0;
}

// making lock available
// this works same with kmem.use_lock in kalloc.c
void tlbinit2()
{
    tlb.use_lock = 1;
}

// init ipt table
void iptinit1()
{
    initlock(&iptcache.lock, "iptcache");
    initlock(&iptcache.tablelock, "tablelock");
    ipt_lock = &iptcache.tablelock;
    iptcache.use_lock = 0;
}

// making lock available
void iptinit2()
{
    iptcache.use_lock = 1;
}

// insert new ipt table given va, pa, flags and pid
// it computes index by dividing physical address by page size
// if a entry with that index already exists, it overwrites.
// if none of entries was allocated before, it is assigned to the head
// else, it put into the tail of entry
// finally it invalidates tlb, becuase ipt_insert called when mappings changed
int ipt_insert(uint32_t va, uint32_t pa, uint32_t perm, int pid)
{
    uint idx = pa / PGSIZE;
    struct ipt_entry *t = 0, *last = 0;
    if (idx >= IPT_BUCKETS)
    {
        panic("ipt: out of range");
    }

    if (iptcache.use_lock)
        acquire(&iptcache.tablelock);

    t = ipt_hash[idx];

    while (t)
    {
        if (t->va == va && t->pid == pid)
        {
            break;
        }
        last = t;
        t = t->next;
    }

    if (t)
    {
        // just update
        t->flags = perm | PTE_P;
    }
    else
    {
        // allocate new
        t = iptalloc();
        if (t == 0)
        {
            if (iptcache.use_lock)
                release(&iptcache.tablelock);
            return -1;
        }
        t->flags = perm | PTE_P;
        t->va = va;
        t->pfn = idx;
        t->pid = pid;
        t->refcnt = 0;
        t->next = 0;

        if (last)
        {
            // put it to tail
            last->next = t;
            if (ipt_hash[idx])
                ipt_hash[idx]->refcnt++;
        }
        else
        {
            ipt_hash[idx] = t;
        }
    }

    if (iptcache.use_lock)
        release(&iptcache.tablelock);
    tlbivltp(pid, va);
    return 0;
}

// remove ipt entry from ipt table, decrementing refcnt of ipt entry
// and release that entry, if that entry was in the middle of chain, chain will be reconnected
int ipt_remove(uint32_t va, uint32_t pa, int pid)
{
    uint idx = pa / PGSIZE;
    struct ipt_entry *ent = 0, *t = 0, *prev = 0;
    if (iptcache.use_lock)
        acquire(&iptcache.tablelock);
    ent = ipt_hash[idx];
    t = ent;

    while (t)
    {
        if (t->va == va && t->pid == pid)
        {
            break;
        }
        prev = t;
        t = t->next;
    }

    if (t)
    {
        if (prev)
            prev->next = t->next;
        else
            ipt_hash[idx] = t->next;

        t->next = 0;
        t->va = 0;
        t->pid = 0;
        t->pfn = 0;
        t->flags = 0;
        ent->refcnt--;
        if (t == ent)
        {
            ipt_hash[idx] = 0;
        }
        iptrelse(t);
        if (iptcache.use_lock)
            release(&iptcache.tablelock);
        return 1;
    }
    if (iptcache.use_lock)
        release(&iptcache.tablelock);
    return 0;
}

// get the head entry of chain
struct ipt_entry *ipt_head(uint32_t pa)
{
    uint idx = pa / PGSIZE;
    return ipt_hash[idx];
}