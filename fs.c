// File system implementation.  Five layers:
//   + Blocks: allocator for raw disk blocks.
//   + Log: crash recovery for multi-step updates.
//   + Files: inode allocator, reading, writing, metadata.
//   + Directories: inode with special contents (list of other inodes!)
//   + Names: paths like /usr/rtm/xv6/fs.c for convenient naming.
//
// This file contains the low-level file system manipulation
// routines.  The (higher-level) system call implementations
// are in sysfile.c.

#include "types.h"
#include "defs.h"
#include "param.h"
#include "stat.h"
#include "mmu.h"
#include "proc.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "buf.h"
#include "file.h"

#define NINODES 200
#define min(a, b) ((a) < (b) ? (a) : (b))

// size of all blocks in file system
#define NBLOCKS ((FSSIZE - (2 + LOGSIZE + NINODES / IPB + 1 + FSSIZE / (BSIZE * 8) + 1)))
// size of cow bitmap
// element type of array is uchar, which size is 8 bits.
// per element, it stores 8 blocks' cow information
#define SSMAP (NBLOCKS / 8)

// truncate inodes which releasing all block addresses of inode
static void itrunc(struct inode *);
// this function simulates scandir of libc
// given inode, it recursively loops all entries in that directory
// if any match with given filter, it stores dirent data and offset to pointer in parameter
// if there is no more entry, it returns negative number
// if unavailable entry found, then it returns 0
// else it returns positive number
static int dirnext(struct inode *, int (*)(struct dirent *), struct dirent *, uint *);
// directly copied from sysfile.c
// check given directory is empty
static int isdirempty(struct inode *dp);
// filter function which filters out . and ..
static int filter_dots(struct dirent *de);
// save current snapshot_meta to file
int update_snapshot_meta();
// directly copied from sysfile.c
// remove file
static int dirunlink(struct inode *dp, char *name);
// create file
static struct inode *create(struct inode *, char *, short, short, short);
// there should be one superblock per disk device, but we run with
// only one device
struct superblock sb;
struct inode *get_snapshot_root(struct inode *);
// container struct of snapshot metadata
struct snapshot_meta
{
  // snapshot id counter
  // increase when snapshot is created
  uint next_id;
  // snapshot bitmap for blocks
  uchar smap[SSMAP];
};

// this lock is only used for restricted conditions
// because almost operations on smap are executed with locked by ilock
// this only requires before handling snapshots, such as incrementing snapshot id
struct spinlock smap_lock;

// global variable of snapshot meta
static struct snapshot_meta smeta;

uint snapshot_id = 0;
// Read the super block.
void readsb(int dev, struct superblock *sb)
{
  struct buf *bp;

  bp = bread(dev, 1);
  memmove(sb, bp->data, sizeof(*sb));
  brelse(bp);
}

// Zero a block.
static void
bzero(int dev, int bno)
{
  struct buf *bp;

  bp = bread(dev, bno);
  memset(bp->data, 0, BSIZE);
  log_write(bp);
  brelse(bp);
}

// Blocks.

// Allocate a zeroed disk block.
static uint
balloc(uint dev)
{
  int b, bi, m;
  struct buf *bp;

  bp = 0;
  for (b = 0; b < sb.size; b += BPB)
  {
    bp = bread(dev, BBLOCK(b, sb));
    for (bi = 0; bi < BPB && b + bi < sb.size; bi++)
    {
      m = 1 << (bi % 8);
      if ((bp->data[bi / 8] & m) == 0)
      {                        // Is block free?
        bp->data[bi / 8] |= m; // Mark block in use.
        log_write(bp);
        brelse(bp);
        bzero(dev, b + bi);
        return b + bi;
      }
    }
    brelse(bp);
  }
  panic("balloc: out of blocks");
}

// Free a disk block.
static void
bfree(int dev, uint b)
{
  struct buf *bp;
  int bi, m;

  m = 1 << (b % 8);

  // if bcow is enabled on this block, then don't free it
  if ((smeta.smap[b / 8] & m) != 0)
  {
    return;
  }
  bp = bread(dev, BBLOCK(b, sb));
  bi = b % BPB;
  m = 1 << (bi % 8);
  if ((bp->data[bi / 8] & m) == 0)
    panic("freeing free block");
  bp->data[bi / 8] &= ~m;
  log_write(bp);
  brelse(bp);
}

// Inodes.
//
// An inode describes a single unnamed file.
// The inode disk structure holds metadata: the file's type,
// its size, the number of links referring to it, and the
// list of blocks holding the file's content.
//
// The inodes are laid out sequentially on disk at
// sb.startinode. Each inode has a number, indicating its
// position on the disk.
//
// The kernel keeps a cache of in-use inodes in memory
// to provide a place for synchronizing access
// to inodes used by multiple processes. The cached
// inodes include book-keeping information that is
// not stored on disk: ip->ref and ip->valid.
//
// An inode and its in-memory representation go through a
// sequence of states before they can be used by the
// rest of the file system code.
//
// * Allocation: an inode is allocated if its type (on disk)
//   is non-zero. ialloc() allocates, and iput() frees if
//   the reference and link counts have fallen to zero.
//
// * Referencing in cache: an entry in the inode cache
//   is free if ip->ref is zero. Otherwise ip->ref tracks
//   the number of in-memory pointers to the entry (open
//   files and current directories). iget() finds or
//   creates a cache entry and increments its ref; iput()
//   decrements ref.
//
// * Valid: the information (type, size, &c) in an inode
//   cache entry is only correct when ip->valid is 1.
//   ilock() reads the inode from
//   the disk and sets ip->valid, while iput() clears
//   ip->valid if ip->ref has fallen to zero.
//
// * Locked: file system code may only examine and modify
//   the information in an inode and its content if it
//   has first locked the inode.
//
// Thus a typical sequence is:
//   ip = iget(dev, inum)
//   ilock(ip)
//   ... examine and modify ip->xxx ...
//   iunlock(ip)
//   iput(ip)
//
// ilock() is separate from iget() so that system calls can
// get a long-term reference to an inode (as for an open file)
// and only lock it for short periods (e.g., in read()).
// The separation also helps avoid deadlock and races during
// pathname lookup. iget() increments ip->ref so that the inode
// stays cached and pointers to it remain valid.
//
// Many internal file system functions expect the caller to
// have locked the inodes involved; this lets callers create
// multi-step atomic operations.
//
// The icache.lock spin-lock protects the allocation of icache
// entries. Since ip->ref indicates whether an entry is free,
// and ip->dev and ip->inum indicate which i-node an entry
// holds, one must hold icache.lock while using any of those fields.
//
// An ip->lock sleep-lock protects all ip-> fields other than ref,
// dev, and inum.  One must hold ip->lock in order to
// read or write that inode's ip->valid, ip->size, ip->type, &c.

