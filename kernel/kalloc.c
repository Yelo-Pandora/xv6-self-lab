#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

uint page_ref[(PHYSTOP - KERNBASE) / PGSIZE];

void freerange(void *pa_start, void *pa_end);
extern char end[];

struct run
{
  struct run *next;
};

struct
{
  struct spinlock lock;
  struct run *freelist;
} kmem;

struct spinlock ref_lock;

void kinit()
{
  initlock(&kmem.lock, "kmem");
  initlock(&ref_lock, "refcnt");
  freerange(end, (void *)PHYSTOP);
}

void freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char *)PGROUNDUP((uint64)pa_start);
  for (; p + PGSIZE <= (char *)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char *)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&ref_lock);
  if (page_ref[COW_INDEX(pa)] > 1)
  {
    // 还有别的进程在共享这一页,只减少引用计数
    page_ref[COW_INDEX(pa)]--;
    release(&ref_lock);
    return;
  }
  page_ref[COW_INDEX(pa)] = 0;
  release(&ref_lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run *)pa;

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if (r)
    kmem.freelist = r->next;
  release(&kmem.lock);

  if (r)
  {
    memset((char *)r, 5, PGSIZE); // fill with junk
    acquire(&ref_lock);
    page_ref[COW_INDEX(r)] = 1;
    release(&ref_lock);
  }
  return (void *)r;
}

// 增加物理页 pa 的引用计数
void krefinc(uint64 pa)
{
  acquire(&ref_lock);
  page_ref[COW_INDEX(pa)]++;
  release(&ref_lock);
}

// 返回物理页 pa 的引用计数
uint krefget(uint64 pa)
{
  uint n;

  acquire(&ref_lock);
  n = page_ref[COW_INDEX(pa)];
  release(&ref_lock);
  return n;
}

// 处理 va 处的 COW 写缺页:
//   - 若该页被共享(引用 > 1),分配新页、复制内容、以可写方式重新映射;
//   - 若只剩本进程一个引用,直接恢复写权限即可,不必复制。
// 成功返回 0,失败返回 -1。非 COW 页不做任何处理,返回 0。
int cow_alloc(pagetable_t pagetable, uint64 va)
{
  va = PGROUNDDOWN(va);
  if (va >= MAXVA)
    return -1;

  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0)
    return -1;

  uint64 pa = PTE2PA(*pte);
  if (pa == 0)
    return -1;

  uint64 flags = PTE_FLAGS(*pte);

  if (flags & PTE_COW)
  {
    uint64 mem;

    if (krefget(pa) == 1)
    {
      // 只剩最后一个引用:直接拿回写权限
      *pte = (PA2PTE(pa) | flags | PTE_W) & ~PTE_COW;
      sfence_vma();
      return 0;
    }

    mem = (uint64)kalloc();
    if (mem == 0)
      return -1;
    memmove((char *)mem, (char *)pa, PGSIZE);

    kfree((void *)pa); // 放弃对旧页的引用

    // 原地修改 PTE,免去 uvmunmap + mappages 的二次 walk
    *pte = (PA2PTE(mem) | flags | PTE_W) & ~PTE_COW;
    sfence_vma();
  }
  return 0;
}