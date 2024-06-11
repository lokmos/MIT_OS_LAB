#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/param.h"
#include "kernel/fs.h"

void run(char *program, char **args) {
    if (fork() == 0) {
        exec(program, args);
        exit(0);
    }
    return;
}

int main(int argc, char *argv[]) {
    char buf[2048];
    char *p = buf, *last_p = buf;
    char *argsbuf[128];
    char **args = argsbuf;
    for (int i = 1; i < argc; ++i) {
        *args = argv[i];
        args++;
    }
    char **pa = args;
    while (read(0, p, 1) != 0) {
        if (*p == ' ' || *p == '\n') {
            if (*p == ' ') {
                *p = '\0';
                *(pa++) = last_p;
                last_p = p + 1;
            }
            else {
                *p = '\0';
                *(pa++) = last_p;
                last_p = p + 1;
                *pa = 0;
                run(argv[1], argsbuf);
                pa = args;
            }
        }
        p++;
    }
    if (pa != args) {
        *pa = 0;
        run(argv[1], argsbuf);
    }
    while (wait(0) != -1);
    exit(0);
}

