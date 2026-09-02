// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.
//
// This version splits the cache into NBUCKET hash buckets, each with
// its own lock, so that lookups of different blocks can proceed in
// parallel. The miss/eviction path is serialized by evlock, which is
// the only code that ever touches more than one bucket at a time.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct bucket {
  struct spinlock lock;
  // Linked list of buffers in this bucket.
  // head.next is most recently used, head.prev is least recently used.
  struct buf head;
};

struct {
  struct buf buf[NBUF];
  struct bucket bucket[NBUCKET];
  struct spinlock evlock;   // serializes the miss (eviction) path
} bcache;

static uint hash_v(uint key) {
  return key % NBUCKET;
}

void
binit(void)
{
  struct buf *b;
  int i;

  initlock(&bcache.evlock, "bcache.evict");
  for (i = 0; i < NBUCKET; ++i) {
    initlock(&bcache.bucket[i].lock, "bcache.bucket");
    bcache.bucket[i].head.prev = &bcache.bucket[i].head;
    bcache.bucket[i].head.next = &bcache.bucket[i].head;
  }

  // Put all buffers on bucket lists as free (refcnt == 0) buffers,
  // distributed round-robin.
  i = 0;
  for (b = bcache.buf; b < bcache.buf + NBUF; b++, i++) {
    struct bucket *bk = &bcache.bucket[i % NBUCKET];
    initsleeplock(&b->lock, "buffer");
    b->refcnt = 0;
    b->next = bk->head.next;
    b->prev = &bk->head;
    bk->head.next->prev = b;
    bk->head.next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;
  uint v = hash_v(blockno);
  struct bucket *bk = &bcache.bucket[v];

  // Fast path: is the block already cached? Only one bucket lock.
  acquire(&bk->lock);
  for (b = bk->head.next; b != &bk->head; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bk->lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bk->lock);

  // Miss. The eviction path is serialized so that only one CPU at a
  // time moves buffers between buckets (avoids circular lock waits
  // and duplicate cache entries for the same block).
  acquire(&bcache.evlock);

  // Re-check: another CPU may have cached this block while we had
  // no bucket lock held.
  acquire(&bk->lock);
  for (b = bk->head.next; b != &bk->head; b = b->next) {
    if (b->dev == dev && b->blockno == blockno) {
      b->refcnt++;
      release(&bk->lock);
      release(&bcache.evlock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  release(&bk->lock);

  // Recycle the least recently used free (refcnt == 0) buffer,
  // searching each bucket from its LRU end. This is the only code
  // that takes two bucket locks (old and new), and only one CPU at
  // a time can be here.
  for (int i = 0; i < NBUCKET; i++) {
    struct bucket *bp = &bcache.bucket[i];
    acquire(&bp->lock);
    for (b = bp->head.prev; b != &bp->head; b = b->prev) {
      if (b->refcnt == 0) {
        // Unlink from the old bucket's list.
        b->prev->next = b->next;
        b->next->prev = b->prev;
        release(&bp->lock);

        // Claim it for this block.
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;

        // Insert at the MRU end of the target bucket.
        acquire(&bk->lock);
        b->next = bk->head.next;
        b->prev = &bk->head;
        bk->head.next->prev = b;
        bk->head.next = b;
        release(&bk->lock);

        release(&bcache.evlock);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bp->lock);
  }

  release(&bcache.evlock);
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// A buffer with refcnt == 0 stays in its bucket's list: it is the
// cache of that block. Move it to the MRU end.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  uint v = hash_v(b->blockno);
  struct bucket *bk = &bcache.bucket[v];
  acquire(&bk->lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // Move to the MRU end of the bucket.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bk->head.next;
    b->prev = &bk->head;
    bk->head.next->prev = b;
    bk->head.next = b;
  }
  release(&bk->lock);
}

void
bpin(struct buf *b) {
  uint v = hash_v(b->blockno);
  struct bucket *bk = &bcache.bucket[v];
  acquire(&bk->lock);
  b->refcnt++;
  release(&bk->lock);
}

void
bunpin(struct buf *b) {
  uint v = hash_v(b->blockno);
  struct bucket *bk = &bcache.bucket[v];
  acquire(&bk->lock);
  b->refcnt--;
  release(&bk->lock);
}
