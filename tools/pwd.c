// simple standalone pwd (Deprecated)
// build: gcc -o tools/pwd tools/pwd.c

#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <string.h>

int main(int argc, char **argv) {
    char buf[PATH_MAX];

    // -P : print physical directory (resolve symlinks)
    if (argc > 1 && strcmp(argv[1], "-P") == 0) {
        if (realpath(".", buf) != NULL) {
            printf("%s\n", buf);
            return 0;
        } else {
            fprintf(stderr, "pwd: realpath: %s\n", strerror(errno));
            return 2;
        }
    }

    // prefer getcwd (gives the actual current directory path)
    if (getcwd(buf, sizeof(buf)) != NULL) {
        printf("%s\n", buf);
        return 0;
    }

    // fallback: some shells keep PWD in env; try that before failing
    char *env = getenv("PWD");
    if (env && *env) {
        printf("%s\n", env);
        return 0;
    }

    fprintf(stderr, "pwd: getcwd: %s\n", strerror(errno));
    return 1;
}
