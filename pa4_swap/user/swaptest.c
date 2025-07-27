#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main () {
	int a = -1, b = -1;
    swapstat(&a, &b);
    printf("a: %d, b: %d\n", a, b);
}
