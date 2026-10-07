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


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"

struct {
  struct spinlock lock[NBUCKETS]; // 为每个哈希桶创建一个自旋锁
  struct buf buf[NBUF];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf hashbucket[NBUCKETS]; // 为每个哈希桶创建一个哨兵结点
} bcache;

void
binit(void)
{
  struct buf *b;

  // 初始化每个哈希桶的自旋锁
  for (int i = 0; i < NBUCKETS; i++) {
    static char lock_names[NBUCKETS][16];
    // 保证新增锁名以 bcache 开头
    snprintf(lock_names[i], sizeof(lock_names[i]), "bcache%d", i);
    initlock(&bcache.lock[i], lock_names[i]);
    // 初始化每个哈希桶的哨兵结点
    bcache.hashbucket[i].next = &bcache.hashbucket[i];
    bcache.hashbucket[i].prev = &bcache.hashbucket[i];
  }

  // Create linked list of buffers
  // 初始化所有缓冲区, 并将所有缓冲区插入到每个哈希桶的链表中
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    // 按下标均匀入桶
    int bucket = (b - bcache.buf) % NBUCKETS;
    b->next = bcache.hashbucket[bucket].next;
    b->prev = &bcache.hashbucket[bucket];
    initsleeplock(&b->lock, "buffer");
    bcache.hashbucket[bucket].next->prev = b;
    bcache.hashbucket[bucket].next = b;
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  int bucket = blockno % NBUCKETS; // 计算哈希桶编号

  acquire(&bcache.lock[bucket]);

  // Is the block already cached?
  for(b = bcache.hashbucket[bucket].next; b != &bcache.hashbucket[bucket]; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache.lock[bucket]);
      acquiresleep(&b->lock);
      return b;
    }
  }

  release(&bcache.lock[bucket]);

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for (int i = 0; i < NBUCKETS; i++) { // 遍历所有哈希桶
    acquire(&bcache.lock[i]);
    for(b = bcache.hashbucket[i].prev; b != &bcache.hashbucket[i]; b = b->prev){
      if(b->refcnt == 0) {
        // 从其他哈希桶中找到一个空闲的缓冲区并移除
        b->next->prev = b->prev;
        b->prev->next = b->next;
        release(&bcache.lock[i]);

        acquire(&bcache.lock[bucket]);
        // 在当前桶中重扫，看是否已被别人缓存
        for (struct buf *t = bcache.hashbucket[bucket].next;
          t != &bcache.hashbucket[bucket]; t = t->next) {
          if(t->dev == dev && t->blockno == blockno){
            // 竞争输了就复用已存在的 t 并把  退回原桶 i
            t->refcnt++;
            release(&bcache.lock[bucket]);

            acquire(&bcache.lock[i]);
            b->next = bcache.hashbucket[i].next;
            b->prev = &bcache.hashbucket[i];
            bcache.hashbucket[i].next->prev = b;
            bcache.hashbucket[i].next = b;
            release(&bcache.lock[i]);

            acquiresleep(&t->lock);
            return t;
          }
        }
        // 竞争赢了才给 b 赋值并插入当前桶
        b->dev = dev;
        b->blockno = blockno;
        b->valid = 0;
        b->refcnt = 1;
        b->next = bcache.hashbucket[bucket].next;
        b->prev = &bcache.hashbucket[bucket];
        bcache.hashbucket[bucket].next->prev = b;
        bcache.hashbucket[bucket].next = b;
        release(&bcache.lock[bucket]);
        acquiresleep(&b->lock);
        return b;
      }
    }
    release(&bcache.lock[i]);
  }
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
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);

  int bucket = b->blockno % NBUCKETS; // 计算哈希桶编号

  acquire(&bcache.lock[bucket]);
  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache.hashbucket[bucket].next;
    b->prev = &bcache.hashbucket[bucket];
    bcache.hashbucket[bucket].next->prev = b;
    bcache.hashbucket[bucket].next = b;
  }
  
  release(&bcache.lock[bucket]);
}

void
bpin(struct buf *b) {
  int bucket = b->blockno % NBUCKETS; // 计算哈希桶编号
  acquire(&bcache.lock[bucket]);
  b->refcnt++;
  release(&bcache.lock[bucket]);
}

void
bunpin(struct buf *b) {
  int bucket = b->blockno % NBUCKETS; // 计算哈希桶编号
  acquire(&bcache.lock[bucket]);
  b->refcnt--;
  release(&bcache.lock[bucket]);
}