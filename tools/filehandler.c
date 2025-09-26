/*
 * filehandler.c
 *
 * Full-featured file copy utility used by Cortez Terminal.
 * Usage:
 *   filehandler [options] <src> <dst>
 *
 * Options:
 *   -f    force overwrite destination
 *   -n    no-clobber (don't overwrite)
 *   -s    copy symlink itself (create symlink at destination) instead of following
 *   -p    preserve permissions (mode) -- default on
 *   -o    preserve owner (uid/gid) -- best-effort (requires privileges)
 *   -t    preserve timestamps -- default on
 *   -v    verbose
 *   -h    help
 *
 * Behavior:
 *  - Copies src -> dst atomically by writing to a temporary file next to dst and renaming.
 *  - Preserves mode/timestamps/ownership as requested (best-effort; non-fatal failures).
 *  - Refuses to overwrite directories (unless -f and target is a file).
 *  - Detects same-file copy and treats as success (no-op).
 *
 * Compile:
 *   mkdir -p tools
 *   gcc -std=c11 -O2 -Wall -Wextra -o tools/filehandler tools/filehandler.c
 */

#define _POSIX_C_SOURCE 200809L
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libgen.h>
#include <inttypes.h>
#include <stdarg.h>

static int opt_force = 0;
static int opt_noclobber = 0;
static int opt_copy_symlink = 0;
static int opt_preserve_mode = 1;
static int opt_preserve_owner = 0;
static int opt_preserve_times = 1;
static int opt_verbose = 0;

