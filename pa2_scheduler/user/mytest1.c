#include "../kernel/types.h"
#include "user.h"
#include "../kernel/stat.h"

int main ()
{

    int pid1 = fork();
    int a = 1;
    int range = 10000;


    if(pid1>0){

        setnice(getpid(),0);
        for(int i = 0; i< range; i++){
            for(int j = 0; j< range; j++){
            a = 9+9*a;
            }
        }
        ps(0);
        printf("ans: %d\n",a);

    }
    else if(pid1 == 0){
        setnice(getpid(),10);
        for(int i = 0; i< range; i++){
            for(int j = 0; j< range; j++){
            a = 9+9*a;
            }
        }
        ps(0);
        printf("ans: %d\n",a);
    }

    exit(0);
}