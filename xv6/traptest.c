#include "types.h"
#include "user.h"
#include "stat.h"
#include "fcntl.h"

#define NUM_FORKS 3

void acuire_user() {

    while ((open("lockfile", O_CREATE  | O_WRONLY)) < 0) ;
}

void release_user() {

    unlink("lockfile");
}

int main(int argc, char* argv[]){
    int fd=open("file.txt",O_CREATE|O_WRONLY);
    for (int i = 0; i < NUM_FORKS; i++){
        int pid = fork();
        if (pid == 0){
            acuire_user();
           
            char* write_data = "OS Lab4 - G29";
            int max_length = 13;

            write(fd,write_data,max_length);
            write(fd,"\n",1);
           
            release_user();
            exit();
           
        }
    }
   
    while (wait() != -1);
    close(fd);

    getcount();
    exit();
}