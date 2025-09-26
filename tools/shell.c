/* tools/shell.c
 *
 * Simple helper to be run under a PTY. Behavior:
 * - If invoked with additional args, execvp(argv[1], &argv[1])
 * - Otherwise exec user's $SHELL -i (interactive) or /bin/sh -i
 *
 * This binary is optional; backend falls back to running shell directly if not found.
 */

#define _XOPEN_SOURCE 700
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc >= 2) {
        /* run given command (argv[1..]) */
        execvp(argv[1], &argv[1]);
        perror("execvp");
        return 127;
    }

    const char *shell = getenv("SHELL");
    if (!shell) shell = "/bin/sh";
    /* run interactive shell */
    char *const sh_argv[] = { (char*)shell, "-i", NULL };
    execvp(shell, sh_argv);
    /* if exec fails */
    perror("execvp");
    return 127;
}
