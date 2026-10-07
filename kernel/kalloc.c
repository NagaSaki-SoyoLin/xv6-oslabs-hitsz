// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct kmem{
  struct spinlock lock;
  struct run *freelist;
} kmems[NCPU]; // 由原来所有CPU共享全局freelist, 改为每个CPU有自己的freelist

void
kinit()
{
  for (int i = 0; i < NCPU; i++) {
    // 初始化每个CPU的kmem.lock锁
    static char lock_names[NCPU][16]; // 使用静态变量防止乱码
    snprintf(lock_names[i], sizeof(lock_names[i]), "kmem%d", i); // 新增锁名以 kmem 开头
    initlock(&kmems[i].lock, lock_names[i]);
    // 初始化每个CPU的freelist
    int freelist_size = (PHYSTOP - (uint64)end) / NCPU; // 计算每个CPU的freelist的最大内存
    if (i < NCPU - 1)
      freerange(end + i * freelist_size, end + (i + 1) * freelist_size);
    else
      freerange(end + i * freelist_size, (char*)PHYSTOP); // 防止漏页
  }
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;

  int freelist_size = (PHYSTOP - (uint64)end) / NCPU; // 计算每个CPU的freelist的最大内存
  int cpu_id = ((uint64)pa - (uint64)end) / freelist_size; // 计算内存页对应的CPU号
  cpu_id = cpu_id >= NCPU ? NCPU - 1 : cpu_id; // 防止越界
  struct kmem *kmem = &kmems[cpu_id]; // 获取对应CPU的kmem

  // 将内存页加入对应CPU的freelist
  acquire(&kmem->lock);
  r->next = kmem->freelist;
  kmem->freelist = r;
  release(&kmem->lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off(); // 禁止中断，避免死锁
  int cpu_id = cpuid(); // 获取当前CPU号
  struct kmem *kmem = &kmems[cpu_id]; // 获取当前CPU的kmem
  pop_off(); // 允许中断

  acquire(&kmem->lock);
  r = kmem->freelist;
  if(r) {
    kmem->freelist = r->next;
    release(&kmem->lock);
  }
  else {
    // 当前CPU的 freelist 为空，尝试从其他CPU的 freelist 中获取内存页
    release(&kmem->lock); // 释放锁，避免死锁
    for (int i = 0; i < NCPU; i++) {
      if (i == cpu_id)
        continue;
      struct kmem *other_kmem = &kmems[i];
      acquire(&other_kmem->lock);
      r = other_kmem->freelist;
      if (r) {
        other_kmem->freelist = r->next;
        release(&other_kmem->lock); // 释放锁，避免死锁
        break; // 找到一个可用的内存页，跳出循环
      }
      release(&other_kmem->lock);
    }
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
