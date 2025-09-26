/*
 * cortez_memmgr.c
 *
 * Default behaviour:
 *   - uses ./data.img (same directory as the binary) unless --img-path is provided
 *   - ensures image exists and is at least --img-size-mb (default 1024)
 *   - attaches image via losetup, mkswap, swapon (uses sudo if not run as root)
 *   - creates a cgroup v2 subtree under /sys/fs/cgroup/cortez-<ts>-<pid>
 *   - sets memory.max and memory.swap.max (bytes)
 *   - runs the requested program inside that cgroup
 *   - on exit (or on SIGINT/SIGTERM) cleans up: swapoff, losetup -d, remove cgroup
 *
 * Notes:
 *   - preferred to run as root (sudo) so losetup/swapon/cgroup writes do not require interactive sudo calls.
 *   - this program assumes cgroup v2 is available at /sys/fs/cgroup.
 *
 * Usage:
 *   ./cortez_memmgr [--img-path /full/path/data.img] [--img-size-mb 1024]
 *                   [--mem-mb 512] [--swap-mb 1024] -- <program> [args...]
 *
 * Compile:
 *   gcc -O2 -Wall -o cortez_memmgr cortez_memmgr.c
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <signal.h>
#include <sys/statvfs.h>
#include <libgen.h>

static char g_loopdev[PATH_MAX];
static int  g_created_loop = 0;
static char g_cgdir[PATH_MAX];

static void die(const char *fmt, ...) __attribute__((noreturn));
static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (errno) fprintf(stderr, ": %s\n", strerror(errno));
    else fputc('\n', stderr);
    exit(1);
}

static int file_exists(const char *path) {
    struct stat st;
    return (stat(path, &st) == 0);
}

static off_t file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) == -1) return -1;
    return st.st_size;
}

static void ensure_image(const char *imgpath, size_t size_mb) {
    off_t wanted = (off_t)size_mb * 1024 * 1024;
    if (file_exists(imgpath)) {
        off_t sz = file_size(imgpath);
        if (sz < 0) die("stat failed on %s", imgpath);
        if (sz < wanted) {
            int fd = open(imgpath, O_WRONLY);
            if (fd < 0) die("open for enlarge failed %s", imgpath);
            if (ftruncate(fd, wanted) != 0) {
                close(fd);
                die("ftruncate failed when enlarging %s", imgpath);
            }
            close(fd);
        }
        return;
    }

    int fd = open(imgpath, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (fd < 0) die("creating image failed %s", imgpath);
    if (ftruncate(fd, wanted) != 0) {
        close(fd);
        die("ftruncate failed when creating image %s", imgpath);
    }
    close(fd);
}

static int run_cmd_capture(const char *cmd, char *out, size_t outlen) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return -1;
    if (out && outlen) {
        if (fgets(out, outlen, fp) == NULL) out[0] = '\0';
        else {
            size_t L = strlen(out);
            while (L > 0 && (out[L-1] == '\n' || out[L-1] == '\r')) { out[L-1] = '\0'; L--; }
        }
    }
    int rc = pclose(fp);
    return rc;
}

static int run_cmd(const char *cmd) {
    int rc = system(cmd);
    if (rc == -1) return -1;
    if (WIFEXITED(rc)) return WEXITSTATUS(rc);
    return -1;
}

static int need_sudo() {
    return (geteuid() != 0);
}

static void write_str_to_file_checked(const char *path, const char *val) {
    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) die("open for write failed: %s", path);
    ssize_t w = write(fd, val, strlen(val));
    if (w < 0) {
        close(fd);
        die("write to %s failed", path);
    }
    close(fd);
}

static void cleanup_swap_and_loop(void) {
    if (g_loopdev[0] == '\0') return;
    char cmd[PATH_MAX + 128];
    // best-effort swapoff (try as root or via sudo)
    snprintf(cmd, sizeof(cmd), "swapoff %s 2>/dev/null || sudo swapoff %s 2>/dev/null", g_loopdev, g_loopdev);
    run_cmd(cmd);
    if (g_created_loop) {
        snprintf(cmd, sizeof(cmd), "losetup -d %s 2>/dev/null || sudo losetup -d %s 2>/dev/null", g_loopdev, g_loopdev);
        run_cmd(cmd);
    }
    g_loopdev[0] = '\0';
    g_created_loop = 0;
}

static void cleanup_cgroup(void) {
    if (g_cgdir[0] == '\0') return;
    // rmdir best-effort
    rmdir(g_cgdir);
    g_cgdir[0] = '\0';
}

/* signal handler to ensure cleanup */
static void on_signal(int signo) {
    (void)signo;
    cleanup_swap_and_loop();
    cleanup_cgroup();
    _exit(128 + (signo & 0xff));
}

static char *get_exe_dir(char *out, size_t outlen) {
    char linkbuf[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", linkbuf, sizeof(linkbuf)-1);
    if (n <= 0) return NULL;
    linkbuf[n] = '\0';
    char *dir = dirname(linkbuf);
    if (!dir) return NULL;
    strncpy(out, dir, outlen-1);
    out[outlen-1] = '\0';
    return out;
}

static char *get_now_unit_name(char *buf, size_t buflen) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    pid_t pid = getpid();
    snprintf(buf, buflen, "cortez-%ld-%d", (long)ts.tv_sec, (int)pid);
    return buf;
}

