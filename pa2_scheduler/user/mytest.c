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


int main() {
    int size = 8192;
    int fd = open("README", O_RDONLY);
    char *addr = (char *)mmap(0, size, PROT_READ, MAP_ANONYMOUS | MAP_POPULATE, fd, 0);

    printf(1, "addr: %p\n", addr);
    printf(1, "text[0] is %c\n", addr[0]);
    return 0;
}
