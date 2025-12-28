# xv6 Physical Page Frame Tracking

## Project Overview

The xv6 operating system's physical memory manager maintains available page frames using a free list, but doesn't provide a global tracking table showing which frames are used by which processes. This project extends xv6's memory allocator (kalloc/kfree) to implement real-time tracking of all physical memory frame usage, enabling identification of which process owns which frame and when it started using it.

This functionality is valuable for memory leak debugging, memory usage pattern analysis, and visualizing OS memory management operations.

## Implementation Goals

### Required Features (Part A & B)

- Introduce a global frame information table in the kernel to continuously track allocation status, owner PID, and start tick for all physical page frames
- Modify kalloc/kfree to immediately update corresponding entries during frame allocation/deallocation, integrating tracking functionality into the existing free list-based allocator
- Implement `dump_physmem_info()` system call to allow user processes to query and display the global frame state table

### Advanced Features (Part C)

- Implement software-only virtual-to-physical address translation without hardware page table (PGDIR/PTE) dependencies
- Introduce Inverted Page Table (IPT) for reverse mapping from physical page to (process, virtual address)
- Implement software-based TLB cache
- Ensure consistency between page tables, IPT, and software TLB

## Part A: Required Implementation

### 1. Global Frame Information Table

Create and maintain a global table in the kernel storing information for each physical memory frame. Modify xv6's memory allocation function `kalloc()` and deallocation function `kfree()` to update this table whenever frames are allocated or freed.

**Important:** The global frame information table must have exactly **60,000** elements.

The table contains entries for all physical page frames in the system, adding frame usage tracking to xv6's free list-based memory allocator.

### 2. Per-Frame Information Storage

Each table entry stores the following information:

**(1) Allocation Status** - Whether the frame is currently allocated and in use (boolean or flag value)

**(2) Owner Process PID** - PID of the process currently occupying the frame (use reserved value if none)

**(3) Start Time** - Tick count when the frame was allocated to the current owner process (total ticks elapsed since system boot)

**Note:** The tick value is a global time unit that increments with each clock interrupt in the xv6 kernel, managed by the `ticks` variable. This enables tracking how long a frame has been in use.

### 3. System Call: dump_physmem_info

Implement a new system call to output global frame information from kernel space to user space:

```c
int dump_physmem_info(void *addr, int max_entries);
```

This system call reads up to `max_entries` frame usage information from the kernel's global table and copies it to a user-provided buffer (memory pointed to by `addr`) as an array of structures.

Rather than printing directly via `printf` in the kernel, the system call copies structured information for use by user programs. Use xv6 kernel's `copyout()` for safe kernel → user space copying.

Provide the same structure definition in user space (declared in `user.h`, etc.) so kernel and user share the information format:

```c
struct physframe_info {
    uint frame_index;    // Physical frame number
    int allocated;       // 1 if allocated, 0 if free
    int pid;            // Owner process PID (-1 if none)
    uint start_tick;    // Tick when current PID started using this frame
};

#define PFNNUM 60000
struct physframe_info pf_info[PFNNUM];
```

The `dump_physmem_info()` system call should return the number of copied entries or another meaningful value (implement according to convenience).

### 4. Memory Allocator Integration

The global table must be accurately updated when `kalloc()` allocates a new page frame and when `kfree()` deallocates a page.

**kalloc():**
- When allocating a new page, find the table entry corresponding to the physical address
- Set `allocated = 1` and `pid` to the currently executing process's PID
- Kernel-used pages don't need tracking
- Record the current global tick value in the `start_tick` field
- Tick value can be obtained from the `ticks` global variable (note synchronization; `ticks` is protected by `tickslock` as it increments in interrupt handlers)

**kfree():**
- When a page is deallocated, find the frame's entry and set `allocated = 0`
- Initialize the `pid` field (e.g., set to -1) to indicate it no longer belongs to any process
- Initialize or reset `start_tick` to 0 (no longer a valid time)
- These updates must be performed just before returning the frame to the free list, and must occur while holding the memory allocator lock
- Note: xv6's `kmem` structure is protected by a spinlock along with the free list

### 5. Concurrency and Correctness

