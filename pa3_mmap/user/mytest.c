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
    printf("mmap test\n");
    printf("---------------------------\n\n");

    printf("(init) free memory page count: %d\n\n", freemem());
    
    int size = 8192;
    int fd = open("README", O_RDWR);
    int status;

    printf("<is anonymous and populate>\n");
    char *addr1 = (char *)mmap(0, size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_POPULATE, -1, 0);

    printf("addr: %p\n", addr1);
    printf("text[0] is %d\n\n", addr1[0]);
    addr1[0] = 100;
    printf("(after change) text[0] is %d\n", addr1[0]);

    printf("(1 assigned) free memory page count: %d\n", freemem());
    
    printf("is populate\n");

    char *addr2 = (char*)mmap(8192, size, PROT_READ | PROT_WRITE, MAP_POPULATE, fd, 1);

    printf("addr: %p\n", addr2);
    printf("text[0] is %d\n", addr2[0]);
    addr2[1] = 200;
    printf("(after change) text[1] is %d\n", addr2[1]);
    printf("(2 assigned) free memory page count: %d\n", freemem());

    printf("No populate without anonymous\n");
    char *addr3= (char *)mmap(16384, size, PROT_READ, 0, fd, 0);

    printf("addr: %p\n", (void*) addr3);
    printf("(before access) free memory page count: %d\n", freemem());
    printf("(3 assigned) free memory page count: %d\n", freemem());

    printf("No populate with anonymous\n");
    char *addr4= (char *)mmap(24576, size, PROT_READ, MAP_ANONYMOUS, -1, 0);

    printf("addr: %p\n", (void*) addr4);
    printf("(before access) free memory page count: %d\n", freemem());
    printf("text[0] is %d\n", addr4[0]);

    printf("(4 assigned) free memory page count: %d\n", freemem());
    
    printf("부모 프로세스 복제\n\n");
    int rc = fork();
    if (rc < 0) {
        printf("프로세스 복제 실패\n");
    } else if (rc == 0) {
        printf("자식프로세스 실행\n\n");

        printf("(after fork) free memory page count: %d\n", freemem());
        printf("addr: %p\n", addr1);
        printf("text[0] is %d\n", addr1[0]);
        printf("(case1) free memory page count: %d\n", freemem());

        printf("addr: %p\n", addr2);
        addr2[1] = 150;
        printf("text[0] is %d\n", addr2[1]);
        printf("(case2) free memory page count: %d\n", freemem());

        printf("addr: %p\n", addr3);
        printf("(case3) free memory page count: %d\n", freemem());
        printf("text[0] is %d\n", addr3[0]);
        printf("(case3) free memory page count: %d\n", freemem());

        printf("addr: %p\n", addr4);
        printf("text[0] is %d\n", addr4[0]);
        printf("(case4) free memory page count: %d\n", freemem());


        
        int j = munmap((uint64) addr2);
        if (j == -1) printf("no unmap");
        printf("free memory page count: %d\n", freemem());
    
        int k = munmap((uint64) addr3);
        if (k == -1) printf("no unmap");
        printf("free memory page count: %d\n", freemem());
    
        int l = munmap((uint64) addr4);
        if (l == -1) printf("no unmap");
        printf("free memory page count: %d\n", freemem());

    } else {
        int wc = wait(&status);
        printf("부모프로세스 실행\n");
        
        printf("(get out of fork) free memory page count: %d\n", freemem());

        printf("addr2[0] is %d\n", addr2[1]);
        int i = munmap((uint64) addr1);
        if (i == -1) printf("no unmap");
        printf("free memory page count: %d\n", freemem());
        
        int j = munmap((uint64) addr2);
        if (j == -1) printf("no unmap");
        printf("free memory page count: %d\n", freemem());
    
        int k = munmap((uint64) addr3);
        if (k == -1) printf("no unmap");
        printf("free memory page count: %d\n", freemem());
    
        int l = munmap((uint64) addr4);
        if (l == -1) printf("no unmap");
        printf("free memory page count: %d\n", freemem());

        printf("자식프로세스 종료 상태: %d\n", wc);
    }



    return 0;
}
