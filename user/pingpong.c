#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main() {
    int p1[2], p2[2];
    pipe(p1); // parent->child
    pipe(p2); // child->parent

    if (fork() == 0) {
        // child
        close(p1[1]);
        close(p2[0]);
        
        char buf[1];
        if (read(p1[0], buf, 1) != 1) {
            printf("child: read error\n");
            exit(1);
        }
        printf("%d: received ping\n", getpid());
        close(p1[0]);
        write(p2[1], "1", 1);
    }
    else {
        // parent
        close(p1[0]);
        close(p2[1]);
        write(p1[1], "1", 1);
        // wait(0);
        char buf[1];
        if (read(p2[0], buf, 1) != 1) {
            printf("parent: read error\n");
            exit(1);
        }
        printf("%d: received pong\n", getpid());
        close(p2[0]);
        exit(0);
    }
    exit(0);
}