#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#define READ  0
#define WRITE 1
#define stdin 0
#define stdout 1
#define stderr 2

int main(int argc, char *argv[]) {
    int pid, p2c[2], c2p[2];
    char buf = 0;
    pipe(p2c);
    pipe(c2p);

    if( (pid = fork()) < 0 ) {
        fprintf(stderr, "fork error...\n");
        exit(1);
    }
    else if( pid > 0 ) { // parent process
        close(p2c[READ]);
        close(c2p[WRITE]);
      
        write(p2c[WRITE], &buf, 1);
        close(p2c[WRITE]);
          
        read(c2p[READ], &buf, 1);
        close(c2p[READ]);
      
        printf("%d: received pong\n", getpid());
        exit(0);
    }
    else { // child process
        close(p2c[WRITE]); 
        close(c2p[READ]);

        read(p2c[READ], &buf, 1); 
        close(p2c[READ]); 
        printf("%d: received ping\n", getpid());

        write(c2p[WRITE], &buf, 1);
        close(c2p[WRITE]);
        exit(0);
    }
}
