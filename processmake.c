#include "types.h"
#include "user.h"

#define PROCS_NUM 5

int main()
{
    printf(1, "started\n");
    for (int i = 0; i < PROCS_NUM; ++i)
    {
        //printf(1, "one\n");
        int pid = fork();
        if (pid == 0)
        {
            
            //sleep(5000);
            for (int m = 0; m < 100000000000; ++m){
                for (int j = 0; j < 100000000000; ++j)
                {
                    int x = 1;
                    for (long k = 0; k < 10000000000000; ++k)
                        x++;
                }
            }
            exit();
        }
    }
    printf(1, "end making the processes\n");
    for (int i = 0; i < PROCS_NUM; i++)
        wait();
    exit();
}