struct
{
  struct spinlock lock;
  struct inode inode[NINODE];
} icache;

void iinit(int dev)
{
  int i = 0;

  initlock(&icache.lock, "icache");
  for (i = 0; i < NINODE; i++)
  {
    initsleeplock(&icache.inode[i].lock, "inode");
  }

  readsb(dev, &sb);
  cprintf("sb: size %d nblocks %d ninodes %d nlog %d logstart %d\
 inodestart %d bmap start %d\n",
          sb.size, sb.nblocks,
          sb.ninodes, sb.nlog, sb.logstart, sb.inodestart,
          sb.bmapstart);
}

static struct inode *iget(uint dev, uint inum);
static struct inode *iget_safe(uint dev, uint inum);
// PAGEBREAK!
//  Allocate an inode on device dev.
//  Mark it as allocated by  giving it type type.
//  Returns an unlocked but allocated and referenced inode.
struct inode *ialloc(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for (inum = 1; inum < sb.ninodes; inum++)
  {
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode *)bp->data + inum % IPB;
    if (dip->type == 0)
    { // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp); // mark it allocated on the disk
      brelse(bp);
      return iget(dev, inum);
    }
    brelse(bp);
  }
  panic("ialloc: no inodes");
}

struct inode *
ialloc_safe(uint dev, short type)
{
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for (inum = 1; inum < sb.ninodes; inum++)
  {
    bp = bread(dev, IBLOCK(inum, sb));
    dip = (struct dinode *)bp->data + inum % IPB;
    if (dip->type == 0)
    { // a free inode
      memset(dip, 0, sizeof(*dip));
      dip->type = type;
      log_write(bp); // mark it allocated on the disk
      brelse(bp);
      return iget_safe(dev, inum);
    }
    brelse(bp);
  }
  return 0;
}

// Copy a modified in-memory inode to disk.
// Must be called after every change to an ip->xxx field
// that lives on disk, since i-node cache is write-through.
// Caller must hold ip->lock.
void iupdate(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  bp = bread(ip->dev, IBLOCK(ip->inum, sb));
  dip = (struct dinode *)bp->data + ip->inum % IPB;
  dip->type = ip->type;
  dip->major = ip->major;
  dip->minor = ip->minor;
  dip->nlink = ip->nlink;
  dip->size = ip->size;
  memmove(dip->addrs, ip->addrs, sizeof(ip->addrs));
  log_write(bp);
  brelse(bp);
}

// Find the inode with number inum on device dev
// and return the in-memory copy. Does not lock
// the inode and does not read it from disk.
static struct inode *
iget(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++)
  {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum)
    {
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0) // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if (empty == 0)
    panic("iget: no inodes");

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

static struct inode *
iget_safe(uint dev, uint inum)
{
  struct inode *ip, *empty;

  acquire(&icache.lock);

  // Is the inode already cached?
  empty = 0;
  for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++)
  {
    if (ip->ref > 0 && ip->dev == dev && ip->inum == inum)
    {
      ip->ref++;
      release(&icache.lock);
      return ip;
    }
    if (empty == 0 && ip->ref == 0) // Remember empty slot.
      empty = ip;
  }

  // Recycle an inode cache entry.
  if (empty == 0)
    return 0;

  ip = empty;
  ip->dev = dev;
  ip->inum = inum;
  ip->ref = 1;
  ip->valid = 0;
  release(&icache.lock);

  return ip;
}

static int micount()
{
  struct inode *ip;

  acquire(&icache.lock);
  int count = 0;

  for (ip = &icache.inode[0]; ip < &icache.inode[NINODE]; ip++)
  {
    if (ip->ref > 0 || ip->valid || ip->nlink > 0)
    {
      count++;
    }
  }
  release(&icache.lock);

  return count;
}

// Increment reference count for ip.
// Returns ip to enable ip = idup(ip1) idiom.
struct inode *
idup(struct inode *ip)
{
  acquire(&icache.lock);
  ip->ref++;
  release(&icache.lock);
  return ip;
}

// Lock the given inode.
// Reads the inode from disk if necessary.
void ilock(struct inode *ip)
{
  struct buf *bp;
  struct dinode *dip;

  if (ip == 0 || ip->ref < 1)
    panic("ilock");

  acquiresleep(&ip->lock);

  if (ip->valid == 0)
  {
    bp = bread(ip->dev, IBLOCK(ip->inum, sb));
    dip = (struct dinode *)bp->data + ip->inum % IPB;
    ip->type = dip->type;
    ip->major = dip->major;
    ip->minor = dip->minor;
    ip->nlink = dip->nlink;
    ip->size = dip->size;

    memmove(ip->addrs, dip->addrs, sizeof(ip->addrs));
    brelse(bp);
    ip->valid = 1;
    if (ip->type == 0)
      panic("ilock: no type");
  }
}

// Unlock the given inode.
void iunlock(struct inode *ip)
{
  if (ip == 0 || !holdingsleep(&ip->lock) || ip->ref < 1)
    panic("iunlock");

  releasesleep(&ip->lock);
}

// Drop a reference to an in-memory inode.
// If that was the last reference, the inode cache entry can
// be recycled.
// If that was the last reference and the inode has no links
// to it, free the inode (and its content) on disk.
// All calls to iput() must be inside a transaction in
// case it has to free the inode.
void iput(struct inode *ip)
{
  acquiresleep(&ip->lock);
  if (ip->valid && ip->nlink == 0)
  {
    acquire(&icache.lock);
    int r = ip->ref;
    release(&icache.lock);
    if (r == 1)
    {
      // inode has no links and no other references: truncate and free.
      itrunc(ip);
      ip->type = 0;
      iupdate(ip);
      ip->valid = 0;
    }
  }
  releasesleep(&ip->lock);

  acquire(&icache.lock);
  ip->ref--;
  release(&icache.lock);
}

