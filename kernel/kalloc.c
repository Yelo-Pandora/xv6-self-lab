// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "proc.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

//struct {
//  struct spinlock lock;
//  struct run *freelist;
//} kmem;

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem[NCPU];

struct spinlock steal_lock;   // 用于跨 CPU 偷页时保护 freelist 的锁

void
kinit()
{
  // 为每个 CPU 的 freelist 初始化锁，名字以 "kmem" 开头
  char name[8];
  for (int i = 0; i < NCPU; ++i) {
    snprintf(name, sizeof(name), "kmem%d", i);
    initlock(&kmem[i].lock, name);
  }
  initlock(&steal_lock, "kmem_steal");
  freerange(end, (void*)PHYSTOP);
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

  r = (struct run*)pa;

  push_off();                   // 关中断
  int id = cpuid();
  pop_off();

  acquire(&kmem[id].lock);
  r->next = kmem[id].freelist;
  kmem[id].freelist = r;
  release(&kmem[id].lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int id = cpuid();
  pop_off();

  acquire(&kmem[id].lock);
  r = kmem[id].freelist;
  if(r)
    kmem[id].freelist = r->next;
  release(&kmem[id].lock);

  if(r == 0) {
    // 偷页：从其他 CPU 拿一半
    acquire(&steal_lock);
    for(int i = 0; i < NCPU; ++i) {
      if(i == id) continue;

      acquire(&kmem[i].lock);
      struct run *head = kmem[i].freelist;
      if(head == 0) {
        release(&kmem[i].lock);
        continue;
      }

      // 计算链表长度
      int cnt = 0;
      struct run *p = head;
      while(p) { cnt++; p = p->next; }

      // 取一半（向上取整）
      int half = (cnt + 1) / 2;
      struct run *prev = 0;
      p = head;
      for(int j = 0; j < half; ++j) { prev = p; p = p->next; }
      prev->next = 0;          // 断开
      kmem[i].freelist = p;    // 剩余留在原 CPU

      release(&kmem[i].lock);

      // 把偷来的链表头放到本 CPU
      acquire(&kmem[id].lock);
      r = head;
      // 如果偷来的不止一页，把剩余页挂到本 CPU
      if(r->next) {
        struct run *tail = r;
        while(tail->next) tail = tail->next;
        tail->next = kmem[id].freelist;
        kmem[id].freelist = r->next;
      }
      release(&kmem[id].lock);

      break;  // 只偷一次
    }
    release(&steal_lock);
  }

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}