static void eprintf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void vlog(const char *fmt, ...) {
    if (!opt_verbose) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

/* Check whether two paths refer to the same file (device + inode).
 * Returns 1 if same, 0 otherwise, -1 on error.
 */
static int same_file(const char *p1, const struct stat *st1, const char *p2) {
    struct stat st2;
    if (stat(p2, &st2) != 0) {
        if (errno == ENOENT) return 0;
        return -1;
    }
    return (st1->st_dev == st2.st_dev && st1->st_ino == st2.st_ino) ? 1 : 0;
}

/* Create a temporary file in the same directory as dest and return fd and path (caller must free tmp_path).
 * Template is destdir/.<basename>.tmp.XXXXXX
 */
static int create_temp_in_dir(const char *dest_path, char **out_tmp_path) {
    char *dup = NULL;
    char *dname = NULL;
    char *bname = NULL;
    char *tmp_template = NULL;
    int fd = -1;

    dup = strdup(dest_path);
    if (!dup) goto fail;
    dname = dirname(dup);
    if (!dname) goto fail;

    dup = strdup(dest_path); /* dirname clobbers, so remake for basename */
    if (!dup) goto fail;
    bname = basename(dup);
    if (!bname) goto fail;

    size_t tpl_len = strlen(dname) + 1 + strlen(bname) + 32;
    tmp_template = malloc(tpl_len);
    if (!tmp_template) goto fail;
    snprintf(tmp_template, tpl_len, "%s/.%s.tmp.XXXXXX", dname, bname);

    fd = mkstemp(tmp_template);
    if (fd < 0) {
        eprintf("mkstemp(%s) failed: %s\n", tmp_template, strerror(errno));
        free(tmp_template);
        free(dup);
        return -1;
    }

    /* set restrictive mode initially; will chmod later if requested */
    if (fchmod(fd, S_IRUSR | S_IWUSR) != 0) {
        /* non-fatal */
        vlog("warning: fchmod on temp file failed: %s\n", strerror(errno));
    }

    *out_tmp_path = tmp_template;
    return fd;

fail:
    if (dup) free(dup);
    if (tmp_template) free(tmp_template);
    return -1;
}

/* copy data from fd_src to fd_dst returning 0 on success */
static int copy_data(int fd_src, int fd_dst) {
    ssize_t nread, nwritten;
    static const size_t BUF_SIZE = 64 * 1024;
    void *buf = malloc(BUF_SIZE);
    if (!buf) return -1;

    lseek(fd_src, 0, SEEK_SET);
    lseek(fd_dst, 0, SEEK_SET);

    while ((nread = read(fd_src, buf, BUF_SIZE)) > 0) {
        ssize_t off = 0;
        while (off < nread) {
            nwritten = write(fd_dst, (char*)buf + off, (size_t)(nread - off));
            if (nwritten < 0) {
                if (errno == EINTR) continue;
                free(buf);
                return -1;
            }
            off += nwritten;
        }
    }
    if (nread < 0) {
        free(buf);
        return -1;
    }
    free(buf);
    return 0;
}

/* preserve timestamps using utimensat (works for modern POSIX) */
static int preserve_times(const char *dst, const struct stat *st_src) {
    struct timespec times[2];
    times[0] = st_src->st_atim;
    times[1] = st_src->st_mtim;
#if defined(UTIME_NOW)
    if (utimensat(AT_FDCWD, dst, times, 0) != 0) {
        eprintf("warning: utimensat failed on %s: %s\n", dst, strerror(errno));
        return -1;
    }
    return 0;
#else
    struct timeval tv[2];
    tv[0].tv_sec = st_src->st_atime;
    tv[0].tv_usec = 0;
    tv[1].tv_sec = st_src->st_mtime;
    tv[1].tv_usec = 0;
    if (utimes(dst, tv) != 0) {
        eprintf("warning: utimes failed on %s: %s\n", dst, strerror(errno));
        return -1;
    }
    return 0;
#endif
}

/* Main high-level copy operation: copy regular file src_path -> dst_path atomically.
 * Returns 0 on success, non-zero on failure (and prints diagnostics).
 */
static int copy_regular_file_atomic(const char *src_path, const char *dst_path, const struct stat *st_src) {
    int fd_src = -1, fd_tmp = -1;
    char *tmp_path = NULL;
    int rc = 1;

    fd_src = open(src_path, O_RDONLY);
    if (fd_src < 0) {
        eprintf("open(src=%s) failed: %s\n", src_path, strerror(errno));
        goto out;
    }

    fd_tmp = create_temp_in_dir(dst_path, &tmp_path);
    if (fd_tmp < 0) {
        goto out;
    }

    /* copy data */
    if (copy_data(fd_src, fd_tmp) != 0) {
        eprintf("copy error from %s -> temp %s: %s\n", src_path, tmp_path, strerror(errno));
        goto out;
    }

    /* ensure data is flushed to disk */
    if (fsync(fd_tmp) != 0) {
        eprintf("fsync(temp=%s) warning: %s\n", tmp_path, strerror(errno));
        /* not fatal */
    }

    close(fd_tmp);
    fd_tmp = -1;

    /* preserve mode first (chmod) */
    if (opt_preserve_mode) {
        if (chmod(tmp_path, st_src->st_mode & 07777) != 0) {
            eprintf("warning: chmod failed on %s: %s\n", tmp_path, strerror(errno));
        }
    }

    /* attempt to preserve ownership if requested */
    if (opt_preserve_owner) {
        if (chown(tmp_path, st_src->st_uid, st_src->st_gid) != 0) {
            /* ignore EPERM for non-root; warn */
            eprintf("warning: chown failed on %s: %s\n", tmp_path, strerror(errno));
        }
    }

    /* preserve timestamps */
    if (opt_preserve_times) {
        preserve_times(tmp_path, st_src);
    }

    /* Atomically rename temp -> dest */
    if (rename(tmp_path, dst_path) != 0) {
        eprintf("rename(%s -> %s) failed: %s\n", tmp_path, dst_path, strerror(errno));
        goto out;
    }

    /* Success */
    rc = 0;

out:
    if (fd_src >= 0) close(fd_src);
    if (fd_tmp >= 0) {
        /* try to close and remove temp file */
        close(fd_tmp);
        unlink(tmp_path);
    }
    if (tmp_path) free(tmp_path);
    return rc;
}

static void usage_and_exit(const char *prog) {
    fprintf(stderr,
        "Usage: %s [options] <src> <dst>\n"
        "Options:\n"
        "  -f    force overwrite existing destination\n"
        "  -n    no-clobber (do not overwrite)\n"
        "  -s    copy symlink itself (create symlink at dst) instead of following\n"
        "  -p    preserve mode (permissions) (default: on)\n"
        "  -o    preserve owner (try; may require privileges)\n"
        "  -t    preserve timestamps (default: on)\n"
        "  -v    verbose\n"
        "  -h    help\n",
        prog);
    exit(2);
}

int main(int argc, char **argv) {
    int c;
    while ((c = getopt(argc, argv, "fnspotvh")) != -1) {
        switch (c) {
            case 'f': opt_force = 1; break;
            case 'n': opt_noclobber = 1; break;
            case 's': opt_copy_symlink = 1; break;
            case 'p': opt_preserve_mode = 1; break;
            case 'o': opt_preserve_owner = 1; break;
            case 't': opt_preserve_times = 1; break;
            case 'v': opt_verbose = 1; break;
            case 'h':
            default:
                usage_and_exit(argv[0]);
        }
    }

    if (optind + 2 != argc) usage_and_exit(argv[0]);
    const char *src = argv[optind];
    const char *dst = argv[optind + 1];

    struct stat st_src;
    int st_src_ret;
    if (opt_copy_symlink) {
        /* If copying symlink itself, lstat to see if it's a symlink */
        st_src_ret = lstat(src, &st_src);
    } else {
        st_src_ret = stat(src, &st_src);
    }
    if (st_src_ret != 0) {
        eprintf("stat(%s) failed: %s\n", src, strerror(errno));
        return 10;
    }

    /* If destination exists, check status */
    struct stat st_dst;
    int dst_exists = (stat(dst, &st_dst) == 0);

    if (dst_exists && S_ISDIR(st_dst.st_mode)) {
        eprintf("Destination is a directory: %s\n", dst);
        return 11;
    }

    if (dst_exists && !opt_force && opt_noclobber) {
        eprintf("Destination exists and no-clobber specified: %s\n", dst);
        return 12;
    }

    if (dst_exists && !opt_force && !opt_noclobber) {
        /* allowed to overwrite by default - but respect same-file */
        int sf = same_file(src, &st_src, dst);
        if (sf < 0) {
            eprintf("Failed to check same-file: %s\n", strerror(errno));
            return 13;
        } else if (sf == 1) {
            /* same file; nothing to do */
            vlog("Source and destination are the same file; nothing to do.\n");
            return 0;
        }
    }

    /* Handle symlink copying if requested */
    if (opt_copy_symlink && S_ISLNK(st_src.st_mode)) {
        /* read link target */
        ssize_t len;
        size_t bufsize = 4096;
        char *linkbuf = malloc(bufsize);
        if (!linkbuf) {
            eprintf("malloc failed\n");
            return 20;
        }
        while (1) {
            len = readlink(src, linkbuf, bufsize);
            if (len < 0) {
                eprintf("readlink(%s) failed: %s\n", src, strerror(errno));
                free(linkbuf);
                return 21;
            }
            if ((size_t)len < bufsize) break;
            /* buffer too small, expand and retry */
            bufsize *= 2;
            char *tmp = realloc(linkbuf, bufsize);
            if (!tmp) { free(linkbuf); eprintf("realloc failed\n"); return 22; }
            linkbuf = tmp;
        }
        linkbuf[len] = '\0';
        /* remove dest if exists and force */
        if (dst_exists && opt_force) unlink(dst);
        if (symlink(linkbuf, dst) != 0) {
            eprintf("symlink(%s -> %s) failed: %s\n", linkbuf, dst, strerror(errno));
            free(linkbuf);
            return 23;
        }
        /* try to preserve ownership and times for symlink if possible (lchown may work) */
        if (opt_preserve_owner) {
            if (lchown(dst, st_src.st_uid, st_src.st_gid) != 0) {
                vlog("warning: lchown failed on %s: %s\n", dst, strerror(errno));
            }
        }
        /* timestamps on symlink are system-dependent; skipping */
        free(linkbuf);
        vlog("symlink created: %s -> %s\n", dst, src);
        return 0;
    }

    /* Only allow regular files for copying */
    if (!S_ISREG(st_src.st_mode) && !S_ISFIFO(st_src.st_mode)) {
        eprintf("Unsupported source file type (not regular or fifo): %s\n", src);
        return 30;
    }

    /* If dst exists and is same file, handled earlier; if exists and is not overwritten, fail */
    if (dst_exists && !opt_force && opt_noclobber) {
        eprintf("Destination exists and will not be overwritten: %s\n", dst);
        return 31;
    }

    /* If destination directory doesn't exist, try to create parent */
    char *dup_dst = strdup(dst);
    if (!dup_dst) { eprintf("malloc failed\n"); return 40; }
    char *dst_dir = dirname(dup_dst);
    struct stat st_dir;
    if (stat(dst_dir, &st_dir) != 0) {
        /* try to create the directory */
        if (mkdir(dst_dir, 0755) != 0) {
            /* creating parent failed — but maybe parent exists under race — try again */
            if (stat(dst_dir, &st_dir) != 0) {
                eprintf("Destination directory does not exist and could not be created: %s\n", dst_dir);
                free(dup_dst);
                return 41;
            }
        }
    }
    free(dup_dst);

    /* If dst exists and we are forcing, remove it to allow rename to succeed */
    if (dst_exists && opt_force) {
        if (unlink(dst) != 0) {
            if (errno != ENOENT) {
                eprintf("Failed to remove existing destination %s: %s\n", dst, strerror(errno));
                return 42;
            }
        }
    }

    /* Perform the atomic copy */
    int r = copy_regular_file_atomic(src, dst, &st_src);
    if (r != 0) {
        eprintf("Copy failed (%d)\n", r);
        return 100 + r;
    }

    vlog("Copied %s -> %s\n", src, dst);
    return 0;
}