// loops all inodes in inode block
// to count allocated(being used) inodes
int s_isize()
{
  int size = 0;
  int inum;
  struct buf *bp;
  struct dinode *dip;

  for (inum = 1; inum < sb.ninodes; inum++)
  {
    bp = bread(ROOTDEV, IBLOCK(inum, sb));
    dip = (struct dinode *)bp->data + inum % IPB;
    // that inode is being used
    if (dip->type != 0)
    {
      size++;
    }
    brelse(bp);
  }

  return size;
}

// recursively counts inodes (files and directories) in a subtree.
// this function handles its own inode locking and iget/iput balancing.
static int icount(struct inode *ip)
{
  struct dirent de;
  struct inode *nip;
  uint off;
  int total;

  if (ip->type != T_DIR)
  {
    return 1;
  }

  total = 1;

  for (off = 0; off < ip->size; off += sizeof(de))
  {
    if (readi(ip, (char *)&de, off, sizeof(de)) != sizeof(de))
    {
      iunlock(ip);
      panic("icount: readi");
    }
    if (de.inum == 0)
      continue;

    if (namecmp(de.name, ".") == 0 || namecmp(de.name, "..") == 0)
      continue;

    // do not count root inode and snapshot folder
    if (ip->inum == ROOTINO && namecmp(de.name, "snapshot") == 0)
      continue;

    nip = iget(ip->dev, de.inum);
    if (nip == 0)
      continue;
    iunlock(ip);

    // recursively add all children
    ilock(nip);
    total += icount(nip);
    iunlockput(nip);
    ilock(ip);
  }
  return total;
}

// Common idiom: unlock, then put.
void iunlockput(struct inode *ip)
{
  iunlock(ip);
  iput(ip);
}

// PAGEBREAK!
//  Inode content
//
//  The content (data) associated with each inode is stored
//  in blocks on the disk. The first NDIRECT block numbers
//  are listed in ip->addrs[].  The next NINDIRECT blocks are
//  listed in block ip->addrs[NDIRECT].

// Return the disk block address of the nth block in inode ip.
// If there is no such block, bmap allocates one.
static uint
bmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if (bn < NDIRECT)
  {
    if ((addr = ip->addrs[bn]) == 0)
      ip->addrs[bn] = addr = balloc(ip->dev);
    return addr;
  }
  bn -= NDIRECT;

  if (bn < NINDIRECT)
  {
    // Load indirect block, allocating if necessary.
    if ((addr = ip->addrs[NDIRECT]) == 0)
      ip->addrs[NDIRECT] = addr = balloc(ip->dev);
    bp = bread(ip->dev, addr);
    a = (uint *)bp->data;
    if ((addr = a[bn]) == 0)
    {
      a[bn] = addr = balloc(ip->dev);
      log_write(bp);
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

// it works similarly to bmap
// retrieves address of given block number
// but bmap tries to allocate new block if address does not exists
// this function doesn't allocate new block
static uint bmmap(struct inode *ip, uint bn)
{
  uint addr, *a;
  struct buf *bp;

  if (bn < NDIRECT)
  {
    if ((addr = ip->addrs[bn]) == 0)
      return 0;
    return addr;
  }
  bn -= NDIRECT;

  if (bn < NINDIRECT)
  {
    // Load indirect block, allocating if necessary.
    if ((addr = ip->addrs[NDIRECT]) == 0)
      return 0;
    bp = bread(ip->dev, addr);
    a = (uint *)bp->data;
    if ((addr = a[bn]) == 0)
    {
      brelse(bp);
      return 0;
    }
    brelse(bp);
    return addr;
  }

  panic("bmap: out of range");
}

// Truncate inode (discard contents).
// Only called when the inode has no links
// to it (no directory entries referring to it)
// and has no in-memory reference to it (is
// not an open file or current directory).
static void
itrunc(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;

  for (i = 0; i < NDIRECT; i++)
  {
    if (ip->addrs[i])
    {
      bfree(ip->dev, ip->addrs[i]);
      ip->addrs[i] = 0;
    }
  }

  if (ip->addrs[NDIRECT])
  {
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint *)bp->data;
    for (j = 0; j < NINDIRECT; j++)
    {
      if (a[j])
      {
        bfree(ip->dev, a[j]);
      }
    }
    brelse(bp);
    bfree(ip->dev, ip->addrs[NDIRECT]);
    ip->addrs[NDIRECT] = 0;
  }

  ip->size = 0;
  iupdate(ip);
}

// helper function for marking all addresses hold by the inode to smap array, including indirect addresses
static void smapi(struct inode *ip)
{
  int i, j;
  struct buf *bp;
  uint *a;
  uint b, x;
  if (ip->type != T_FILE)
    return;

  // direct addresses
  for (i = 0; i < NDIRECT; i++)
  {
    if (ip->addrs[i])
    {

      // get index of smap
      b = ip->addrs[i] / 8;
      // and bit index inside smap element
      x = 1 << (ip->addrs[i] % 8);
      // turn on bit
      smeta.smap[b] |= x;
    }
  }

  if (ip->addrs[NDIRECT])
  {
    bp = bread(ip->dev, ip->addrs[NDIRECT]);
    a = (uint *)bp->data;
    for (j = 0; j < NINDIRECT; j++)
    {
      if (a[j])
      {
        b = a[j] / 8;
        x = 1 << (a[j] % 8);
        smeta.smap[b] |= x;
      }
    }

    b = ip->addrs[NDIRECT] / 8;
    x = 1 << (ip->addrs[NDIRECT] % 8);
    smeta.smap[b] |= x;
    brelse(bp);
  }
}

// Copy stat information from inode.
// Caller must hold ip->lock.
void stati(struct inode *ip, struct stat *st)
{
  st->dev = ip->dev;
  st->ino = ip->inum;
  st->type = ip->type;
  st->nlink = ip->nlink;
  st->size = ip->size;
}

// PAGEBREAK!
//  Read data from inode.
//  Caller must hold ip->lock.
int readi(struct inode *ip, char *dst, uint off, uint n)
{
  uint tot, m;
  struct buf *bp;

  if (ip->type == T_DEV)
  {
    if (ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].read)
      return -1;
    return devsw[ip->major].read(ip, dst, n);
  }

  if (off > ip->size || off + n < off)
    return -1;
  if (off + n > ip->size)
    n = ip->size - off;

  for (tot = 0; tot < n; tot += m, off += m, dst += m)
  {
    bp = bread(ip->dev, bmap(ip, off / BSIZE));
    m = min(n - tot, BSIZE - off % BSIZE);
    memmove(dst, bp->data + off % BSIZE, m);
    brelse(bp);
  }
  return n;
}

