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

struct kmem {
  struct spinlock lock;
  struct run *freelist;
};

void
kinit()
{
  struct kmem kmem;
  initlock(&kmem.lock, "kmem");
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

struct {
  struct spinlock lock;
  struct run *freelist;
  uint refcount[(PHYSTOP - KERNBASE) / PGSIZE]; 
} kmem;


// 增加物理页的引用计数
void 
krefinc(void *pa) 
{
  if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("krefinc: invalid pa");

  acquire(&kmem.lock);
  uint idx = ((uint64)pa - KERNBASE) / PGSIZE;
  kmem.refcount[idx]++;
  release(&kmem.lock);
}

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)

// Free a page of physical memory, managing reference counts
void 
kfree(void *pa)
{
  struct run *r;

  // 检查物理地址合法性
  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree: invalid pa");

  // 减少引用计数
  acquire(&kmem.lock);
  uint idx = ((uint64)pa - KERNBASE) / PGSIZE;
  if (kmem.refcount[idx] == 0)
    panic("kfree: refcount underflow");
  
  kmem.refcount[idx]--; // 引用计数减一
  
  // 如果引用计数仍大于0，不释放物理页
  if (kmem.refcount[idx] > 0) {
    release(&kmem.lock);
    return;
  }
  release(&kmem.lock);

  // 填充垃圾数据以检测悬垂指针
  memset(pa, 1, PGSIZE);

  // 将物理页归还空闲链表
  r = (struct run*)pa;
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
    if(r) {
      kmem.refcount[((uint64)r - KERNBASE) / PGSIZE] = 1; // 初始化引用计数
      memset((char*)r, 0, PGSIZE);
    }
    return (void*)r;
}
