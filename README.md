# xv6 Snapshot (Checkpointing) System

## Project Overview

File system snapshots (also called checkpoints) preserve the state of a file system at a specific point in time, allowing restoration or reference to that state later. Process checkpointing is a related but separate technique.

xv6's file system does not natively support snapshots, checkpoints, or incremental backup functionality. This project implements snapshot capabilities in the xv6 file system using Copy-On-Write (COW) techniques, enabling efficient backup and recovery of storage data while learning multi-version file system design.

Using COW, new data writes preserve existing blocks and record to new blocks, allowing space-efficient snapshots without copying all data at snapshot creation time. Real file systems like Oracle's ZFS use similar COW techniques to provide fast, space-efficient snapshots with minimal performance degradation compared to non-snapshot systems.

## Project Goals

- Implement block-level Copy-On-Write (COW) for efficient snapshot management
- Add snapshot creation, rollback, and deletion functionality to xv6 file system
- Understand backup and recovery techniques for storage devices
- Design a COW-based multi-version file system

## Part A: Block-Level COW Implementation

### Current xv6 Structure

- xv6 manages file metadata and data block locations through inodes, but this design doesn't consider scenarios where one block is shared by multiple files
- Preserving file system state at a specific point requires copying all disk blocks
- This approach requires copying all blocks during snapshot creation, creating multiple duplicate blocks when only partial modifications exist

### Solution: Copy-On-Write (COW)

Instead of actual block copying during snapshot creation, introduce a reference-based approach:

- When multiple snapshots share the same block and that block is modified, a problem arises
- Modifying one block would change the content of other files referencing it, breaking snapshot restoration
- Apply COW to blocks to solve this problem

### Implementation Requirements

**Block Reference Counting Metadata File:**
- Create metadata file tracking how many files reference each block
- This metadata determines how `bcow` and `bfree` operations work
- File format is flexible (choose your own)
- Metadata file location: `/snapshot` directory

**COW Rules:**
- Snapshot files are immutable; current file system is modifiable
- All blocks referenced by snapshot files are immutable blocks
- When current file system modifies a block referenced by snapshots, apply COW to that block
- Deallocate a block only when all references to it are removed

**Directory Handling:**
- This project focuses on content recovery
- Directory data blocks don't require `bcow` application
- Don't capture `/snapshot` directory and `T_DEV` files in snapshot operations

## Part B: Snapshot System Calls

### 1. snapshot_create()

```c
int snapshot_create(void);
```

**Functionality:**
- Creates a snapshot of current file system state
- Allocates and returns a new snapshot identifier (ID)
- On success, creates read-only snapshot corresponding to the ID
- Captures current file system with `/snapshot/[ID]` directory as root
- Recursively copies all files and subdirectories from root directory to `/snapshot/[ID]` (without changing blocks)
- Records state of all files and directories on disk using COW (no actual data block copying)
- Returns value < 0 on failure

**Important Notes:**
- `/snapshot` directory and `T_DEV` files are not captured in any snapshot operations
- Uses COW, so data blocks are not copied during snapshot creation

### 2. snapshot_rollback()

```c
int snapshot_rollback(int snap_id);
```

**Functionality:**
- Restores current file system to the state of snapshot `snap_id`
- No need to preserve original inode numbers; allocate new inodes during recovery
- Returns value < 0 for invalid ID input

### 3. snapshot_delete()

```c
int snapshot_delete(int snap_id);
```

**Functionality:**
- Deletes the snapshot with specified ID
- Returns value < 0 for invalid ID input

**System Call Declaration (user.h):**
```c
int snapshot_create(void);
int snapshot_rollback(int);
int snapshot_delete(int);
```

## Part C: Test User Processes and System Calls

### 1. mk_test_file

Creates test files for verification. (Code provided, see usage in code)

```c
#include "types.h"
#include "fcntl.h"
#include "user.h"

int main(int argc, char *argv[]) {
    int fd;
    if(argc < 2) {
        printf(1, "need argv[1]\n");
        exit();
    }
    if((fd = open(argv[1], O_CREATE | O_WRONLY)) < 0) {
        printf(1, "open error for %s\n", argv[0]);
        exit();
    }
    char buf[513];
    for(int i = 1; i < 511; i++) buf[i] = 0;
    buf[511] = '\n';
    for(int i = 0; i < 12; i++) {
        buf[0] = i % 10 + '0';
        write(fd, buf, 512);
    }
    char *str = "hello\n";
    write(fd, str, 6);
    close(fd);
    exit();
}
```

