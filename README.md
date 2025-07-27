# skku_OS

## Project 2: Scheduling
- **Objective:** Implement the Earliest Eligible Virtual Deadline First (EEVDF) in the xv6 operating system and modify the `ps` system call to display scheduling information.
- **Grade:** 100/100
- **Key Features:**
  - Implement EEVDF to manage process scheduling based on priority.
  - Modify `ps` system call to output runtime, weight, vruntime, and other relevant scheduling information.

## Project 3: Virtual Memory
- **Objective:** Implement virtual memory support in the xv6 operating system.
- **Grade:** 72/100
- **Key Features:**
  - Implement `mmap()`, `munmap()`, and `freemem()` system calls.
  - Implement a page fault handler to manage memory accesses to mapped regions.

## Project 4: Page Replacement
- **Objective:** Implement page-level swapping and manage swappable pages using the LRU (Least Recently Used) list.
- **Grade:** ?/100
- **Key Features:**
  - Implement swap-in and swap-out operations to move pages between main memory and backing store.
  - Manage swappable pages with an LRU list.
  - Implement the clock algorithm for page replacement.
