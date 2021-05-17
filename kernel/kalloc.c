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
  int count[PHYSTOP/PGSIZE];  // ref cnts
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  memset(kmem.count, 0, sizeof(kmem.count) / sizeof(int));
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

  acquire(&kmem.lock);
  if((--kmem.count[REFINDEX(pa)]) <= 0) {
    // Fill with junk to catch dangling refs.
    memset(pa, 1, PGSIZE);
    r = (struct run*)pa;
    r->next = kmem.freelist;
    kmem.freelist = r;
  }
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
    kmem.freelist = r->next;
    kmem.count[REFINDEX(r)] = 1;
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

void incref(void *pa) {
  acquire(&kmem.lock);
  kmem.count[REFINDEX(pa)] += 1;
  release(&kmem.lock);
}

int pgfault_handler(pagetable_t pagetable, uint64 va) {
  if(va >= MAXVA) return -1;

  pte_t *pte = walk(pagetable, va, 0);
  if(pte == 0) 
    return -1;
  if((*pte & PTE_V) == 0)
    return -1;
  if((*pte & PTE_U) == 0)
    return -1;

  // check if refcnt is 1. unset cow page if refcnt = 1, alloc a new page otherwise
  uint64 mem, pa;
  pa = (uint64)PTE2PA(*pte);
  acquire(&kmem.lock);
  if(kmem.count[REFINDEX(pa)] == 1) {
    *pte &= ~PTE_C;
    *pte |= PTE_W;
    release(&kmem.lock);
    return 0;
  }
  release(&kmem.lock);

  if((mem = (uint64)kalloc()) == 0) return -1;
  memmove((void *)mem, (void *)pa, PGSIZE); // copy contents
  kfree((void *)pa); // decrease reference, and free pa if necessary

  // modify mappings and unset cow page
  *pte = PA2PTE(mem) | PTE_FLAGS(*pte); 
  *pte &= ~PTE_C; // unset PTE_C
  *pte |= PTE_W;  // set PTE_W

  return 0;
}