**Usage:**
```bash
$ mk_test_file hi
$ print_addr hi
addr[0]: 33d
addr[1]: 33e
addr[2]: 33f
addr[3]: 340
addr[4]: 341
addr[5]: 342
addr[6]: 343
addr[7]: 344
addr[8]: 345
addr[9]: 346
addr[10]: 347
addr[11]: 348
addr[12]: 349 (INDIRECT POINTER)
addr[12]->[0](bn:12): 34a
```

### 2. append

Modifies files by appending data. (Code provided)

```c
#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h"

int main(int argc, char *argv[]) {
    if(argc != 3) {
        printf(2, "Usage: append filename string\n");
        exit();
    }
    
    // Open the file for read+write, create if needed
    int fd = open(argv[1], O_RDWR | O_CREATE);
    if(fd < 0) {
        printf(2, "append: cannot open %s\n", argv[1]);
        exit();
    }
    
    // Move offset to the end by reading until EOF
    char buf[1];
    while(read(fd, buf, 1) == 1); // drain to EOF
    
    // Now at end, write the string
    if(write(fd, argv[2], strlen(argv[2])) < 0) {
        printf(2, "append: write failed\n");
        close(fd);
        exit();
    }
    close(fd);
    exit();
}
```

**Usage:**
```bash
$ append myfile "additional text"
```

### 3. print_addr

Takes a filename as argument and prints the block addresses referenced by that file.

```bash
$ print_addr README
addr[0]: 3c
addr[1]: 3d
addr[2]: 3e
addr[3]: 3f
addr[4]: 40
```

### 4. Snapshot Management Programs

**snap_create:**
- Calls `snapshot_create()` system call to create a snapshot
- ID assignment is automatic

**Example Snapshot Test:**
```bash
$ snap_test
$ ls /snapshot/01/
.              1 27 384
..             1 2 564
README         2 282 286
cat            2 291 5684
echo           2 301 4560
forktest       2 318 996
grep           2 321 8520
init           2 331 5456
kill           2 341 4648
ln             2 351 4544
ls             2 361 7116
mkdir          2 371 4668
rm             2 381 4648
sh             2 392 8700
stressfs       2 401 5580
usertests      2 416 3072
wc             2 421 6096
zombie         2 431 4220
snap_create    2 441 4112
print_addr     2 451 4360
append         2 461 5132
mk_test_file   2 471 5332
snap_rollback  2 491 4800
```

**snap_rollback:**
- Calls `snapshot_rollback()` system call
- Restores current file system to the state of the snapshot with given ID

**snap_delete:**
- Calls `snapshot_delete()` system call
- Deletes the snapshot file with given ID

## Implementation Guidelines

### Key Files to Modify

- **fs.c / fs.h** - File system core functions, block allocation/deallocation
- **file.c / file.h** - File operations
- **sysfile.c** - System call implementations for file operations
- **syscall.c / syscall.h** - System call registration
- **user.h** - User-space system call declarations
- **usys.S** - System call assembly stubs
- **Makefile** - Add user programs to build

### COW Implementation Strategy

1. **Block Reference Counter:**
   - Maintain reference count for each disk block
   - Increment count when block is shared (snapshot creation)
   - Decrement count when reference is removed (file deletion, snapshot deletion)

2. **Block Allocation (balloc):**
   - When allocating new block, initialize reference count to 1

3. **Block Free (bfree):**
   - Decrement reference count
   - Only free block when reference count reaches 0

4. **Block COW (bcow):**
   - Check if block is shared (reference count > 1)
   - If shared and write requested:
     - Allocate new block
     - Copy data from original block to new block
     - Decrement original block's reference count
     - Update inode to point to new block