// PAGEBREAK!
// Write data to inode.
// Caller must hold ip->lock.
// when writing data to block, it checks smap to if bcow bit is enabled on each block
// if enabled, it allocates new block for that address
// if target block address is indirect, it allocates entire indirect addresses including indirect block
int writei(struct inode *ip, char *src, uint off, uint n)
{
  uint tot, m;
  uint blockno, toff, i, x;
  uint iaddr;
  int update_meta = 0, migrate_indirect = 0;
  char *tsrc;
  struct buf *bp;
  uchar buf[BSIZE], temp_buf[BSIZE], indirect_buf[BSIZE];

  if (ip->type == T_DEV)
  {
    if (ip->major < 0 || ip->major >= NDEV || !devsw[ip->major].write)
      return -1;
    return devsw[ip->major].write(ip, src, n);
  }

  if (off > ip->size || off + n < off)
    return -1;
  if (off + n > MAXFILE * BSIZE)
    return -1;

  if (ip->type != T_DIR)
  {
    // find all blocks which this function targets for
    for (tot = 0, toff = off, tsrc = src; tot < n; tot += m, toff += m, tsrc += m)
    {
      m = min(n - tot, BSIZE - off % BSIZE);
      iaddr = toff / BSIZE;
      blockno = bmmap(ip, iaddr);
      i = blockno / 8;
      x = 1 << (blockno % 8);
      if (blockno == 0)
      {
        if (iaddr >= NDIRECT)
        {
          if ((smeta.smap[i] & x) != 0)
          {
            smeta.smap[i] &= ~x;
          }
        }
        continue;
      }

      if ((smeta.smap[i] & x) != 0)
      {
        // if that data block in indirect block, then just skip for now
        if (iaddr >= NDIRECT)
        {
          migrate_indirect = 1;
          smeta.smap[i] &= ~x;
          continue;
        }
        // cow
        smeta.smap[i] &= ~x;
        update_meta = 1;

        bp = bread(ip->dev, blockno);  // read block
        memmove(buf, bp->data, BSIZE); // store entire block data to a buffer
        brelse(bp);

        ip->addrs[iaddr] = 0;                 // set addr to zero
        bp = bread(ip->dev, bmap(ip, iaddr)); // this allocates new data block
        memmove(bp->data, buf, BSIZE);        // copy block data
        log_write(bp);
        brelse(bp);
      }
    }
    // migrate indirect block entirely
    if (migrate_indirect)
    {
      bp = bread(ip->dev, ip->addrs[NDIRECT]);
      memmove(buf, bp->data, BSIZE);
      memmove(indirect_buf, bp->data, BSIZE);
      brelse(bp);
      uint *a = (uint *)indirect_buf;
      uint *b = (uint *)buf;
      // loop all indirect block
      for (uint i = 0; i < BSIZE / sizeof(uint); i++)
      {
        // allocate new data block for that indirect block
        if (a[i] != 0)
        {
          bp = bread(bp->dev, a[i]);          // read block
          memmove(temp_buf, bp->data, BSIZE); // store block data to buffer
          brelse(bp);
          b[i] = balloc(bp->dev);             // allocate new block
          bp = bread(bp->dev, b[i]);          // read new block
          memmove(bp->data, temp_buf, BSIZE); // copy block data
          log_write(bp);
          brelse(bp);
        }
      }

      // allocate new block
      ip->addrs[NDIRECT] = balloc(ip->dev);

      // after migration finished, then save new indirect block
      bp = bread(ip->dev, ip->addrs[NDIRECT]); // read again
      memmove(bp->data, buf, BSIZE);           // store new indirect addresses
      log_write(bp);
      brelse(bp);
    }
  }

  for (tot = 0; tot < n; tot += m, off += m, src += m)
  {
    bp = bread(ip->dev, bmap(ip, off / BSIZE));
    m = min(n - tot, BSIZE - off % BSIZE);
    memmove(bp->data + off % BSIZE, src, m);
    log_write(bp);
    brelse(bp);
  }

  if (n > 0 && off > ip->size)
  {
    ip->size = off;
    iupdate(ip);
  }

  if (update_meta)
    update_snapshot_meta();
  return n;
}

// PAGEBREAK!
//  Directories

int namecmp(const char *s, const char *t)
{
  return strncmp(s, t, DIRSIZ);
}

// Look for a directory entry in a directory.
// If found, set *poff to byte offset of entry.
struct inode *
dirlookup(struct inode *dp, char *name, uint *poff)
{
  uint off, inum;
  struct dirent de;

  if (dp->type != T_DIR)
    panic("dirlookup not DIR");

  for (off = 0; off < dp->size; off += sizeof(de))
  {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlookup read");
    if (de.inum == 0)
      continue;
    if (namecmp(name, de.name) == 0)
    {
      // entry matches path element
      if (poff)
        *poff = off;
      inum = de.inum;
      return iget(dp->dev, inum);
    }
  }

  return 0;
}

// iterate dir entry (similar to C)
static int dirnext(struct inode *ip, int (*filter)(struct dirent *), struct dirent *de, uint *boff)
{
  uint off = 0;

  ilock(ip);
  if (ip->type != T_DIR)
    panic("dirnext not DIR");
  off = *boff;

  if (!(off < ip->size))
  {
    iunlock(ip);
    return -1;
  }

  off += sizeof(struct dirent);

  // read dirent directly
  if (readi(ip, (char *)de, off, sizeof(struct dirent)) != sizeof(struct dirent))
  {
    iunlock(ip);
    return -1;
  }

  *boff = off;
  if (de->inum == 0)
  {
    iunlock(ip);
    return -1;
  }

  // check filter
  if (filter && !filter(de))
  {
    iunlock(ip);
    return 0;
  }

  iunlock(ip);
  return 1;
}

