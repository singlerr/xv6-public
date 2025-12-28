struct ipt_entry
{
    uint32_t pfn;
    uint32_t pid;
    uint32_t va;
    uint16_t flags;
    uint16_t refcnt;

    struct ipt_entry *next;
    struct ipt_entry *cnext;
};

struct tlb_entry
{
    uint32_t pid;
    uint32_t va;
    uint32_t pa;
    uint16_t flags;
    int valid;
};

extern struct spinlock pflock;
void tlbinit1();
void tlbinit2();
void iptinit1();
void iptinit2();

struct ipt_entry *ipt_head(uint32_t);

int ipt_insert(uint32_t, uint32_t, uint32_t, int);
int ipt_remove(uint32_t, uint32_t, int);

void drop_trackers(struct proc *);
void track_va(struct proc *, uint32_t);
void drop_trackers_except(struct proc *, uint32_t);

int tlballoc(uint32_t, uint32_t, uint32_t, uint32_t);
int tlblookup(uint32_t, uint32_t, uint32_t *, uint32_t *);
void tlbivlt(uint32_t);
void tlbivltp(uint32_t, uint32_t);
void tlbflsh();
void tlbinit1(void);
void iptinit2(void);
void iptinit2(void);
struct ipt_entry *iptalloc();
void iptrelse(struct ipt_entry *);
int gettlbinfo(uint32_t *, uint32_t *);