Frame tracking information must remain consistent and accurate even when multiple processes execute simultaneously in a multi-process/multi-core environment.

- Treat memory allocation/deallocation and table update operations as a single critical section for synchronization (e.g., reuse existing `kmem.lock` or protect with separate lock)
- While `dump_physmem_info()` system call reads the global table, other CPUs or processes must not modify it
- If necessary, briefly acquire the memory allocator lock in the syscall implementation for safe copying, or temporarily block interrupts/scheduling to obtain a consistent snapshot
- Release the lock after copying completes
- Note: This enables stable provision of real-time changing data to users

## Part B: Testing Required Features

### Test Programs

Three test programs will be provided (partial source code released around October 10):

**memdump.c**
- Processes frame information from `dump_physmem_info()` system call and outputs in table format
- Each row represents one physical frame with fields separated by brackets
- Default: outputs only allocated frames
- Option `-a`: outputs entire frame table (including free frames)
- Option `-p <PID>`: filters frames occupied by specific PID

**memstress.c**
- Tool for inducing state changes through dynamic allocation
- Allocates specified number of pages and optionally writes to them
- Option `-n <N>`: allocate N pages via sbrk (default 10)
- Option `-t <ticks>`: hold allocation for ticks duration before exiting (default 200)
- Option `-w`: write first byte of each allocated page to ensure physical page allocation

**memtest.c**
- Test program using memstress and memdump

**Expected Output:**

```bash
$ memtest
[memstress] pid=4 pages=31 hold=500 ticks write=0
[memstress] pid=5 pages=31 hold=500 ticks write=0
[memdump] pid=6 // checking pid 4 process
[frame#][alloc][pid][start_tick] // start_tick may vary based on memtest start time
56994 1 4 152
56995 1 4 152
56996 1 4 152
56997 1 4 152
56998 1 4 152
56999 1 4 152
57000 1 4 152
# ... (additional frames)
57341 1 4 152
[memdump] pid=7 // checking pid 5 process
[frame#][alloc][pid][start_tick]
56893 1 5 251
56894 1 5 251
56895 1 5 251
# ... (additional frames)
56966 1 5 251
[memstress] pid=4 done
[memstress] pid=5 done
[memdump] pid=8 // checking terminated pid 5 process. No output is correct
[frame#][alloc][pid][start_tick]
$
```

**Important Notes:**
- Provided code portions are absolutely not modifiable
- Must produce exactly the output format shown above

## Part C: Advanced Implementation

### (1) Software Page Walker (sw_vtop())

```c
int sw_vtop(pde_t *pgdir, const void *va, uint32_t *pa_out, uint32_t *pte_flags_out);
```

From a given process's top-level directory (pgdir) and virtual address (va), find PTE via software only and calculate/return physical address and flags.

Without hardware (emulator) access, directly parse page table entry structures in memory:
- Calculate PDE/PTE indices
- Check present/permission bits
- Combine PTE → frame number → physical address

### (2) Inverted Page Table (IPT) Implementation

**IPT Data Structure:**

```c
struct ipt_entry {
    uint32_t pfn;           // Physical frame number
    uint32_t pid;           // Owner process PID
    uint32_t va;            // Mapped virtual address (page-aligned)
    uint16_t flags;         // PTE permission snapshot (P/W/U, etc.)
    uint16_t refcnt;        // Reverse reference count (optional)
    struct ipt_entry *next; // Hash chain
};

extern struct ipt_entry *ipt_hash[IPT_BUCKETS];
```

**Insert/Delete Triggers:**
- Update IPT similarly during mapping/unmapping in `mappages()`, `uvmalloc()`/`uvmdealloc()`, `loaduvm()`, etc.

**Duplicate Mapping Handling:**
- Same physical frame can map to multiple (pid, va) pairs, so maintain as chain
- Manage `refcnt`

**Synchronization:**
- Simple spinlock protection is acceptable, but analyze cost in performance section

### (3) Software-Based TLB

- Store recent N lookups `(pid, va_page) → pa_page` in fixed-size direct-mapped or 2-way set associative cache
- Output hit rate/miss rate via /proc-style interface or debug output

