# xv6 Stride Scheduler Project

## Project Overview

This project implements a deterministic Stride Scheduling algorithm in the xv6 kernel. Unlike traditional priority or weight-based scheduling, Stride Scheduling provides proportional CPU time allocation based on ticket counts, offering more predictable and fair process execution.

## Objectives

- Implement a deterministic Stride Scheduler that allocates CPU time proportional to ticket counts
- Learn CPU allocation methods different from traditional priority-based scheduling
- Understand kernel-level scheduling algorithm implementation, process structure extension, and system call addition
- Compare different scheduling techniques and understand their trade-offs

## Background

### Round Robin Scheduling (xv6 Default)

- Basic CPU scheduling method in xv6 operating system
- Allocates equal time slices to all processes

### Lottery Scheduling

- An improved scheduler over Round Robin
- Randomly selects tickets (like a lottery) and allocates CPU to the process holding the selected ticket
- Provides weighted CPU time distribution through per-process ticket counts
- Ensures long-term fairness among processes

### Stride Scheduling

- Addresses Lottery Scheduling's short-term variability and unpredictability in real-time scenarios
- Performs deterministic ticket-based proportional distribution without random numbers
- Provides more predictable and stable scheduling
- Each process receives a stride value based on ticket count to control execution intervals
- Achieves fair CPU distribution according to ticket ratios without using random numbers
- All processes ultimately receive CPU time close to their ticket proportion

### Comparison Table

| Feature                       | Lottery Scheduling                                                        | Stride Scheduling                                                                                           |
| ----------------------------- | ------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| **Selection Method**          | Random ticket lottery                                                     | Deterministic selection by minimum pass value                                                               |
| **Short-term Fairness**       | Short-term deviation exists; some processes may be selected consecutively | Minimal short-term deviation; strict alternation keeps execution close to ticket ratios (bounded deviation) |
| **Random Number Usage**       | Required (lottery via RNG)                                                | Not required (no lottery, fixed algorithm)                                                                  |
| **Predictability**            | Low (results vary per execution, proportional only statistically)         | High (repeatable results with consistent proportional distribution)                                         |
| **Implementation Complexity** | Relatively simple (ticket sum and lottery)                                | Slightly complex (stride/pass management and overflow handling)                                             |

## Implementation Requirements

### 1. Basic Setup and Implementation

#### (1) Makefile Configuration

Modify the Makefile to run the provided test files. Since test files are pre-compiled binaries, add them only to `EXTRA`, not `UPROGS`.

**Example Makefile modification:**
```makefile
# ... (omitted) ...
UPROGS=\
	_cat\
	_echo\
	# ... (middle section omitted) ...
	_wc\
	_zombie\
	_debug_test\      # Add to UPROGS only
	_syscall_test\
	_scheduler_test\

fs.img: mkfs README $(UPROGS)
	./mkfs fs.img README $(UPROGS)
# ... (omitted) ...
```

#### (2) Extend proc Structure and Define Stride Scheduler Constants

Extend the `proc` structure in `proc.h`:

```c
// ... (omitted) ...
struct proc {
    // ... (middle section omitted) ...
    struct inode *cwd;              // Current directory
    char name[16];                  // Process name (debugging)
    int tickets;
    uint stride;                    // Default: 0 (set by setticket)
    uint pass;                      // Default: 0 (set by setticket)
    int ticks;                      // Default: 0
    int end_ticks;                  // Default: -1 (process exits when ticks reaches end_ticks if positive)
};
// ... (middle section omitted) ...

#define STRIDE_MAX 100000
#define PASS_MAX 15000
#define DISTANCE_MAX 7500
```

#### (3) Implement Debug Code

Implement debug output required for grading. Debug output should only be printed when both the process and its parent have `pid > 2`.

**Three debug output scenarios:**

1. **Just before a CPU-mode process yields control to the scheduler**

Example location in `trap.c`:
```c
// ... (omitted) ...
void
trap(struct trapframe *tf)
{
    // ... (middle section omitted) ...
    if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
        exit();
    
    // Force process to give up CPU on clock tick
    if(myproc() && myproc()->state == RUNNING &&
       tf->trapno == T_IRQ0+IRQ_TIMER)
        yield();
    
    if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
        exit();
}
// ... (omitted) ...
```

2. **Immediately after process creation completes**

