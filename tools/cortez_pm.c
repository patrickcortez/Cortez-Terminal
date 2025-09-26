// tools/cortez_pm.c

// compile: gcc -O2 -Wall -o tools/cortez_pm tools/cortez_pm.c
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <time.h>
#include <fcntl.h>
#include <dirent.h>
#include <signal.h>
#include <stdarg.h>
#include <libgen.h>


static const char *JOBDIR = "/tmp/cortez_pm";

static void die(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (errno) fprintf(stderr, ": %s\n", strerror(errno));
    else fputc('\n', stderr);
    exit(1);
}

static void ensure_jobdir(void) {
    struct stat st;
    if (stat(JOBDIR, &st) != 0) {
        if (mkdir(JOBDIR, 0755) != 0 && errno != EEXIST) die("mkdir %s failed", JOBDIR);
    }
}

static char *make_jobid(void) {
    char *id = malloc(64);
    if (!id) return NULL;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    snprintf(id, 64, "job-%ld-%d", (long)ts.tv_sec, (int)getpid());
    return id;
}

static int write_file(const char *path, const char *data) {
    int fd = open(path, O_CREAT|O_TRUNC|O_WRONLY, 0644);
    if (fd < 0) return -1;
    ssize_t w = write(fd, data, strlen(data));
    close(fd);
    return (w < 0) ? -1 : 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage:\n");
    fprintf(stderr, "  %s start --img-path <img> --img-size-mb N --mem-mb M --swap-mb S -- <prog> [args...]\n", prog);
    fprintf(stderr, "  %s list\n", prog);
    fprintf(stderr, "  %s stop <jobid>\n", prog);
}

/* helper to probe /proc for a child of a parent pid (returns first child pid or 0 if none) */
static int find_child_pid_via_proc(pid_t parent_pid)
{
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "/proc/%d/task/%d/children", (int)parent_pid, (int)parent_pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return 0; }
    fclose(f);

    char *p = buf;
    while (*p && isspace((unsigned char)*p)) p++;
    if (!*p) return 0;
    long val = strtol(p, NULL, 10);
    if (val <= 0) return 0;
    return (int)val;
}

/* start: exec memmgr and capture its stdout/stderr lines to find "CHILD_PID <n>" */
static int cmd_start(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "start: missing args\n"); return 1; }

    char exe_link[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe_link, sizeof(exe_link)-1);
    char tools_dir[PATH_MAX] = {0};
    if (n > 0) {
        exe_link[n] = '\0';
        char *d = dirname(exe_link);
        strncpy(tools_dir, d, sizeof(tools_dir)-1);
    } else {
        strncpy(tools_dir, ".", sizeof(tools_dir)-1);
    }

    /* create pipes to capture memmgr stdout+stderr */
    int pipefd[2];
    if (pipe(pipefd) != 0) die("pipe failed");

    pid_t child = fork();
    if (child < 0) die("fork failed");
    if (child == 0) {
        /* child: redirect stdout+stderr -> pipe write and exec memmgr (argv[1..]) */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);

        char **newargv = calloc(argc, sizeof(char*));
        if (!newargv) _exit(127);
        for (int i = 1; i < argc; ++i) newargv[i-1] = argv[i];
        newargv[argc-1] = NULL;
        execv(newargv[0], newargv);
        _exit(127);
    }

    /* parent */
    close(pipefd[1]);
    FILE *rf = fdopen(pipefd[0], "r");
    if (!rf) { close(pipefd[0]); die("fdopen failed"); }

    /* job dir */
    ensure_jobdir();
    char *jobid = make_jobid();
    char jobdirpath[PATH_MAX];
    snprintf(jobdirpath, sizeof(jobdirpath), "%s/%s", JOBDIR, jobid);
    if (mkdir(jobdirpath, 0755) != 0) {
        free(jobid); fclose(rf); die("mkdir jobdir failed");
    }

    /* memmgr.pid */
    char mempidbuf[32];
    snprintf(mempidbuf, sizeof(mempidbuf), "%d\n", (int)child);
    char mpf[PATH_MAX]; snprintf(mpf, sizeof(mpf), "%s/memmgr.pid", jobdirpath);
    if (write_file(mpf, mempidbuf) != 0) {
        free(jobid); fclose(rf); die("write memmgr.pid failed");
    }

    /* read lines until CHILD_PID or timeout */
    char line[512];
    int childpid = -1;
    time_t start = time(NULL);
    while (fgets(line, sizeof(line), rf)) {
        size_t L = strlen(line);
        while (L && (line[L-1]=='\n' || line[L-1]=='\r')) { line[--L] = 0; }
        
        // tools/cortez_pm.c (inside cmd_start's while loop)

    char *pid_str_loc = strstr(line, "CHILD_PID ");
    if (pid_str_loc) {
    // Calculate the length of the program output before our message
    size_t prefix_len = pid_str_loc - line;
    
    // If there was program output, print it.
    if (prefix_len > 0) {
        // The %.*s format prints a specific number of characters from a string
        fprintf(stderr, "%.*s", (int)prefix_len, line);
    }

    // Now, process the PID as before and suppress the rest of the line
    int p = atoi(pid_str_loc + 10);
    if (p > 0) childpid = p;
    char cpf[PATH_MAX]; snprintf(cpf, sizeof(cpf), "%s/child.pid", jobdirpath);
    char buf[32]; snprintf(buf, sizeof(buf), "%d\n", p);
    write_file(cpf, buf);
    continue;
}

        fprintf(stderr, "%s\n", line);
        if (time(NULL) - start > 5) break;
    }
    /* ... */

    /* Fallback: if we didn't get CHILD_PID text, try /proc polling for a short period */
    if (childpid <= 0) {
        const int max_ms = 5000;
        const int step_ms = 100;
        int waited = 0;
        while (waited < max_ms) {
            int cp = find_child_pid_via_proc(child);
            if (cp > 0) {
                childpid = cp;
                char cpf[PATH_MAX]; snprintf(cpf, sizeof(cpf), "%s/child.pid", jobdirpath);
                char buf[32]; snprintf(buf, sizeof(buf), "%d\n", cp);
                write_file(cpf, buf);
                break;
            }
            usleep(step_ms * 1000);
            waited += step_ms;
        }
    }

    fclose(rf); /* <-- only once */

    printf("JOB_STARTED %s %d %d\n", jobid, (int)child, childpid);
    fflush(stdout);
    free(jobid);
    return 0;
}