5. **Snapshot Creation:**
   - Recursively traverse directory structure from root
   - Create corresponding directory structure in `/snapshot/[ID]/`
   - Copy inode metadata (don't copy data blocks)
   - Increment reference counts for all data blocks

6. **Snapshot Rollback:**
   - Delete current file system content (except `/snapshot`)
   - Recursively copy snapshot structure to root
   - Properly manage block reference counts

7. **Snapshot Delete:**
   - Remove snapshot directory structure
   - Decrement reference counts for all blocks in snapshot
   - Free blocks when reference count reaches 0

### Synchronization Considerations

- Protect block reference counter with appropriate locks
- Use existing xv6 locks where possible (`icache.lock`, `bcache.lock`)
- Ensure atomicity of COW operations

## Testing

### Basic Functionality Test

```bash
# Create initial files
$ mk_test_file file1
$ print_addr file1

# Create snapshot
$ snap_create
Snapshot created with ID: 1

# Modify file
$ append file1 "new data"
$ print_addr file1  # Should show different blocks due to COW

# Check snapshot is unchanged
$ print_addr /snapshot/01/file1  # Should show original blocks

# Rollback
$ snap_rollback 1
$ print_addr file1  # Should match original

# Delete snapshot
$ snap_delete 1
```

### Advanced Test Scenarios

1. **Multiple Snapshots:**
   - Create snapshot 1
   - Modify files
   - Create snapshot 2
   - Verify both snapshots preserve their respective states

2. **Block Sharing:**
   - Create file
   - Create snapshot
   - Verify blocks are shared (same addresses)
   - Modify file
   - Verify COW created new blocks

3. **Reference Counting:**
   - Create file with multiple blocks
   - Create multiple snapshots
   - Delete snapshots one by one
   - Verify blocks are freed only when last reference is removed

## Important Notes

- Report should be in `.hwp`, `.doc`, or `.docx` format and include screenshots of execution results
- Submit all modified xv6 source code
- Submit all test shell program source code (including provided code for this project)
- `/snapshot` directory is reserved for snapshot storage
- Don't capture `/snapshot` directory itself in snapshots (avoid recursion)
- `T_DEV` files should not be included in snapshots

## Submission Format

**File naming:** `#4_StudentID_Section.zip`
- Should contain: `#4_StudentID_Section.hwp` (or `.doc`) and source code directory
- Source code directory should include:
  - All modified xv6 source code
  - All test programs (mk_test_file.c, append.c, print_addr.c, snap_create.c, snap_rollback.c, snap_delete.c)
  - Modified Makefile

## Expected Challenges

1. **Block Reference Tracking:**
   - Deciding data structure for reference counts
   - Efficient lookup and update mechanisms

2. **COW Trigger Points:**
   - Identifying all write operations that require COW
   - Handling direct vs indirect blocks

3. **Directory Copying:**
   - Recursive directory traversal
   - Preserving directory structure without copying blocks

4. **Concurrency:**
   - Multiple processes accessing same blocks
   - Race conditions during COW operations

5. **Edge Cases:**
   - Nested snapshots
   - Snapshot of empty file system
   - Rollback with open files
   - Deleting non-existent snapshot

## Download

After implementation, download the complete project including:
- Modified kernel source files (`fs.c`, `fs.h`, `file.c`, `sysfile.c`, etc.)
- Modified system call infrastructure (`syscall.c`, `syscall.h`, `usys.S`)
- All test programs with full source code
- Modified Makefile
- Comprehensive report with:
  - Implementation details and design decisions
  - COW mechanism explanation
  - Test results with screenshots
  - Performance analysis (optional)
  - Known limitations and future improvements

## Resources

- [xv6 Book](https://pdos.csail.mit.edu/6.828/2020/xv6/book-riscv-rev1.pdf)
- [Copy-On-Write Explanation](https://en.wikipedia.org/wiki/Copy-on-write)
- [ZFS Snapshots](https://docs.oracle.com/cd/E19253-01/819-5461/gbciq/index.html)
- [File System Snapshots Research](https://www.usenix.org/legacy/publications/library/proceedings/usenix04/tech/general/full_papers/santry/santry.pdf)

## Troubleshooting Tips

**Problem:** Blocks not being COW'd properly
- Verify reference count increments during snapshot creation
- Check all write paths trigger COW check
- Ensure COW happens before write, not after

**Problem:** Snapshot shows modified data
- Verify blocks are marked as shared before modification
- Check that new block allocation happens during COW
- Ensure inode update points to new block

**Problem:** Memory leaks / blocks not freed
- Verify reference count decrements on file deletion
- Check snapshot deletion decrements all block references
- Ensure blocks freed when count reaches 0

**Problem:** File system corruption after rollback
- Verify proper cleanup of current file system
- Check directory structure recreation
- Ensure proper inode allocation during restore