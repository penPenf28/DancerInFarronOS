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

//Buffer Cache的具体实现。因为读写磁盘操作效率不高，
//根据时间与空间局部性原理，这里将最近经常访问的磁盘块缓存在内存中。

#include "../libs/types.h"
#include "../libs/param.h"
#include "../libs/spinlock.h"
#include "../libs/sleeplock.h"
#include "../libs/riscv.h"
#include "../libs/buf.h"
#include "../libs/sdcard.h"
#include "../libs/printf.h"
#include "../libs/disk.h"

//高速缓存块
//通过链表将所有buffer进行链接
//spinlock是互斥原语，保证原子操作
struct {
  struct spinlock lock;
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache;

//cache的初始化
void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // Create linked list of buffers
  bcache.head.prev = &bcache.head;
  bcache.head.next = &bcache.head;
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    b->refcnt = 0;
    b->sectorno = ~0;
    b->dev = ~0;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    initsleeplock(&b->lock, "buffer");
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  #ifdef DEBUG
  printf("binit\n");
  #endif
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
// 给定dev号和扇区号
// 遍历，如果扇区已分配则返回
// 没分配就分配一个buffer并返回
static struct buf*
bget(uint dev, uint sectorno)
{
  struct buf *b;

  acquire(&bcache.lock);

  // Is the block already cached?
  for(b = bcache.head.next; b != &bcache.head; b = b->next){
    if(b->dev == dev && b->sectorno == sectorno){
      b->refcnt++;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache.head.prev; b != &bcache.head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->sectorno = sectorno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache.lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
}

// Return a locked buf with the contents of the indicated block.
// 把dev中对应给定的扇区读到buffer中并返回
struct buf* 
bread(uint dev, uint sectorno) {
  struct buf *b;

  b = bget(dev, sectorno);
  if (!b->valid) {
    disk_read(b);
    b->valid = 1;
  }

  return b;
}

// Write b's contents to disk.  Must be locked.
void 
bwrite(struct buf *b) {
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  disk_write(b);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  acquire(&bcache.lock);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.head.next;
    b->prev = &bcache.head;
    bcache.head.next->prev = b;
    bcache.head.next = b;
  }
  
  release(&bcache.lock);
}

void
bpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt++;
  release(&bcache.lock);
}

void
bunpin(struct buf *b) {
  acquire(&bcache.lock);
  b->refcnt--;
  release(&bcache.lock);
}