static int cmd_list(void) {
    ensure_jobdir();
    DIR *d = opendir(JOBDIR);
    if (!d) { printf("No jobs\n"); return 0; }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue;
        char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/%s/memmgr.pid", JOBDIR, ent->d_name);
        char mp[64] = "";
        FILE *f = fopen(path, "r");
        if (f) { if (fgets(mp, sizeof(mp), f)) { /* strip */ size_t L=strlen(mp); while(L&&(mp[L-1]=='\n'||mp[L-1]=='\r')) mp[--L]=0; } fclose(f); }
        char cp[PATH_MAX]; snprintf(cp, sizeof(cp), "%s/%s/child.pid", JOBDIR, ent->d_name);
        char chp[64] = "";
        f = fopen(cp, "r");
        if (f) { if (fgets(chp, sizeof(chp), f)) { size_t L=strlen(chp); while(L&&(chp[L-1]=='\n'||chp[L-1]=='\r')) chp[--L]=0; } fclose(f); }
        printf("%s memmgr=%s child=%s\n", ent->d_name, mp[0]?mp:"-", chp[0]?chp:"-");
    }
    closedir(d);
    return 0;
}

static int cmd_stop(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "stop needs <jobid>\n"); return 2; }
    char jobid[128];
    strncpy(jobid, argv[1], sizeof(jobid)-1);
    char memf[PATH_MAX]; snprintf(memf, sizeof(memf), "%s/%s/memmgr.pid", JOBDIR, jobid);
    FILE *f = fopen(memf, "r");
    if (!f) { fprintf(stderr, "job not found\n"); return 2; }
    int pid=0; if (fscanf(f, "%d", &pid)!=1) { fclose(f); fprintf(stderr, "bad pid\n"); return 2; }
    fclose(f);
    if (kill(pid, SIGTERM) != 0) {
        perror("kill");
        return 3;
    }
    printf("STOPPED %s %d\n", jobid, pid);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { print_usage(argv[0]); return 1; }
    if (strcmp(argv[1], "start") == 0) {
        return cmd_start(argc-1, &argv[1]);
    } else if (strcmp(argv[1], "list") == 0) {
        return cmd_list();
    } else if (strcmp(argv[1], "stop") == 0) {
        return cmd_stop(argc-1, &argv[1]);
    } else {
        print_usage(argv[0]);
        return 1;
    }
}