# xv6 System Calls Project

## Project Overview

This project involves installing xv6, implementing custom system calls, and creating user applications to test them. You'll learn how to extend the xv6 operating system by adding new system calls and building programs that utilize them.

## Objectives

- Install and compile xv6
- Implement user applications `helloxv6` and `psinfo` that demonstrate system call usage
- Add two custom system calls to xv6: `hello_number()` and `get_procinfo()`
- Test the implementations through shell execution

## Background

### About xv6

- An educational operating system developed at MIT for x86 multiprocessor and RISC-V systems
- Implemented in ANSI C based on UNIX V6
- Unlike Linux or BSD, xv6 is simple yet contains essential OS concepts and structures

### Key Concepts

**Cross Compilation**
- xv6 has no built-in text editor or compiler
- Write and compile programs on your Linux host system using gcc
- Execute the resulting binaries within xv6

**System Call Implementation Patterns**
- Simple calls with no arguments, returning integers (e.g., `uptime()` in `sysproc.c`)
- Calls accepting multiple arguments and returning simple values (e.g., `open()` in `sysfile.c`)
- Calls returning complex data via user-defined structures (e.g., `fstat()` with `struct stat`)

### xv6 Kernel Structure

Files you'll need to modify:
- `user.h` - System call definitions for xv6
- `usys.S` - System call list
- `syscall.h` - System call number mappings
- `syscall.c` - System call dispatcher and argument parsing
- `sysproc.c` - Process-related system call implementations
- `proc.h` - Process structure definitions
- `proc.c` - Process scheduling and context switching

## Setup

### 1. Download xv6

```bash
git clone https://github.com/mit-pdos/xv6-public
cd xv6-public
```

### 2. Install QEMU

The xv6 operating system runs on QEMU, an x86 hardware emulator.

```bash
apt-get install qemu-kvm
```

### 3. Build and Run xv6

```bash
make
make qemu-nox
```

You should see the xv6 boot sequence and shell prompt. Test with `ls` command.

## Implementation Tasks

### Task 1: Add `hello_number` System Call

Implement a system call that accepts an integer argument, prints a message in the kernel, and returns a computed value.

**Specification:**
```c
int hello_number(int n);
```
- Accepts an integer `n` (positive or negative)
- Prints "Hello, xv6! Your number is n" to kernel console
- Returns `n * 2` to the calling process
- Assumes input values won't overflow 32-bit integer range

**Implementation Steps:**

1. **syscall.h** - Define system call number
```c
#define SYS_hello_number 22
```

2. **sysproc.c** - Implement kernel handler
```c
int
sys_hello_number(void) {
    int n;
    if(argint(0, &n) < 0)
        return -1;
    cprintf("Hello, xv6! Your number is %d\n", n);
    return n * 2;
}
```

3. **syscall.c** - Register the system call
```c
extern int sys_hello_number(void);

// In syscalls[] array:
[SYS_hello_number] sys_hello_number,
```

4. **usys.S** - Add assembly stub
```c
SYSCALL(hello_number)
```

5. **user.h** - Add user-space prototype
```c
int hello_number(int n);
```

### Task 2: Add `get_procinfo` System Call

Implement a system call that retrieves process information.

**Specification:**
```c
int get_procinfo(int pid, struct procinfo *uinfo);
```
- If `pid > 0`: Query information for the specified PID
- If `pid <= 0`: Query information for the calling process
- Returns 0 on success, -1 on failure (invalid PID, pointer error, etc.)

**Data Structure:**

Define in both `user.h` and `sysproc.c`:
```c
struct procinfo {
    int pid;           // Target PID
    int ppid;          // Parent PID
    int state;         // Process state (enum procstate)
    uint sz;           // Memory size in bytes
    char name[16];     // Process name
};
```

**Implementation in sysproc.c:**
```c
struct k_procinfo {
    int pid, ppid, state; 
    uint sz; 
    char name[16];
};

int sys_get_procinfo(void) {
    int pid;
    char *uaddr;
    struct proc *p, *t;
    struct k_procinfo kinfo;
    
    if(argint(0, &pid) < 0) return -1;
    if(argptr(1, &uaddr, sizeof(struct k_procinfo)) < 0) return -1;
    
    acquire(&ptable.lock);
    if(pid <= 0) 
        t = myproc();
    else {
        t = 0;
        for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
            if(p->pid == pid) { t = p; break; }
    }
    
    if(t == 0 || (t->state == UNUSED)) { 
        release(&ptable.lock); 
        return -1; 
    }
    
    // TODO: Fill kinfo with process data
    
    if(copyout(myproc()->pgdir, (uint)uaddr, (void*)&kinfo, sizeof(kinfo)) < 0)
        return -1;
    
    return 0;
}
```

Follow the same registration steps as `hello_number` for `syscall.h`, `usys.S`, `syscall.c`, and `user.h`.

### Task 3: Create User Applications

**helloxv6.c**

```c
#include "types.h"
#include "stat.h"
#include "user.h"

int main(int argc, char *argv[]) {
    int res = hello_number(5);
    printf(1, "hello_number(5) returned %d\n", res);
    
    // Test with negative number
    int res2 = hello_number(-7);
    printf(1, "hello_number(-7) returned %d\n", res2);
    
    exit();
}
```

**psinfo.c**

```c
#include "types.h"
#include "stat.h"
#include "user.h"

static char* s2str(int s){
    switch(s){
        case 0: return "UNUSED";
        case 1: return "EMBRYO";
        case 2: return "SLEEPING";
        case 3: return "RUNNABLE";
        case 4: return "RUNNING";
        case 5: return "ZOMBIE";
    }
    return "UNKNOWN";
}

int main(int argc, char *argv[]){
    struct procinfo info;
    int pid = (argc >= 2) ? atoi(argv[1]) : 0;
    
    // TODO: Call get_procinfo and fill info structure
    
    printf(1, "PID=%d PPID=%d STATE=%s SZ=%d NAME=%s\n",
           info.pid, info.ppid, s2str(info.state), info.sz, info.name);
    exit();
}
```

### Task 4: Register User Programs

Edit the `Makefile` and add to the `UPROGS` variable:
```makefile
_helloxv6\
_psinfo\
```

## Testing

1. Rebuild xv6:
```bash
make clean
make
make qemu-nox
```

2. In the xv6 shell, run:
```bash
$ helloxv6
$ psinfo
$ psinfo 1
```

## Expected Output

**helloxv6:**
```
Hello, xv6! Your number is 5
hello_number(5) returned 10
Hello, xv6! Your number is -7
hello_number(-7) returned -14
```

**psinfo:**
```
PID=3 PPID=2 STATE=RUNNING SZ=12288 NAME=sh
```

## Download

You can download the complete project files after implementation. Make sure all modified source files and test programs are included.

## Notes

- All source code modifications should be documented
- Test both system calls thoroughly with various inputs
- Capture screenshots of installation and execution process
- Analyze the provided source code and document your understanding

## Resources

- [xv6 Book](https://pdos.csail.mit.edu/6.828/2020/xv6/book-riscv-rev1.pdf)
- [MIT 6.828: Operating System Engineering](https://pdos.csail.mit.edu/6.828/)