int main(int argc, char **argv) {
    const char *imgpath = NULL;
    size_t img_size_mb = 1024;
    size_t mem_mb = 512;
    size_t swap_mb = 1024;

    /* parse args */
    int i = 1;
    while (i < argc) {
        if (strcmp(argv[i], "--img-path") == 0) {
            imgpath = argv[++i];
        } else if (strcmp(argv[i], "--img-size-mb") == 0) {
            img_size_mb = (size_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--mem-mb") == 0) {
            mem_mb = (size_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--swap-mb") == 0) {
            swap_mb = (size_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--") == 0) {
            i++;
            break;
        } else {
            fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 2;
        }
        i++;
    }

    if (i >= argc) {
        fprintf(stderr, "Missing program to run (after --)\n");
        return 2;
    }
    char **child_argv = &argv[i];

    /* default img path => exe_dir/data.img */
    char exe_dir[PATH_MAX];
    if (!get_exe_dir(exe_dir, sizeof(exe_dir))) {
        /* fallback to current directory */
        if (!getcwd(exe_dir, sizeof(exe_dir))) strncpy(exe_dir, ".", sizeof(exe_dir));
    }
    char default_img[PATH_MAX];
    snprintf(default_img, sizeof(default_img), "%s/data.img", exe_dir);
    char imgbuf[PATH_MAX];
    if (imgpath) strncpy(imgbuf, imgpath, sizeof(imgbuf)-1);
    else strncpy(imgbuf, default_img, sizeof(imgbuf)-1);
    imgbuf[PATH_MAX-1] = '\0';
    imgpath = imgbuf;

    /* setup signal handlers for cleanup */
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGQUIT, on_signal);

    /* ensure image exists and is sized */
    ensure_image(imgpath, img_size_mb);

    /* attach loop device */
    char cmd[PATH_MAX + 256];
    g_loopdev[0] = '\0';
    int rc;
    if (!need_sudo()) {
        snprintf(cmd, sizeof(cmd), "losetup --find --show %s 2>/dev/null", imgpath);
        rc = run_cmd_capture(cmd, g_loopdev, sizeof(g_loopdev));
        if (rc != 0 || g_loopdev[0] == '\0') {
            snprintf(cmd, sizeof(cmd), "losetup --find --show %s", imgpath);
            rc = run_cmd_capture(cmd, g_loopdev, sizeof(g_loopdev));
        }
    } else {
        snprintf(cmd, sizeof(cmd), "losetup --find --show %s 2>/dev/null || sudo losetup --find --show %s", imgpath, imgpath);
        rc = run_cmd_capture(cmd, g_loopdev, sizeof(g_loopdev));
    }
    if (rc != 0 || g_loopdev[0] == '\0') die("failed to associate loop device for %s (try running this program with sudo/root)", imgpath);
    g_created_loop = 1;

    /* mkswap (ignore error if already a swap) */
    snprintf(cmd, sizeof(cmd), "mkswap %s 2>/dev/null || sudo mkswap %s", g_loopdev, g_loopdev);
    rc = run_cmd(cmd);
    if (rc != 0) {
        fprintf(stderr, "note: mkswap returned %d (device may already be formatted as swap)\n", rc);
    }

    /* swapon */
    snprintf(cmd, sizeof(cmd), "swapon %s 2>/dev/null || sudo swapon %s", g_loopdev, g_loopdev);
    rc = run_cmd(cmd);
    if (rc != 0) {
        cleanup_swap_and_loop();
        die("swapon failed (need root privileges)");
    }

    /* verify cgroup v2 */
    struct stat st;
    if (stat("/sys/fs/cgroup/cgroup.controllers", &st) != 0) {
        fprintf(stderr, "Warning: cgroup v2 not detected at /sys/fs/cgroup — cgroup operations may fail\n");
    }

    /* create cgroup dir */
    char unit[128];
    get_now_unit_name(unit, sizeof(unit));
    snprintf(g_cgdir, sizeof(g_cgdir), "/sys/fs/cgroup/%s", unit);
    if (mkdir(g_cgdir, 0755) != 0) {
        if (errno != EEXIST) {
            cleanup_swap_and_loop();
            die("Failed to create cgroup dir %s - try running as root", g_cgdir);
        }
    }

    /* write memory limits (bytes) */
    unsigned long long mem_bytes = (unsigned long long)mem_mb * 1024ULL * 1024ULL;
    unsigned long long swap_bytes = (unsigned long long)swap_mb * 1024ULL * 1024ULL;
    char path_buf[PATH_MAX];
    char tmpval[64];

    snprintf(path_buf, sizeof(path_buf), "%s/memory.max", g_cgdir);
    snprintf(tmpval, sizeof(tmpval), "%llu", (unsigned long long)mem_bytes);
    write_str_to_file_checked(path_buf, tmpval);

    snprintf(path_buf, sizeof(path_buf), "%s/memory.swap.max", g_cgdir);
    snprintf(tmpval, sizeof(tmpval), "%llu", (unsigned long long)swap_bytes);
    write_str_to_file_checked(path_buf, tmpval);

    /* fork and exec child */
    pid_t child = fork();
    if (child < 0) {
        cleanup_swap_and_loop();
        cleanup_cgroup();
        die("fork failed");
    }

    if (child == 0) {
        /* child: exec program */
        execvp(child_argv[0], child_argv);
        fprintf(stderr, "exec failed: %s\n", strerror(errno));
        _exit(127);
    }

    /* parent: add child pid to cgroup.procs */
    snprintf(path_buf, sizeof(path_buf), "%s/cgroup.procs", g_cgdir);
    char pidbuf[32];
    snprintf(pidbuf, sizeof(pidbuf), "%d", child);
    write_str_to_file_checked(path_buf, pidbuf);

    /* parent: announce child PID so process manager can capture it */
    fprintf(stderr, "CHILD_PID %d\n", (int)child);
    fflush(stderr);

    /* wait for child to exit */
    int status = 0;
    waitpid(child, &status, 0);

    /* cleanup */
    cleanup_swap_and_loop();
    cleanup_cgroup();

    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 0;
}