// Write a new directory entry (name, inum) into the directory dp.
int dirlink(struct inode *dp, char *name, uint inum)
{
  int off;
  struct dirent de;
  struct inode *ip;

  // Check that name is not present.
  if ((ip = dirlookup(dp, name, 0)) != 0)
  {
    iput(ip);
    return -1;
  }

  // Look for an empty dirent.
  for (off = 0; off < dp->size; off += sizeof(de))
  {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
      panic("dirlink read");
    if (de.inum == 0)
      break;
  }

  strncpy(de.name, name, DIRSIZ);
  de.inum = inum;
  if (writei(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
    panic("dirlink");

  return 0;
}

// PAGEBREAK!
//  Paths

// Copy the next path element from path into name.
// Return a pointer to the element following the copied one.
// The returned path has no leading slashes,
// so the caller can check *path=='\0' to see if the name is the last one.
// If no name to remove, return 0.
//
// Examples:
//   skipelem("a/bb/c", name) = "bb/c", setting name = "a"
//   skipelem("///a//bb", name) = "bb", setting name = "a"
//   skipelem("a", name) = "", setting name = "a"
//   skipelem("", name) = skipelem("////", name) = 0
//
static char *
skipelem(char *path, char *name)
{
  char *s;
  int len;

  while (*path == '/')
    path++;
  if (*path == 0)
    return 0;
  s = path;
  while (*path != '/' && *path != 0)
    path++;
  len = path - s;
  if (len >= DIRSIZ)
    memmove(name, s, DIRSIZ);
  else
  {
    memmove(name, s, len);
    name[len] = 0;
  }
  while (*path == '/')
    path++;
  return path;
}

// Look up and return the inode for a path name.
// If parent != 0, return the inode for the parent and copy the final
// path element into name, which must have room for DIRSIZ bytes.
// Must be called inside a transaction since it calls iput().
int is_snapshot_descendant(struct inode *ip)
{
  struct inode *snap_dir = 0;
  struct inode *root_dir = 0;
  struct inode *curr_inode = 0;
  struct inode *parent_inode = 0;
  int is_snap = 0;
  int unlock = 0;

  if (ip->inum == ROOTINO)
    return 0;

  root_dir = iget(ROOTDEV, ROOTINO);
  if (root_dir == 0)
  {
    return 0; // Should not happen
  }

  snap_dir = get_snapshot_root(root_dir);

  if (snap_dir == 0)
  {
    return 0; // /snapshot does not exist.
  }

  // If the inode is the snapshot directory itself.
  if (ip->inum == snap_dir->inum)
  {
    return 1;
  }

  curr_inode = ip;
  while (1)
  {
    if (curr_inode->inum == snap_dir->inum)
    {
      is_snap = 1;
      unlock = 1;
      break;
    }
    if (curr_inode->inum == ROOTINO)
    {
      is_snap = 0;
      unlock = 1;
      break;
    }
    parent_inode = dirlookup(curr_inode, "..", 0);

    if (curr_inode->inum != ip->inum)
      iunlockput(curr_inode);
    curr_inode = parent_inode;
    if (curr_inode == 0)
    {
      is_snap = 0;
      break;
    }
    ilock(curr_inode);
  }

  if (curr_inode && curr_inode->inum != ip->inum && unlock)
    iunlockput(curr_inode);
  iput(snap_dir);

  return is_snap;
}

static struct inode *
namex(char *path, int nameiparent, char *name)
{
  struct inode *ip, *next;

  if (*path == '/')
    ip = iget(ROOTDEV, ROOTINO);
  else
    ip = idup(myproc()->cwd);

  while ((path = skipelem(path, name)) != 0)
  {
    ilock(ip);
    if (ip->type != T_DIR)
    {
      iunlockput(ip);
      return 0;
    }
    if (nameiparent && *path == '\0')
    {
      // Stop one level early.
      iunlock(ip);
      return ip;
    }
    if ((next = dirlookup(ip, name, 0)) == 0)
    {
      iunlockput(ip);
      return 0;
    }
    iunlockput(ip);
    ip = next;
  }
  if (nameiparent)
  {
    iput(ip);
    return 0;
  }
  return ip;
}

struct inode *
namei(char *path)
{
  char name[DIRSIZ];
  return namex(path, 0, name);
}

struct inode *
nameiparent(char *path, char *name)
{
  return namex(path, 1, name);
}

// create a copy of file captured in snapshot directory to target directory
// if that file is directory, it does not copy but create new one
// it also only creates new inode not data block, pointing same block since data blocks in snapshot marked in smap, so if any changes to that file will occur cow operation in writei
static struct inode *irestore(struct inode *dp, struct inode *snapshot_ip, const char *name)
{
  struct inode *new_ip;
  begin_op();
  if ((new_ip = ialloc(snapshot_ip->dev, snapshot_ip->type)) == 0)
  {
    end_op();
    return 0;
  }
  end_op();
  ilock(new_ip);

  // copy inode metadata
  new_ip->major = snapshot_ip->major;
  new_ip->minor = snapshot_ip->minor;
  new_ip->nlink = 1;
  new_ip->size = snapshot_ip->size;

  // if directory, create new one, not copying block address
  if (new_ip->type == T_DIR)
  {
    new_ip->nlink++;
    iupdate(new_ip);

    if (dirlink(new_ip, ".", new_ip->inum) < 0 ||
        dirlink(new_ip, "..", dp->inum) < 0)
    {
      iunlockput(new_ip);
      end_op();
      return 0;
    }
  }
  else if (new_ip->type != T_DEV)
  {
    ilock(snapshot_ip);
    // Share data blocks instead of allocating and copying
    memmove(new_ip->addrs, snapshot_ip->addrs, sizeof(snapshot_ip->addrs));
    // and register to smap ensuring all these blocks are cow protected
    smapi(new_ip);

    // save updated meta file
    if (update_snapshot_meta() < 0)
    {
      iunlockput(new_ip);
      end_op();
      return 0;
    }
  }

  iunlock(snapshot_ip);
  begin_op();
  iupdate(new_ip);

  // link it to entry of directory
  if (dirlink(dp, (char *)name, new_ip->inum) < 0)
  {
    iunlockput(new_ip);
    end_op();
    return 0;
  }

  iunlock(new_ip);
  end_op();
  return new_ip;
}

// unlink file or directory from the parent directory, also deallocate inode
// while unlinking the inode, it tries to free blocks which belongs to the inode
// if the block is protected by smap, then it still remains to work properly
// it works same as "rm" command
// almost copied from sysfile.c
static int dirunlink(struct inode *dp, char *name)
{
  uint off;
  struct dirent de;
  struct inode *ip;

  if ((ip = dirlookup(dp, name, &off)) == 0)
    return -1;

  ilock(ip);

  if (ip->type == T_DIR && !isdirempty(ip))
  {
    iunlockput(ip);
    return -1;
  }

  memset(&de, 0, sizeof(de));
  if (writei(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
  {
    iunlockput(ip);
    return -1;
  }

  if (ip->type == T_DIR)
  {
    ilock(dp);
    dp->nlink--;
    iupdate(dp);
    iunlock(dp);
  }

  ip->nlink--;
  iupdate(ip);
  iunlockput(ip);

  return 0;
}

static int isdirempty(struct inode *dp)
{
  int off;
  struct dirent de;

  for (off = 2 * sizeof(de); off < dp->size; off += sizeof(de))
  {
    if (readi(dp, (char *)&de, off, sizeof(de)) != sizeof(de))
      panic("isdirempty: readi");
    if (de.inum != 0)
      return 0;
  }
  return 1;
}

// recursively loops all entries including file and directory and tries to restore file
// it works given steps:
// 1. for each entries:
// 2. it removes the file in the target directory
// 3. allocates new inode which will substitute the inode in step 2
// 4. restore the file calling irestore
// if the file is directory, it just creates the directory only if it does not exist with same name in the target directory
static int sub_snapshot_rollback(struct inode *snapshot_dp, struct inode *target_dp)
{
  struct inode *sp, *dp;
  int ret;
  uint off = 0;
  struct dirent de;

  off = 0;
  while ((ret = dirnext(snapshot_dp, filter_dots, &de, &off)) >= 0)
  {
    if (ret == 0)
      continue;

    if ((sp = iget(snapshot_dp->dev, de.inum)) == 0)
      continue;

    ilock(sp);

    if (sp->type == T_DIR)
    {
      // ignore snapshot directory and its children
      if (strncmp(de.name, "snapshot", DIRSIZ) == 0)
      {
        iunlockput(sp);
        continue;
      }

      iunlock(sp);

      ilock(target_dp);
      dp = dirlookup(target_dp, de.name, 0);
      iunlock(target_dp);

      // if directory does not exist, create new one
      if (dp == 0)
      {
        begin_op();
        dp = create(target_dp, de.name, T_DIR, 0, 0);
        end_op();

        if (dp)
        {
          ilock(dp);
          iunlock(dp);
          sub_snapshot_rollback(sp, dp);
          iput(dp);
        }
        iput(sp);
      }
      else
      {
        // recursive
        sub_snapshot_rollback(sp, dp);
        iput(dp);
        iput(sp);
      }
    }
    else if (sp->type != T_DEV)
    {
      iunlock(sp);

      ilock(target_dp);
      dp = dirlookup(target_dp, de.name, 0);
      iunlock(target_dp);

      if (dp)
      {
        iput(dp);
        begin_op();
        ilock(target_dp);
        dirunlink(target_dp, de.name);
        iunlock(target_dp);
        end_op();
      }
      // restore
      dp = irestore(target_dp, sp, de.name);
      if (dp)
        iput(dp);

      iput(sp);
    }
    else
    {
      iunlockput(sp);
    }
  }

  return 0;
}

// create a new inode as a copy of an existing one.
// the new inode is linked to the given directory.
struct inode *
icopy(struct inode *dp, struct inode *ip, const char *name)
{
  struct inode *np;
  int i;

  // allocate a new inode.
  if ((np = ialloc(ip->dev, ip->type)) == 0)
    panic("create: ialloc");

  ilock(np);

  // copy metadata from the original inode.
  np->major = ip->major;
  np->minor = ip->minor;
  np->nlink = 1;
  np->size = 0;

  if (np->type == T_DIR)
  {
    np->nlink++;
    iupdate(np);

    if (dirlink(np, ".", np->inum) < 0 || dirlink(np, "..", dp->inum) < 0)
    {
      iunlockput(np);
      panic("create dots");
    }
  }
  else if (np->type != T_DEV)
  {
    // for files, copy the data block addresses.
    // pointing to same address - cow
    np->size = ip->size;
    for (i = 0; i < NDIRECT + 1; i++)
    {
      np->addrs[i] = ip->addrs[i];
    }
    // mark bits to bitmap
    smapi(ip);
    iupdate(np);
  }
  else
  {
    iupdate(np);
  }

  // link the new inode to the destination directory.
  if (dirlink(dp, (char *)name, np->inum) < 0)
  {
    iunlockput(np);
    panic("create: dirlink");
  }

  iunlock(np);
  return np;
}

static int filter_dots(struct dirent *de)
{
  return strncmp(de->name, "..", DIRSIZ) && strncmp(de->name, ".", DIRSIZ);
}

// recursively create a snapshot of a directory.
static int
sub_snapshot_create(struct inode *dp, struct inode *tp)
{
  struct inode *p, *destp;
  int ret;
  struct dirent de;
  uint off = 0;

  while ((ret = dirnext(dp, filter_dots, &de, &off)) >= 0)
  {
    if (ret == 0)
      continue;

    if ((p = iget(dp->dev, de.inum)) == 0)
      continue;

    ilock(p);

    if (p->type == T_DIR)
    {
      // ignore snapshot
      if (strncmp(de.name, "snapshot", DIRSIZ) != 0)
      {
        iunlock(p);
        begin_op();
        // create a copy of the directory in the target.
        destp = icopy(tp, p, de.name);
        end_op();

        if (destp)
        {
          ilock(destp);
          // recurse into the subdirectory.
          sub_snapshot_create(p, destp);
          iunlockput(destp);
        }
        iput(p);
      }
      else
      {
        iunlockput(p);
      }
    }
    // ignore T_DEV
    else if (p->type != T_DEV)
    {
      iunlock(p);
      begin_op();
      // create a copy of the file in the target.
      // only inode!
      destp = icopy(tp, p, de.name);
      end_op();

      if (destp)
        iput(destp);
      iput(p);
    }
    else
    {
      iunlockput(p);
    }
  }

  return 0;
}

// recursively delete the contents of a directory.
static int
sub_snapshot_delete(struct inode *dp)
{
  struct inode *p;
  int ret;
  struct dirent de;
  uint off = 0;

  while ((ret = dirnext(dp, filter_dots, &de, &off)) >= 0)
  {
    if (ret == 0)
      continue;

    if ((p = iget(dp->dev, de.inum)) == 0)
      continue;

    ilock(p);

    if (p->type == T_DIR)
    {
      iunlock(p);
      // recurse into subdirectory before deleting it.
      // before deleting the directory, there must be no entries
      sub_snapshot_delete(p);

      begin_op();
      ilock(dp);
      // delete the directory entry.
      dirunlink(dp, de.name);
      iunlock(dp);
      end_op();
      iput(p);
    }
    // ignore T_DEV
    else if (p->type != T_DEV)
    {
      iunlock(p);
      begin_op();
      ilock(dp);
      // delete the file entry.
      dirunlink(dp, de.name);
      iunlock(dp);
      iput(p);
      end_op();
    }
    else
    {
      iunlockput(p);
    }
  }

  return 0;
}

static struct inode *
create(struct inode *dp, char *path, short type, short major, short minor)
{
  struct inode *ip;
  ilock(dp);

  if ((ip = dirlookup(dp, path, 0)) != 0)
  {
    iunlockput(dp);
    ilock(ip);
    if (type == T_FILE && ip->type == T_FILE)
      return ip;
    iunlockput(ip);
    return 0;
  }

  if ((ip = ialloc(dp->dev, type)) == 0)
    panic("create: ialloc");

  ilock(ip);
  ip->major = major;
  ip->minor = minor;
  ip->nlink = 1;
  iupdate(ip);

  if (type == T_DIR)
  {              // Create . and .. entries.
    dp->nlink++; // for ".."
    iupdate(dp);
    // No ip->nlink++ for ".": avoid cyclic ref count.
    if (dirlink(ip, ".", ip->inum) < 0 || dirlink(ip, "..", dp->inum) < 0)
      panic("create dots");
  }

  if (dirlink(dp, path, ip->inum) < 0)
    panic("create: dirlink");

  iunlock(dp);
  iunlock(ip);
  return ip;
}

// get snapshot directory
struct inode *get_snapshot_root(struct inode *ip)
{
  struct inode *rnode;

  ilock(ip);
  rnode = dirlookup(ip, "snapshot", 0);
  iunlock(ip);

  if (!rnode)
  {
    begin_op();
    rnode = create(ip, "snapshot", T_DIR, 0, 0);
    end_op();
  }
  return rnode;
}

// create snapshot folder with given id
struct inode *create_snapshot(struct inode *snapshot_root)
{
  char buf[DIRSIZ];
  char *ptr;
  struct inode *ip;

  ptr = itoa(snapshot_id, 16, buf);

  if (!ptr)
  {
    return 0;
  }

  ilock(snapshot_root);
  ip = dirlookup(snapshot_root, ptr, 0);
  iunlock(snapshot_root);

  if (ip != 0)
  {
    iput(ip);
    return 0;
  }

  begin_op();
  ip = create(snapshot_root, ptr, T_DIR, 0, 0);
  end_op();

  return ip;
}

// get snapshot root dir
struct inode *get_snapshot(struct inode *snapshot_root, int id)
{
  char buf[DIRSIZ];
  char *ptr;
  struct inode *ip;

  ptr = itoa(id, 16, buf);
  if (!ptr)
  {
    return 0;
  }

  ilock(snapshot_root);
  ip = dirlookup(snapshot_root, ptr, 0);
  iunlock(snapshot_root);

  if (ip == 0)
  {
    return 0;
  }
  return ip;
}

// get inode of snapshot meta file
struct inode *get_snapshot_info(struct inode *root)
{
  struct inode *ip;
  ilock(root);
  if ((ip = dirlookup(root, "smap", 0)) == 0)
  {
    iunlock(root);
    begin_op();
    ip = create(root, "smap", T_FILE, 0, 0);
    end_op();
    if (!ip)
    {
      panic("read_snapshot_id: file creation failed");
    }
  }
  else
  {
    iunlock(root);
  }

  return ip;
}

// read snapshot info (smap and id)
struct inode *read_snapshot_info(struct inode *root)
{
  struct inode *ip;
  ilock(root);
  if ((ip = dirlookup(root, "smap", 0)) == 0)
  {
    iunlock(root);
    begin_op();
    ip = create(root, "smap", T_FILE, 0, 0);
    end_op();
    if (!ip)
    {
      panic("read_snapshot_id: file creation failed");
    }
    acquire(&smap_lock);
    smeta.next_id = 1;
    memset(smeta.smap, 0, sizeof(smeta.smap));
    release(&smap_lock);
  }
  else
  {
    iunlock(root);
    ilock(ip);
    if (readi(ip, (char *)&smeta, 0, sizeof(smeta)) != sizeof(smeta))
    {
      smeta.next_id = 1;
      memset(smeta.smap, 0, sizeof(smeta.smap));
    }
    iunlock(ip);
  }

  return ip;
}

// save snapshot meta to file
int update_snapshot_info(struct inode *ip)
{
  ilock(ip);
  begin_op();
  if (writei(ip, (char *)&smeta, 0, sizeof(smeta)) != sizeof(smeta))
  {
    end_op();
    iunlock(ip);
    return -1;
  }

  end_op();
  iunlock(ip);
  return 0;
}

// update smap to file
int update_snapshot_meta()
{
  struct inode *fs_root;
  struct inode *snapshot_root;
  struct inode *snapshot_info;

  if ((fs_root = iget(ROOTDEV, ROOTINO)) == 0)
    return -1;

  if ((snapshot_root = get_snapshot_root(fs_root)) == 0)
  {
    return -1;
  }

  if ((snapshot_info = get_snapshot_info(snapshot_root)) == 0)
  {
    return -1;
  }

  return update_snapshot_info(snapshot_info);
}

struct inode *setup_snapshot()
{
  struct inode *fs_root;
  struct inode *snapshot_root;
  struct inode *id_root;
  struct inode *snapshot_info;

  if ((fs_root = iget(ROOTDEV, ROOTINO)) == 0)
    return 0;

  if ((snapshot_root = get_snapshot_root(fs_root)) == 0)
  {
    iput(fs_root);
    return 0;
  }

  // read snapshot info (smap and next id)
  if ((snapshot_info = read_snapshot_info(snapshot_root)) == 0)
  {
    iput(snapshot_root);
    iput(fs_root);
    return 0;
  }
  acquire(&smap_lock);
  snapshot_id = smeta.next_id;
  smeta.next_id++;
  release(&smap_lock);

  if ((id_root = create_snapshot(snapshot_root)) == 0)
  {
    iput(snapshot_info);
    iput(snapshot_root);
    iput(fs_root);
    return 0;
  }

  // save
  if (update_snapshot_info(snapshot_info) < 0)
  {
    iput(id_root);
    iput(snapshot_info);
    iput(snapshot_root);
    iput(fs_root);
    panic("setup_snapshot: info update failed");
  }

  iput(snapshot_info);
  iput(snapshot_root);
  iput(fs_root);

  return id_root;
}

// create snapshot
int s_snapshot_create(void)
{
  struct inode *s_root;
  struct inode *ip;
  int curinodes;
  int reqinodes;
  int temp;
  int total_inodes = sb.ninodes;

  if ((ip = iget(ROOTDEV, ROOTINO)) == 0)
    return -1;

  curinodes = s_isize();
  ilock(ip);
  reqinodes = icount(ip);
  iunlock(ip);

  temp = micount();

  if (temp > curinodes)
  {
    curinodes = temp;
  }

  // compare required inode count and current available inode size
  // prevent no inode panic
  if (curinodes + reqinodes + 1 > total_inodes)
  {
    iput(ip);
    return -2;
  }

  if ((ip = iget(ROOTDEV, ROOTINO)) == 0)
    return -1;

  if ((s_root = setup_snapshot()) == 0)
  {
    iput(ip);
    return -1;
  }

  ilock(s_root);
  sub_snapshot_create(ip, s_root);
  iunlockput(s_root);
  iput(ip);

  return snapshot_id;
}

int s_snapshot_rollback(int id)
{
  struct inode *s_root;
  struct inode *ip;
  struct inode *snapshot;
  int temp;
  int result;
  int curinodes;
  int inodes_to_add;
  int inodes_to_delete;
  int reqinodes;
  int total_inodes;
  if ((ip = iget(ROOTDEV, ROOTINO)) == 0)
    return -1;

  if ((s_root = get_snapshot_root(ip)) == 0)
  {
    iput(ip);
    return -1;
  }

  if ((snapshot = get_snapshot(s_root, id)) == 0)
  {
    iput(s_root);
    iput(ip);
    return -1;
  }

  curinodes = s_isize();
  ilock(snapshot);
  inodes_to_add = icount(snapshot);
  iunlock(snapshot);
  ilock(ip);
  inodes_to_delete = icount(ip);
  iunlock(ip);
  total_inodes = sb.ninodes;
  temp = micount();
  if (temp > curinodes)
  {
    curinodes = temp;
  }

  reqinodes = inodes_to_add - inodes_to_delete;

  // compare required inode count and current available inode size
  // prevent no inode panic
  if (curinodes + reqinodes > total_inodes)
  {
    iput(snapshot);
    iput(s_root);
    iput(ip);
    return -2;
  }

  result = sub_snapshot_rollback(snapshot, ip);

  iput(snapshot);
  iput(s_root);
  iput(ip);
  return result;
}

// snapshot delete
int s_snapshot_delete(int id)
{
  struct inode *s_root;
  struct inode *snapshot_dp;
  struct inode *root_dp;
  char buf[DIRSIZ];
  char *ptr;

  // get root
  if ((root_dp = iget(ROOTDEV, ROOTINO)) == 0)
    return -1;

  if ((s_root = get_snapshot_root(root_dp)) == 0)
  {
    iput(root_dp);
    return -1;
  }

  if ((snapshot_dp = get_snapshot(s_root, id)) == 0)
  {
    iput(s_root);
    iput(root_dp);
    return -1;
  }

  // delete inner entries
  sub_snapshot_delete(snapshot_dp);
  ilock(snapshot_dp);
  begin_op();

  // trunc inode
  itrunc(snapshot_dp);
  snapshot_dp->type = 0;
  snapshot_dp->nlink = 0;
  iupdate(snapshot_dp);

  end_op();
  iunlock(snapshot_dp);

  ptr = itoa(id, 16, buf);

  // delete snapshot folder
  if (ptr)
  {
    begin_op();
    ilock(s_root);
    dirunlink(s_root, ptr);
    iunlock(s_root);
    end_op();
  }

  iput(snapshot_dp);
  iput(s_root);
  iput(root_dp);

  return 0;
}

// when xv6 boots, read snapshot meta file
void sminit(void)
{
  struct inode *fs_root;
  struct inode *snapshot_root;
  struct inode *snapshot_info_ip;

  initlock(&smap_lock, "smeta");

  if ((fs_root = iget(ROOTDEV, ROOTINO)) == 0)
  {
    panic("sminit: no root inode");
  }

  if ((snapshot_root = get_snapshot_root(fs_root)) == 0)
  {
    iput(fs_root);
    panic("sminit: no snapshot root");
  }

  if ((snapshot_info_ip = read_snapshot_info(snapshot_root)) == 0)
  {
    iput(snapshot_root);
    iput(fs_root);
    panic("sminit: no snapshot info");
  }

  iput(snapshot_info_ip);
  iput(snapshot_root);
  iput(fs_root);
}
