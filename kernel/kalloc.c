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

struct {
  struct spinlock lock;
  struct run *freelist;
  int refcount[(PHYSTOP - KERNBASE) / PGSIZE]; // 引用计数数组
} kmem;

void
kinit()
{
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

// Free the page of physical memory pointed at by v,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP){
    panic("kfree");
  }

  int idx = ((uint64)pa - KERNBASE) / PGSIZE;
  acquire(&kmem.lock);

  if ((--kmem.refcount[idx]) > 0) {
    release(&kmem.lock);
    return; // 引用未归零，不释放
  }
  release(&kmem.lock);

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

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
  if (r) {
    kmem.freelist = r->next;
    int idx = ((uint64)r - KERNBASE) / PGSIZE;
    kmem.refcount[idx] = 1; // 初始化为1
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void incref(void *pa) {
  if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("incref: invalid pa");

  int idx = ((uint64)pa - KERNBASE) / PGSIZE;

  // Check for valid index range
  if (idx < 0 || idx >= (PHYSTOP - KERNBASE) / PGSIZE)
    panic("incref: index out of range");

  acquire(&kmem.lock);

  // Check for invalid (negative) reference count before incrementing
  if (kmem.refcount[idx] < 0) {
    release(&kmem.lock);
    panic("incref: negative refcount detected, possible memory corruption");
  }

  // Check for overflow
  if (kmem.refcount[idx] == __INT_MAX__) {
    release(&kmem.lock);
    panic("incref: reference count overflow");
  }

  kmem.refcount[idx]++;
  //printf("incref: pa=%p, ref=%d\n", pa, kmem.refcount[idx]); // Debug output
  release(&kmem.lock);
}


void decref(void *pa) {
  if (((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("decref: invalid pa");

  int idx = ((uint64)pa - KERNBASE) / PGSIZE;

  // Check for valid index range
  if (idx < 0 || idx >= (PHYSTOP - KERNBASE) / PGSIZE)
    panic("decref: index out of range");

  acquire(&kmem.lock);

  // Detect potential memory corruption
  if (kmem.refcount[idx] <= 0) {
    release(&kmem.lock);
    panic("decref: reference count already zero or negative, memory corruption detected");
  }

  // Decrease the reference count
  kmem.refcount[idx]--;
  //printf("decref: pa=%p, ref=%d\n", pa, kmem.refcount[idx]); // Debug output

  // Free the page if the reference count drops to zero
  if (kmem.refcount[idx] == 0) {
    release(&kmem.lock);
    kfree(pa);
  } else {
    release(&kmem.lock);
  }
}


