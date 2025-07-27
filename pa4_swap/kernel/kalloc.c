// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "proc.h"

#define SWAP_SLOTS (SWAPMAX / (PGSIZE/BSIZE))

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmem;

// pa4: struct for page control
struct page pages[PHYSTOP/PGSIZE];
struct page *page_lru_head;
int num_free_pages;
int num_lru_pages;

struct spinlock lru_lock;

struct {
  struct spinlock lock;
  char *bitmap;
} swap;


void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);

  initlock(&swap.lock, "swap");
  swap.bitmap = kalloc();
  if (swap.bitmap == 0){
    printf("kinit: bitmap kalloc failed\n");
  }
  memset(swap.bitmap, 0, PGSIZE);

  initlock(&lru_lock, "lru_lock");
  num_lru_pages = 0;
  page_lru_head = 0;
  num_free_pages = SWAP_SLOTS;
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  int i = 0;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE){
    kfree(p);
    pages[i].pagetable = 0;
    pages[i].vaddr = 0;
    pages[i].prev = 0;
    pages[i].next = 0;
    i++;
  }
}

// Free the page of physical memory pointed at by pa,
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

  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
// pa4: kalloc function
void *
kalloc(void)
{
  struct run *r;

  for (;;){
    acquire(&kmem.lock);
    r = kmem.freelist;

    if(r){
      kmem.freelist = r->next;
      release(&kmem.lock);
      memset((char*)r, 5, PGSIZE);
      return (void*)r;
    }
    release(&kmem.lock);
    
    if(!swap_out()){
      printf("OOM error\n");
      return 0;
    }
  }
}

int
swap_in(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (!pte) {
    panic("swap_in: invalid pte");
  }
  
  if (pte && !(*pte & PTE_V)) { // swapped
    int blkno = *pte >> 10;
    char *physical_space = kalloc();
    if (physical_space == 0){
      printf("kalloc failed\n");
      return -1;
    }

    *pte = PA2PTE((uint64) physical_space) | (*pte & 0x3FF) | PTE_V;

    swapread((uint64) physical_space, blkno);
    num_free_pages++;
    delete_swap_idx(blkno);
    insert_LRU(pagetable, va);
    
    sfence_vma();
    return 0;
  }
  return -1;
}

int
swap_out(void){

  if (num_lru_pages == 0 || num_free_pages == 0){
    return 0;
  }
  
  acquire(&lru_lock);
  pte_t *pte = 0;
  uint64 va = 0;
  pagetable_t pagetable = 0;
  struct page *page_pointer = page_lru_head;
  
  while(1){
    va = (uint64) page_pointer->vaddr;
    pagetable = page_pointer->pagetable;
    pte = walk(pagetable, va, 0);

    if (pte && !(*pte & PTE_U)){
      page_pointer = page_pointer->next;
      release(&lru_lock);
      remove_LRU(pagetable, va);
      continue;
    }

    if (pte && (*pte & PTE_A)){ // PTE_A == 1
      *pte &= ~PTE_A;
      page_lru_head = page_pointer->next;
      page_pointer = page_pointer->next;
    } else { // PTE_A == 0
      // evict this page
      break;
    }
  } 
  release(&lru_lock);
  
  if (page_lru_head == 0) return 0;

  if (va == 0 || pagetable == 0){
    panic("swap_out: no page pointer contents");
  }

  if (!pte) {
    panic("swap_out: invalid pte");
  }

  remove_LRU(pagetable, va);

  int i = find_empty_block();

  if (i < 0){
    return 0; // no empty block found
  }

  uint64 pa = PTE2PA(*pte);

  if (pte && (*pte & PTE_V)){
    *pte = (*pte & 0x3FF) | (i << 10);
    *pte &= ~PTE_V;
  }
  swapwrite(pa, i);
  num_free_pages--;
  kfree((void *)pa);

  return 1;
}

int
find_empty_block(void){
  acquire(&swap.lock);
  int i = 0;
  while (i < SWAP_SLOTS && swap.bitmap[i] != 0){
    i++;
  }
  if (i == SWAP_SLOTS){
    release(&swap.lock);
    return -1;
  }
  swap.bitmap[i] = 1;
  release(&swap.lock);

  return i;
}

int
find_empty_page(void){
  for (int i = 0; i < PHYSTOP/PGSIZE; i++){
    if ((uint64) pages[i].vaddr == 0 && pages[i].pagetable == 0){
      return i;
    }
  }
  return -1;
}

int
find_page(pagetable_t pagetable, uint64 va){ // find page with va
  for (int i = 0; i < PHYSTOP/PGSIZE; i++){
    if (((uint64) pages[i].vaddr == va) && (pages[i].pagetable == pagetable)){
      return i;
    }
  }
  return -1;
}

void
insert_LRU(pagetable_t pagetable, uint64 va){

  acquire(&lru_lock);
  int i = find_empty_page();
  if (i < 0) return; //no empty page

  pages[i].pagetable = pagetable;
  pages[i].vaddr = (char *)va;
  
  if (num_lru_pages == 0){ // first insert
    page_lru_head = &pages[i];
    pages[i].next = &pages[i];
    pages[i].prev = &pages[i];

  } else{
    pages[i].next = page_lru_head;
    pages[i].prev = page_lru_head->prev;
    page_lru_head->prev->next = &pages[i];
    page_lru_head->prev = &pages[i];
    page_lru_head = &pages[i];
  }

  num_lru_pages++;
  release(&lru_lock);
}

void
remove_LRU(pagetable_t pagetable, uint64 va){
  acquire(&lru_lock);
  
  int i = find_page(pagetable, va);
  if (i < 0) {
    printf("remove_LRU failed\n");
    return; // no such page
  }

  struct page *temp;
  temp = &pages[i];
  
  if (temp->next == temp && temp->prev == temp){ // alone
    page_lru_head = 0;
  } else{
    temp->prev->next = temp->next;
    temp->next->prev = temp->prev;

    if (temp == page_lru_head){
      page_lru_head = temp->next;
    }
  }

  temp->next = 0;
  temp->prev = 0;
  temp->pagetable = 0;
  temp->vaddr = 0;

  num_lru_pages--;
  release(&lru_lock);  
}

void 
delete_swap_idx(int blkno){
  acquire(&swap.lock);
  swap.bitmap[blkno] = 0;
  release(&swap.lock);
}