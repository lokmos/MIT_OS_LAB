#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void traversal(int p[2]) {
    close(p[1]);
    int prime;
    read(p[0], &prime, sizeof(prime));
    if (prime == -1)
        exit(0);
    printf("prime %d\n", prime);

    int pout[2];
    pipe(pout);

    if (fork() == 0) {
        close(p[0]);
        close(pout[1]);
        traversal(pout);
        exit(0);
    }
    else {
        close(pout[0]);
        int buf;
        while (read(p[0], &buf, sizeof(buf)) && buf != -1) {
            if (buf % prime != 0)
                write(pout[1], &buf, sizeof(buf));
        }
        buf = -1;
        write(pout[1], &buf, sizeof(buf));
        wait(0);
        exit(0);
    }
}

int main() {
    int p[2];
    pipe(p);
    
    if (fork() == 0) {
        close(p[1]);
        traversal(p);
        exit(0);
    }
    else {
        close(p[0]);
        for (int i = 2; i <= 35; i++)
            write(p[1], &i, sizeof(i));
        int buf = -1;
        write(p[1], &buf, sizeof(buf));
    }
    wait(0);
    exit(0);
}