### (4) Consistency

Maintain IPT and software TLB consistency during page table changes:
- Remove and update IPT during remap/munmap operations (deallocation/permission changes)
- Perform software TLB invalidation

### (5) Test Program Requirements

- Create pages with various permission combinations (P/R/W/U) and verify vtop results
- Verify IPT duplicate chains with simple COW scenario (copy-on-write) for same physical page
- Verify IPT cleanup after `exit()` (no zombies)

**Hint - User/Kernel Interface:**

System call 1:
```c
int sys_vtop(void *va, uint32_t *pa_out, uint32_t *flags_out);
```
- Call `sw_vtop` in current process context and return results

System call 2:
```c
int sys_phys2virt(uint32_t pa_page, struct vlist *out, int max);
```
- Query IPT to return up to max entries of (pid, va_page, flags) list referencing the physical page (reverse query)

**User Interface Programs:**

`vtop`:
```bash
vtop 0xBEEF1234 → PA=... flags=... hit/miss=...
```

`pfind`:
```bash
pfind 0x12345000 → List of (pid, va, flags) referencing this physical page
```

**Files Requiring Modification (Example - determine freely):**
- Kernel: `vm.c` (mappages/alloc/free), `proc.c`, `sysproc.c`, `sysfile.c`, `syscall.h/.c`, `usys.S`, `defs.h`, `mmu.h`

## Implementation Hints for Part A

### User/Kernel Interface

Add the following declarations to `user.h` (or separate shared header) to use the same structure/prototype in user space:

```c
// user.h (addition)
struct physframe_info {
    uint frame_index;    // Physical frame number (index)
    int allocated;       // 1: in use, 0: free
    int pid;            // Owner PID (use -1 or 0 for kernel-only)
    uint start_tick;    // Tick when this PID started using
};

int dump_physmem_info(void *addr, int max_entries);
```

## Important Notes

- Report should be in `.hwp`, `.doc`, or `.docx` format and include screenshots of execution results
- Submit modified xv6 source code and test shell program source code
- Exclude provided code; submit only files you modified (in principle, for memdump.c and memstress.c)
- For this project, also submit memtest.c
- Makefile submission must include builds for memdump, memstress, and memtest
- Report must include analysis of provided source code (memtest.c, memdump.c, memstress.c)

## Required Implementation

- Part A: Global frame tracking and dump_physmem_info() system call
- Part B: Test program execution and verification

## Optional Advanced Implementation

- Part C: Software page walker, IPT, software TLB, and consistency mechanisms

## Submission Format

**File naming:** `#3_StudentID_Section.zip`
- Should contain: `#3_StudentID_Section.hwp` (or `.doc`) and source code directory
- Source code directory should include:
  - All modified xv6 source code
  - Test programs: memtest.c, memdump.c, memstress.c (only if you modified them)
  - Makefile configured to build all test programs

## Testing

Rebuild and test xv6:

```bash
make clean
make
make qemu-nox
```

Run tests in xv6 shell:

```bash
$ memtest
$ memdump
$ memdump -a
$ memdump -p 4
$ memstress -n 20 -t 300 -w
```

For Part C testing:

```bash
$ vtop 0xBEEF1234
$ pfind 0x12345000
```

## Download

After implementation, download the complete project including:
- Modified kernel source files (`kalloc.c`, `proc.c`, `proc.h`, `syscall.c`, `syscall.h`, `sysproc.c`, etc.)
- For Part C: Additional files (`vm.c`, `mmu.h`, `defs.h`, etc.)
- Modified Makefile
- Test programs and their outputs
- Comprehensive report with implementation details, analysis, and performance evaluation

## Resources

- [xv6 Book](https://pdos.csail.mit.edu/6.828/2020/xv6/book-riscv-rev1.pdf)
- [MIT 6.828: Operating System Engineering](https://pdos.csail.mit.edu/6.828/)
- [Memory Management in Operating Systems](https://pages.cs.wisc.edu/~remzi/OSTEP/)
- [Inverted Page Tables Paper](https://www.cs.utexas.edu/~dahlin/Classes/UGOS/reading/IPT.pdf)