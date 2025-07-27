#include "../kernel/types.h"
#include "../kernel/stat.h"
#include "user.h"
#include "../kernel/fcntl.h"
#include "../kernel/memlayout.h"
#include "../kernel/param.h"
#include "../kernel/spinlock.h"
#include "../kernel/sleeplock.h"
#include "../kernel/fs.h"
#include "../kernel/syscall.h"

#define PAGES 13000

int main() {
    char **buf = malloc(sizeof(char*) * PAGES);
    if (buf == 0) {
        printf("malloc for buf failed\n");
        exit(1);
    }
    int reads = 0, writes = 0;
    
    for (int i = 0; i < PAGES; i++) {
        buf[i] = malloc(8192);
        if (buf[i] == 0) {
            printf("malloc failed at %d\n", i);
            exit(1);
        }
        buf[i][0] = i;  // force page allocation
        
        if (i%1000 == 0){
            printf("Allocated %d pages\n", i);
        }
        if (i % 100 == 0){
          swapstat(&reads, &writes);
          printf("swap read: %d swap write: %d\n", reads, writes);
        }
    }



    printf("Allocated %d pages\n", PAGES);
    exit(0);
}