Example location in `proc.c`:
```c
// ... (omitted) ...
int
fork(void)
{
    // ... (middle section omitted) ...
    np->state = RUNNABLE;
    release(&ptable.lock);
    return pid;
}
// ... (omitted) ...
```

3. **When a process terminates**

Example location in `proc.c`:
```c
// ... (omitted) ...
void
exit(void)
{
    // ... (middle section omitted) ...
    curproc->state = ZOMBIE;
    sched();
    panic("zombie exit");
}
// ... (omitted) ...
```

**Debug Output Format:**

When a CPU-mode process yields control to the scheduler:
```
Process 4 selected, stride: 0, ticket: 1, pass: 0 -> 0 (1/-1)
```

Format breakdown:
- `"Process 4 selected, "`: Output the selected process's PID
- `"stride: 0, ticket: 1, "`: Output the selected process's stride and ticket values
- `"pass: 0 -> 0 "`: Output the change in pass value
- `"(1/-1)"`: Output total CPU ticks occupied and end tick count (output -1 if no end tick)

**Example debug_test output:**
```bash
$ debug_test
Process 4 start
Process 4 selected, stride: 0, ticket: 1, pass: 0 -> 0 (1/-1)
Process 4 selected, stride: 0, ticket: 1, pass: 0 -> 0 (2/-1)
Process 4 selected, stride: 0, ticket: 1, pass: 0 -> 0 (3/-1)
# ... (middle section omitted) ...
Process 4 selected, stride: 0, ticket: 1, pass: 0 -> 0 (64/-1)
result: 1540746712 (debug_test process output)
Process 4 exit
```

### 2. Implement settickets System Call

#### (1) System Call Specification

```c
int settickets(int tickets, int end_ticks)
```

- Calculates stride value from the first argument (tickets)
- Determines process lifetime from the second argument (end_ticks)
- First argument validation: `tickets` must be ≥ 1 and ≤ `STRIDE_MAX`, otherwise return -1
- Second argument handling: If `end_ticks` is not ≥ 1, ignore it
- **System call number must be 22**
- **Note:** Remove any content from Project #1 before starting. All projects are independent.

#### (2) Update Process Stride Value

```c
stride = STRIDE_MAX / tickets
```
- Use C language integer division (discard remainder)

**Example syscall_test output:**
```bash
$ syscall_test
Process 4 start
Process 4 selected, stride: 1000, ticket: 100, pass: 0 -> 1000 (1/6)
Process 4 selected, stride: 1000, ticket: 100, pass: 1000 -> 2000 (2/6)
Process 4 selected, stride: 1000, ticket: 100, pass: 2000 -> 3000 (3/6)
Process 4 selected, stride: 1000, ticket: 100, pass: 3000 -> 4000 (4/6)
Process 4 selected, stride: 1000, ticket: 100, pass: 4000 -> 5000 (5/6)
Process 4 selected, stride: 1000, ticket: 100, pass: 5000 -> 6000 (6/6)
Process 4 exit
```

### 3. Stride-Based scheduler() Function

Modify xv6 kernel's scheduler function to implement Stride Scheduling.

**Note:** Implementation may require accessing functions and files beyond the `scheduler()` function.

#### Process Selection Algorithm

1. **Consider only RUNNABLE processes**
2. **Process table locking:** Acquire process table lock during search
3. **No runnable processes:** If no runnable process exists, release lock and repeat search
4. **Select minimum pass:** Among RUNNABLE processes, select the one with the smallest `pass` value
5. **Tie-breaking:** If multiple processes have the same minimum pass, select the one with the smallest `pid`
6. **CPU allocation:** Grant control for 1 tick, then select next process when CPU is released

#### Pass Value Update

- When a process completes 1 tick of CPU time and yields control, increase its `pass` by its `stride` value
- `stride` is set via `setticket()` system call; higher tickets result in lower stride
- Lower stride means `pass` increases less per tick compared to higher stride processes, leading to higher execution probability
- Ultimately, all processes receive CPU time close to their ticket proportion

#### Overflow Prevention

- Continuous `pass` increment can cause overflow
- Implement overflow prevention mechanism
- When any process's `pass` exceeds `PASS_MAX`:
  - Find the minimum `pass` value among all RUNNABLE processes
  - Set that minimum to 0
  - Decrease all RUNNABLE processes' `pass` values by the same amount (i.e., by the minimum pass value)
- To prevent excessive overhead from frequent overflow prevention:
  - Ensure the difference between any two processes' `pass` values doesn't exceed `DISTANCE_MAX`
  - Some processes may have their `pass` decreased more than the minimum pass value

**Example scheduler_test 1 output:**
```bash
$ scheduler_test 1
test 1 start
Process 4 start
Process 5 start
# ... (middle section omitted) ...
Process 6 selected, stride: 100, ticket: 1000, pass: 1900 -> 2000 (20/20)
Process 6 exit
test 1 end
```

**Example scheduler_test 4 output:**
```bash
$ scheduler_test 4
test 4 start
Process 8 start
Process 9 start
Process 10 start
Process 8 selected, stride: 9090, ticket: 11, pass: 0 -> 9090 (1/20)
Process 9 selected, stride: 4347, ticket: 23, pass: 0 -> 4347 (1/20)
Process 10 selected, stride: 3571, ticket: 28, pass: 0 -> 3571 (1/20)
# ... (middle section omitted) ...
Rebase Process Start
Process 8's pass is standardize from 18180, to 0
Rebase Process End
Process 8 selected, stride: 9090, ticket: 11, pass: 0 -> 9090 (20/20)
Process 8 exit
test 4 end
```

**Example scheduler_test (all tests) output:**
```bash
$ scheduler_test
test 1 start
Process 4 start
Process 5 start
Process 6 start
Process 4 selected, stride: 100, ticket: 1000, pass: 0 -> 100 (1/20)
Process 5 selected, stride: 100, ticket: 1000, pass: 0 -> 100 (1/20)
Process 6 selected, stride: 100, ticket: 1000, pass: 0 -> 100 (1/20)
Process 4 selected, stride: 100, ticket: 1000, pass: 100 -> 200 (2/20)
Process 5 selected, stride: 100, ticket: 1000, pass: 100 -> 200 (2/20)
Process 6 selected, stride: 100, ticket: 1000, pass: 100 -> 200 (2/20)
# ... (middle section omitted) ...
Rebase Process Start
Process 19's pass is standardize from 15438, with distance cutting, to 7500
Process 22's pass is standardize from 7142, to 0
Rebase Process End
Process 22 selected, stride: 7142, ticket: 14, pass: 0 -> 7142 (9/10)
Process 22 selected, stride: 7142, ticket: 14, pass: 7142 -> 14284 (10/10)
Process 22 exit
Process 19 selected, stride: 11111, ticket: 9, pass: 7500 -> 18611 (10/10)
Rebase Process Start
Process 19's pass is standardize from 18611, to 0
Rebase Process End
Process 19 exit
test 5 end
```

## Important Notes

- Report should be in `.hwp`, `.doc`, or `.docx` format and include screenshots of execution results
- Submit all modified files
- **Project #2 source code must NOT include any Project #1 code**
- Project #1 modifications and implementations are completely unrelated to Project #2
- For example, system call numbers should match the original xv6 source, with no Project #1 additions
- Including Project #1 content will result in penalty

## Submission Format

**File naming:** `#2_StudentID_Section.zip`
- Should contain: `#2_StudentID_Section.hwp` (or `.doc`) and source code directory
- Source code directory should include:
  - All modified xv6 source code
  - Any added source code
  - Test shell program source code

## Testing

Rebuild and test xv6:
```bash
make clean
make
make qemu-nox
```

Run tests in xv6 shell:
```bash
$ debug_test
$ syscall_test
$ scheduler_test 1
$ scheduler_test 4
$ scheduler_test
```

## Download

After implementation, download the complete project including:
- Modified kernel source files (`proc.c`, `proc.h`, `syscall.c`, `syscall.h`, `sysproc.c`, `trap.c`, etc.)
- Modified Makefile
- All test programs and their outputs
- Comprehensive report with implementation details and analysis

## Resources

- [xv6 Book](https://pdos.csail.mit.edu/6.828/2020/xv6/book-riscv-rev1.pdf)
- [MIT 6.828: Operating System Engineering](https://pdos.csail.mit.edu/6.828/)
- [Stride Scheduling Paper](https://people.eecs.berkeley.edu/~kubitron/courses/cs194-24-S13/hand-outs/stride-sched.pdf)