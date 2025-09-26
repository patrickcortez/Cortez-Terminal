/* src/cortez_backend.c
 *
 * Build:
 *     gcc -o cortez_backend cortez_backend.c -lutil -lpthread
 */

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pty.h>
#include <termios.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <signal.h>
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <sys/select.h>
#include <sys/time.h>
#include <pthread.h>
#include <fnmatch.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define MAXLINE 8192
#define PROJECTS_FILE "../data/projects.json"
#define MODULE_DIR    "../modules"
#define TOOLS_DIR     "../tools"
/* process data file */
#define PROCESS_FILE "../data/processdata.json"
#define BUF_SIZE 4096

/* ----- memory manager integration settings ----- */
/* Path under TOOLS_DIR where the memmgr binary lives */
#define MEMMGR_BIN_FORMAT "%s/cortez_memmgr"
#define MEMMGR_IMG_FORMAT "%s/data.img"
/* Process manager binary (wrapper that launches memmgr and tracks jobs) */
#define PM_BIN_FORMAT "%s/cortez_pm"


/* defaults (change if you want other defaults) */
#define MEMMGR_DEFAULT_IMG_SIZE_MB 1024
#define MEMMGR_DEFAULT_MEM_MB      512
#define MEMMGR_DEFAULT_SWAP_MB     1024

/* Build an argv array that runs: memmgr --img-path <img> --img-size-mb N --mem-mb M --swap-mb S -- -- prog args...
 * Ownership: returns a heap-allocated NULL-terminated char** where each element is strdup()'d.
 * Caller MUST let start_stream_thread/free_argv_array free the returned array.
 */
static char **build_memmgr_wrapped_argv(const char *tools_dir,
                                        const char *prog_path, char **prog_args, int prog_argc,
                                        size_t img_size_mb, size_t mem_mb, size_t swap_mb)
{
    const char *memmgr_bin_fmt = MEMMGR_BIN_FORMAT;
    const char *memmgr_img_fmt = MEMMGR_IMG_FORMAT;
    char memmgr_bin[PATH_MAX];
    char memmgr_img[PATH_MAX];
    snprintf(memmgr_bin, sizeof(memmgr_bin), memmgr_bin_fmt, tools_dir);
    snprintf(memmgr_img, sizeof(memmgr_img), memmgr_img_fmt, tools_dir);

    /* calculate total argv count:
     * memmgr + --img-path + path + --img-size-mb + val + --mem-mb + val + --swap-mb + val + -- + prog + prog_args... + NULL
     * that's 10 + prog_argc elements (approx). We'll allocate a little extra.
     */
    int extra = 12;
    int total = 1 + 2 + 2 + 2 + 2 + 1 + prog_argc + extra; /* safe upper bound */
    char **out = calloc(total, sizeof(char*));
    if (!out) return NULL;
    int idx = 0;

    out[idx++] = strdup(memmgr_bin);
    out[idx++] = strdup("--img-path");
    out[idx++] = strdup(memmgr_img);

    out[idx++] = strdup("--img-size-mb");
    char tmp[64]; snprintf(tmp, sizeof(tmp), "%zu", img_size_mb);
    out[idx++] = strdup(tmp);

    out[idx++] = strdup("--mem-mb");
    snprintf(tmp, sizeof(tmp), "%zu", mem_mb);
    out[idx++] = strdup(tmp);

    out[idx++] = strdup("--swap-mb");
    snprintf(tmp, sizeof(tmp), "%zu", swap_mb);
    out[idx++] = strdup(tmp);

    out[idx++] = strdup("--");

    /* append program + its args */
    if (prog_path) out[idx++] = strdup(prog_path);
    for (int i = 0; i < prog_argc; ++i) {
        if (prog_args && prog_args[i]) out[idx++] = strdup(prog_args[i]);
    }

    out[idx] = NULL;
    return out;
}

/* Build argv for: cortez_pm start <memmgr> --img-path <img> --img-size-mb N --mem-mb M --swap-mb S -- -- prog args...
 * Ownership: returns a heap-allocated NULL-terminated char** where each element is strdup()'d.
 * Caller MUST let start_stream_thread/free_argv_array free the returned array.
 */
static char **build_pm_wrapped_argv(const char *tools_dir,
                                    const char *prog_path, char **prog_args, int prog_argc,
                                    size_t img_size_mb, size_t mem_mb, size_t swap_mb)
{
    char pm_bin[PATH_MAX];
    char memmgr_bin[PATH_MAX];
    char memmgr_img[PATH_MAX];

    snprintf(pm_bin, sizeof(pm_bin), PM_BIN_FORMAT, tools_dir);
    snprintf(memmgr_bin, sizeof(memmgr_bin), MEMMGR_BIN_FORMAT, tools_dir); /* same format as before */
    snprintf(memmgr_img, sizeof(memmgr_img), MEMMGR_IMG_FORMAT, tools_dir);

    /* estimate size: pm + start + memmgr + args... + prog + prog_args + NULL */
    int extra = 16;
    int total = 1 + 1 + 1 + 6 + prog_argc + extra;
    char **out = calloc(total, sizeof(char*));
    if (!out) return NULL;
    int idx = 0;

    out[idx++] = strdup(pm_bin);         /* cortez_pm */
    out[idx++] = strdup("start");        /* subcommand */
    out[idx++] = strdup(memmgr_bin);     /* pass memmgr path so pm execs it */

    out[idx++] = strdup("--img-path");
    out[idx++] = strdup(memmgr_img);

    out[idx++] = strdup("--img-size-mb");
    char tmp[64]; snprintf(tmp, sizeof(tmp), "%zu", img_size_mb);
    out[idx++] = strdup(tmp);

    out[idx++] = strdup("--mem-mb");
    snprintf(tmp, sizeof(tmp), "%zu", mem_mb);
    out[idx++] = strdup(tmp);

    out[idx++] = strdup("--swap-mb");
    snprintf(tmp, sizeof(tmp), "%zu", swap_mb);
    out[idx++] = strdup(tmp);

    out[idx++] = strdup("--"); /* end of memmgr args forwarded by pm */

    /* append program + args */
    if (prog_path) out[idx++] = strdup(prog_path);
    for (int i = 0; i < prog_argc; ++i) {
        if (prog_args && prog_args[i]) out[idx++] = strdup(prog_args[i]);
    }

    out[idx] = NULL;
    return out;
}



/* --- base64 table & encoder --- */
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *b64_encode(const unsigned char *data, size_t in_len, size_t *out_len) {
    if (!data) return NULL;
    size_t olen = 4 * ((in_len + 2) / 3);
    char *out = malloc(olen + 1);
    if (!out) return NULL;
    size_t i = 0, o = 0;
    while (i < in_len) {
        unsigned int a = data[i++];
        unsigned int b = i < in_len ? data[i++] : 0;
        unsigned int c = i < in_len ? data[i++] : 0;
        unsigned int triple = (a << 16) | (b << 8) | c;
        out[o++] = b64_table[(triple >> 18) & 0x3F];
        out[o++] = b64_table[(triple >> 12) & 0x3F];
        out[o++] = (i - 1 <= in_len) ? b64_table[(triple >> 6) & 0x3F] : '=';
        out[o++] = (i <= in_len) ? b64_table[triple & 0x3F] : '=';
    }
    out[o] = '\0';
    if (out_len) *out_len = o;
    return out;
}

/* --- emit helpers --- */
static void emitf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fprintf(stdout, "\n");
    fflush(stdout);
}
static void emit_ok(const char *msg) { emitf("OK %s", msg ? msg : ""); }
static void emit_err(const char *msg) { emitf("ERR %s", msg ? msg : ""); }
static void emit_out(const char *msg) { emitf("OUT %s", msg ? msg : ""); }

/* --- simple tokenizer (supports double quotes like the Python version) --- */
static char **tokenize(const char *line, int *tokc) {
    *tokc = 0;
    if (!line) return NULL;
    char *buf = strdup(line);
    if (!buf) return NULL;
    char **arr = NULL;
    int cap = 0, cnt = 0;
    char *p = buf;
    while (*p) {
        while (*p && (*p == ' ' || *p == '\t')) p++;
        if (!*p) break;
        char *start;
        int quoted = 0;
        if (*p == '"') { quoted = 1; p++; start = p; }
        else { start = p; }
        char *tok = NULL;
        if (quoted) {
            char *q = start;
            while (*q && *q != '"') {
                if (*q == '\\' && *(q+1)) q++; /* allow backslash escaping */
                q++;
            }
            size_t len = q - start;
            tok = malloc(len + 1);
            if (!tok) break;
            strncpy(tok, start, len);
            tok[len] = '\0';
            p = (*q == '"') ? q + 1 : q;
        } else {
            char *q = start;
            while (*q && *q != ' ' && *q != '\t') q++;
            size_t len = q - start;
            tok = malloc(len + 1);
            if (!tok) break;
            strncpy(tok, start, len);
            tok[len] = '\0';
            p = q;
        }
        if (cnt + 1 >= cap) {
            cap = cap ? cap * 2 : 8;
            arr = realloc(arr, cap * sizeof(char*));
        }
        arr[cnt++] = tok;
    }
    free(buf);
    if (!arr) { *tokc = 0; return NULL; }
    arr[cnt] = NULL;
    *tokc = cnt;
    return arr;
}
static void free_tokens(char **toks, int c) {
    if (!toks) return;
    for (int i = 0; i < c; ++i) free(toks[i]);
    free(toks);
}

/* --- safe file read / write --- */
static char *read_whole_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    buf[sz] = '\0';
    fclose(f);
    return buf;
}


/* --- helper: find registered process path by name from PROCESS_FILE
 *   Returns 0 on success and writes path into out (null-terminated).
 *   Returns -1 if not found or on error.
 */
static int get_registered_process_path(const char *name, char *out, size_t outcap)
{
    char *s = read_whole_file(PROCESS_FILE);
    if (!s) return -1;

    /* naive search similar to project parsing used elsewhere:
       find the name, then find the "path" value in the same object */
    char *p = strstr(s, name);
    if (!p) { free(s); return -1; }

    char *path_key = strstr(p, "\"path\"");
    if (!path_key) { free(s); return -1; }

    /* find the first quote that begins the path value */
    char *q = strchr(path_key, '"');
    if (!q) { free(s); return -1; }
    q = strchr(q + 1, '"'); if (!q) { free(s); return -1; }
    q = strchr(q + 1, '"'); if (!q) { free(s); return -1; }
    char *q2 = strchr(q + 1, '"'); if (!q2) { free(s); return -1; }

    size_t len = q2 - (q + 1);
    if (len >= outcap) { free(s); return -1; }
    strncpy(out, q + 1, len);
    out[len] = '\0';

    free(s);
    return 0;
}

static int write_whole_file(const char *path, const char *data) {
    char tmp[PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    size_t len = strlen(data);
    if (fwrite(data, 1, len, f) != len) { fclose(f); unlink(tmp); return -1; }
    fflush(f); fsync(fileno(f));
    fclose(f);
    if (rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return 0;
}

/* forward declarations */
static int path_is_executable(const char *path);
static void free_argv_array(char **argv);

/* ----- proc (process manager) helpers ----- */

static int is_numeric(const char *s)
{
    if (!s || !*s) return 0;
    for (const char *p = s; *p; ++p) if (*p < '0' || *p > '9') return 0;
    return 1;
}

/* reader thread: forwards cortez_pm stdout/stderr lines to emit_out */
struct pm_reader_args {
    int fd;
    pid_t child;
};

static void *pm_reader_thread(void *v)
{
    struct pm_reader_args *a = (struct pm_reader_args*)v;
    int fd = a->fd;
    (void)a->child; /* reserved if you want to use child pid later */
    free(a);

    FILE *f = fdopen(fd, "r");
    if (!f) {
        close(fd);
        return NULL;
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t r;
    while ((r = getline(&line, &len, f)) != -1) {
        /* strip newline */
        while (r > 0 && (line[r-1] == '\n' || line[r-1] == '\r')) line[--r] = '\0';
        if (r == 0) continue;
        emit_out(line);
    }
    free(line);
    fclose(f);
    return NULL;
}

/* proc --list: run tools/cortez_pm list and send output back */
static void cmd_proc_list(void)
{
    char pm_path[PATH_MAX];
    snprintf(pm_path, sizeof(pm_path), "%s/cortez_pm", TOOLS_DIR);

    char cmd[PATH_MAX + 32];
    snprintf(cmd, sizeof(cmd), "%s list 2>&1", pm_path);

    FILE *fp = popen(cmd, "r");
    if (!fp) { emit_err("proc list failed"); return; }

    char line[1024];
    while (fgets(line, sizeof(line), fp)) {
        /* strip newline */
        size_t L = strlen(line);
        while (L && (line[L-1]=='\n' || line[L-1]=='\r')) line[--L] = '\0';
        emit_out(line);
    }
    pclose(fp);
    emit_ok("proc list");
}

/* proc --kill <pid-or-jobid> : numeric => kill(pid), otherwise forward to cortez_pm stop <jobid> */
static void cmd_proc_kill(const char *arg)
{
    if (!arg) { emit_err("proc kill needs argument"); return; }

    if (is_numeric(arg)) {
        int pid = atoi(arg);
        if (pid <= 0) { emit_err("invalid pid"); return; }
        if (kill(pid, SIGTERM) == 0) {
            emit_ok("killed");
        } else {
            emit_err(strerror(errno));
        }
        return;
    }

    char pm_path[PATH_MAX];
    snprintf(pm_path, sizeof(pm_path), "%s/cortez_pm", TOOLS_DIR);

    char cmd[PATH_MAX * 2];
    snprintf(cmd, sizeof(cmd), "%s stop %s 2>&1", pm_path, arg);
    int rc = system(cmd);
    if (rc == 0) emit_ok("proc stop");
    else emit_err("proc stop failed");
}

//ls - behaves exactly like ls but standalone
static void cmd_ls(void) {
    DIR *d = opendir(".");
    if (!d) {
        emit_err("cannot open current directory");
        return;
    }

    struct dirent *ent;
    char path[PATH_MAX];
    char output_line[PATH_MAX + 10];
    struct stat st;

    while ((ent = readdir(d)) != NULL) {
        /* Filter out . and .. */
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        snprintf(path, sizeof(path), "./%s", ent->d_name);
        if (lstat(path, &st) == 0) {
            snprintf(output_line, sizeof(output_line), "%s", ent->d_name);
            if (S_ISDIR(st.st_mode)) {
                strcat(output_line, "/");
            } else if (S_ISREG(st.st_mode) && (st.st_mode & S_IXUSR || st.st_mode & S_IXGRP || st.st_mode & S_IXOTH)) {
                strcat(output_line, "*");
            }
            emit_out(output_line);
        }
    }
    closedir(d);
    emit_ok("ls");
}

/* proc --start behavior:
   - "proc --start <name> <path>"  => register (add/update) name->path in PROCESS_FILE
   - "proc --start <name>"         => start the registered process (via cortez_pm)
*/
static void cmd_proc_start(int argc, char **argv)
{
    /* argc is the count of tokens after "--start" (as used in your caller) */
    if (argc < 1) {
        emit_err("proc start usage: proc --start <name> [<path>]");
        return;
    }

    const char *name = argv[0];

    /* If user supplied a path (argc >= 2) -> register / update */
    if (argc >= 2) {
        const char *path = argv[1];

        /* Read existing file (if any) */
        char *s = read_whole_file(PROCESS_FILE);
        if (!s) {
            /* create new file with single entry */
            char tmp[65536];
            snprintf(tmp, sizeof(tmp),
                     "{\n  \"%s\": { \"path\": \"%s\" }\n}\n",
                     name, path);
            if (write_whole_file(PROCESS_FILE, tmp) == 0) emit_ok("proc registered");
            else emit_err("write failed");
            return;
        }

        /* sanitize trailing spaces/newlines */
        size_t len = strlen(s);
        while (len && (s[len-1]=='\n' || s[len-1]==' ' || s[len-1]=='\t' || s[len-1]=='\r')) { s[--len]=0; }

        char out[131072]; /* large buffer for resulting file */
        if (strcmp(s, "{}") == 0 || strcmp(s, "{ }") == 0) {
            snprintf(out, sizeof(out),
                     "{\n  \"%s\": { \"path\": \"%s\" }\n}\n",
                     name, path);
        } else {
            /* If name already present, do a very small update: locate name and replace path.
               Simpler approach: if name exists, return an error to avoid complicated in-place replace.
               But we will prefer to append if not present. */
            if (strstr(s, name) != NULL) {
                /* naive: replace path for existing entry is tricky; inform user to remove then add instead
                   (keeps implementation simple and robust). */
                free(s);
                emit_err("name already exists; remove and re-add to change path");
                return;
            }

            /* append new entry before the final '}' */
            char *pos = strrchr(s, '}');
            if (!pos) { free(s); emit_err("malformed processdata.json"); return; }
            size_t pre_len = pos - s;
            if (pre_len + 4096 > sizeof(out)) { free(s); emit_err("processdata.json too large"); return; }
            strncpy(out, s, pre_len);
            out[pre_len] = 0;
            /* add trailing comma and new entry */
            strcat(out, ",\n  \"");
            strcat(out, name);
            strcat(out, "\": { \"path\": \"");
            strcat(out, path);
            strcat(out, "\" }\n}\n");
        }

        if (write_whole_file(PROCESS_FILE, out) == 0) emit_ok("proc registered");
        else emit_err("write failed");

        free(s);
        return;
    }

    /* Otherwise argc == 1 => start the named registered process */
    char exe_path[PATH_MAX];
    if (get_registered_process_path(name, exe_path, sizeof(exe_path)) != 0) {
        emit_err("process not found; add with: proc --start <name> <path>");
        return;
    }

    /* verify executable */
    if (!path_is_executable(exe_path)) {
        emit_err("executable not found or not executable");
        return;
    }

    /* Build cortez_pm invocation: pm_path start memmgr_path --img-path <img> --img-size-mb N --mem-mb M --swap-mb S -- <exe> */
    char pm_path[PATH_MAX];
    char memmgr_path[PATH_MAX];
    char imgpath[PATH_MAX];
    snprintf(pm_path, sizeof(pm_path), "%s/cortez_pm", TOOLS_DIR);
    snprintf(memmgr_path, sizeof(memmgr_path), "%s/cortez_memmgr", TOOLS_DIR);
    snprintf(imgpath, sizeof(imgpath), "%s/data.img", TOOLS_DIR);

    int reserve = 32 + 2; /* small fixed reserve (no extra runtime args in this mode) */
    char **pargv = calloc(reserve, sizeof(char*));
    if (!pargv) { emit_err("alloc failed"); return; }
    int idx = 0;
    pargv[idx++] = strdup(pm_path);
    pargv[idx++] = strdup("start");
    pargv[idx++] = strdup(memmgr_path);

    pargv[idx++] = strdup("--img-path");
    pargv[idx++] = strdup(imgpath);

    pargv[idx++] = strdup("--img-size-mb");
    char tmp[64];
    snprintf(tmp, sizeof(tmp), "%zu", (size_t)MEMMGR_DEFAULT_IMG_SIZE_MB);
    pargv[idx++] = strdup(tmp);

    pargv[idx++] = strdup("--mem-mb");
    snprintf(tmp, sizeof(tmp), "%zu", (size_t)MEMMGR_DEFAULT_MEM_MB);
    pargv[idx++] = strdup(tmp);

    pargv[idx++] = strdup("--swap-mb");
    snprintf(tmp, sizeof(tmp), "%zu", (size_t)MEMMGR_DEFAULT_SWAP_MB);
    pargv[idx++] = strdup(tmp);

    pargv[idx++] = strdup("--");

    /* program to run (the registered executable) */
    pargv[idx++] = strdup(exe_path);
    pargv[idx] = NULL;

    /* Create a pipe to capture cortez_pm stdout/stderr and fork the pm runner (same flow as before) */
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        free_argv_array(pargv);
        emit_err("pipe failed");
        return;
    }

    pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]); close(pipefd[1]);
        free_argv_array(pargv);
        emit_err("fork failed");
        return;
    }

    if (child == 0) {
        /* child: redirect stdout+stderr -> pipe write and exec cortez_pm */
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execv(pargv[0], pargv);
        _exit(127);
    }

    /* parent */
    close(pipefd[1]); /* keep pipefd[0] to read from cortez_pm */

    /* spawn reader thread to forward cortez_pm output to backend frontend pipeline */
    struct pm_reader_args *ra = malloc(sizeof(*ra));
    if (!ra) {
        close(pipefd[0]);
        emit_err("alloc failed");
        free_argv_array(pargv);
        return;
    }
    ra->fd = pipefd[0];
    ra->child = child;

    pthread_t thr;
    if (pthread_create(&thr, NULL, pm_reader_thread, ra) != 0) {
        close(pipefd[0]);
        free(ra);
        emit_err("pthread_create failed");
        /* let child run */
    } else {
        pthread_detach(thr);
    }

    free_argv_array(pargv);

    emitf("OK proc start requested pid=%d name=%s", (int)child, name);
}



/* --- module/project helpers (simple operations matched to Python) --- */
static void cmd_module_list(void) {
    DIR *d = opendir(MODULE_DIR);
    if (!d) {
        emit_err("Cannot open module dir");
        return;
    }

    /* dynamic array of strings for unique module names */
    char **names = NULL;
    int ncap = 0, ncnt = 0;

    auto_add_name:
    /* helper to add unique name */
    ;
    /* - implemented below as a static inline block for clarity - */

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        const char *dname = ent->d_name;
        if (dname[0] == '.') continue; /* skip hidden entries */

        char path[PATH_MAX];
        snprintf(path, sizeof(path), "%s/%s", MODULE_DIR, dname);

        struct stat st;
        if (stat(path, &st) != 0) {
            /* can't stat — skip */
            continue;
        }

        /* candidate module name we might emit */
        char candidate[PATH_MAX];
        candidate[0] = 0;

        if (S_ISREG(st.st_mode)) {
            /* regular file: if ends with .c -> base name; else if executable -> name */
            size_t L = strlen(dname);
            if (L > 2 && strcmp(dname + L - 2, ".c") == 0) {
                /* strip .c */
                strncpy(candidate, dname, L - 2);
                candidate[L - 2] = 0;
            } else if (st.st_mode & S_IXUSR) {
                /* executable file */
                strncpy(candidate, dname, sizeof(candidate)-1);
                candidate[sizeof(candidate)-1] = 0;
            }
        } else if (S_ISDIR(st.st_mode)) {
            /* directory: consider it a module if it contains a same-named executable or a .c file or any executable */
            /* 1) check MODULE_DIR/dname/dname (executable) */
            char subpath[PATH_MAX];
            snprintf(subpath, sizeof(subpath), "%s/%s/%s", MODULE_DIR, dname, dname);
            if (path_is_executable(subpath)) {
                strncpy(candidate, dname, sizeof(candidate)-1);
                candidate[sizeof(candidate)-1] = 0;
            } else {
                /* 2) scan directory for *.c or any executable file */
                DIR *sd = opendir(path);
                if (sd) {
                    struct dirent *s;
                    while ((s = readdir(sd)) != NULL) {
                        if (s->d_name[0] == '.') continue;
                        size_t SL = strlen(s->d_name);
                        char sp[PATH_MAX];
                        snprintf(sp, sizeof(sp), "%s/%s/%s", MODULE_DIR, dname, s->d_name);
                        struct stat sst;
                        if (stat(sp, &sst) == 0) {
                            if (S_ISREG(sst.st_mode)) {
                                if (SL > 2 && strcmp(s->d_name + SL - 2, ".c") == 0) {
                                    strncpy(candidate, dname, sizeof(candidate)-1);
                                    candidate[sizeof(candidate)-1] = 0;
                                    break;
                                }
                                if (sst.st_mode & S_IXUSR) {
                                    strncpy(candidate, dname, sizeof(candidate)-1);
                                    candidate[sizeof(candidate)-1] = 0;
                                    break;
                                }
                            }
                        }
                    }
                    closedir(sd);
                }
            }
        }

        if (candidate[0] == 0) continue; /* nothing recognized as a module for this entry */

        /* add unique */
        int found = 0;
        for (int i = 0; i < ncnt; ++i) {
            if (strcmp(names[i], candidate) == 0) { found = 1; break; }
        }
        if (!found) {
            if (ncnt + 1 >= ncap) {
                ncap = ncap ? ncap * 2 : 32;
                names = realloc(names, ncap * sizeof(char*));
            }
            names[ncnt++] = strdup(candidate);
        }
    }
    closedir(d);

    if (ncnt == 0) {
        emit_out("(no modules)");
    } else {
        /* optional: sort names alphabetically for stable output */
        /* simple insertion sort for small lists */
        for (int i = 1; i < ncnt; ++i) {
            char *key = names[i];
            int j = i - 1;
            while (j >= 0 && strcmp(names[j], key) > 0) { names[j+1] = names[j]; j--; }
            names[j+1] = key;
        }
        for (int i = 0; i < ncnt; ++i) {
            emit_out(names[i]);
            free(names[i]);
        }
    }
    free(names);

    emit_ok("module list done");
}


static void cmd_module_build(const char *name) {
    char src[PATH_MAX], out[PATH_MAX];
    snprintf(src, sizeof(src), "%s/%s.c", MODULE_DIR, name);
    snprintf(out, sizeof(out), "%s/%s", MODULE_DIR, name);
    struct stat st;
    if (stat(src, &st) != 0) { emit_err("source not found"); return; }
    pid_t p = fork();
    if (p == 0) {
        execlp("gcc", "gcc", "-Wall", "-O2", "-o", out, src, (char*)NULL);
        _exit(127);
    } else if (p > 0) {
        int status=0; waitpid(p, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) emit_ok("build succeeded");
        else emit_err("gcc failed");
    } else emit_err("fork failed");
}

static void cmd_module_add(const char *name, const char *srcpath) {
    char dest[PATH_MAX];
    snprintf(dest, sizeof(dest), "%s/%s", MODULE_DIR, name);
    FILE *in = fopen(srcpath, "rb");
    if (!in) { emit_err("source not found"); return; }
    FILE *out = fopen(dest, "wb");
    if (!out) { fclose(in); emit_err("cannot create dest"); return; }
    char buf[4096]; size_t r;
    while ((r = fread(buf,1,sizeof(buf),in)) > 0) fwrite(buf,1,r,out);
    fclose(in); fclose(out);
    chmod(dest, 0755);
    emit_ok("integrated");
}

static void cmd_module_remove(const char *name) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", MODULE_DIR, name);
    if (unlink(path) == 0) emit_ok("removed"); else emit_err("remove failed");
}

static void cmd_project_list(void) {
    char *s = read_whole_file(PROJECTS_FILE);
    if (!s) { emit_out("(no projects.json)"); emit_ok("done"); return; }
    emit_out(s); free(s); emit_ok("done");
}




static void cmd_project_add(const char *name, const char *path) {
    char *s = read_whole_file(PROJECTS_FILE);
    if (!s) {
        char tmp[4096];
        snprintf(tmp, sizeof(tmp), "{\n  \"%s\": { \"path\": \"%s\" }\n}\n", name, path);
        if (write_whole_file(PROJECTS_FILE, tmp) == 0) emit_ok("added"); else emit_err("write failed");
        return;
    }
    size_t len = strlen(s);
    while (len && (s[len-1]=='\n' || s[len-1]==' ' || s[len-1]=='\t' || s[len-1]=='\r')) { s[len-1]=0; len--; }
    if (len == 0) { free(s); cmd_project_add(name, path); return; }
    char out[65536];
    if (strcmp(s, "{}")==0 || strcmp(s, "{ }")==0) {
        snprintf(out, sizeof(out), "{\n  \"%s\": { \"path\": \"%s\" }\n}\n", name, path);
    } else {
        char *pos = strrchr(s, '}');
        if (!pos) { free(s); emit_err("malformed projects.json"); return; }
        size_t pre_len = pos - s;
        if (pre_len + 1024 > sizeof(out)) { free(s); emit_err("too large"); return; }
        strncpy(out, s, pre_len); out[pre_len]=0;
        strcat(out, ",\n  \""); strcat(out, name); strcat(out, "\": { \"path\": \""); strcat(out, path); strcat(out, "\" }\n}\n");
    }
    if (write_whole_file(PROJECTS_FILE, out) == 0) emit_ok("added"); else emit_err("write failed");
    free(s);
}

static void cmd_project_remove(const char *name) {
    char *s = read_whole_file(PROJECTS_FILE);
    if (!s) { emit_err("no projects.json"); return; }
    char *p = strstr(s, name);
    if (!p) { free(s); emit_err("not found"); return; }
    char *q = p;
    while (q > s && *q != '"') q--;
    if (q == s) { free(s); emit_err("malformed"); return; }
    char *colon = strstr(p, ":");
    if (!colon) { free(s); emit_err("malformed"); return; }
    char *brace = strchr(colon, '}');
    if (!brace) { free(s); emit_err("malformed"); return; }
    char *after = brace + 1;
    if (*after == ',') after++;
    size_t prefix = q - s;
    char newbuf[65536];
    if (prefix + strlen(after) + 1 > sizeof(newbuf)) { free(s); emit_err("too large"); return; }
    strncpy(newbuf, s, prefix); newbuf[prefix]=0; strcat(newbuf, after);
    if (write_whole_file(PROJECTS_FILE, newbuf) == 0) emit_ok("removed"); else emit_err("write failed");
    free(s);
}

/* --- THREAD-BASED PTY STREAMING --- */
static pthread_t stream_thread;
static int stream_running = 0;
static pthread_mutex_t stream_lock = PTHREAD_MUTEX_INITIALIZER;
static pid_t stream_child_pid = -1;
static int stream_master_fd = -1;

struct stream_args {
    char **argv; /* NULL-terminated */
};

static void free_argv_array(char **argv) {
    if (!argv) return;
    for (int i = 0; argv[i]; ++i) free(argv[i]);
    free(argv);
}

static void *stream_thread_func(void *v) {
    struct stream_args *sa = (struct stream_args*)v;
    char **argv = sa->argv;
    free(sa);

    int master_fd;
    pid_t pid = forkpty(&master_fd, NULL, NULL, NULL);
    if (pid < 0) {
        emit_err("forkpty failed");
        free_argv_array(argv);
        pthread_mutex_lock(&stream_lock); stream_running = 0; stream_child_pid = -1; stream_master_fd = -1; pthread_mutex_unlock(&stream_lock);
        return NULL;
    }

    if (pid == 0) {
        /* child */
        execvp(argv[0], argv);
        _exit(127);
    }

    /* parent: record running pid/fd */
    pthread_mutex_lock(&stream_lock);
    stream_running = 1;
    stream_child_pid = pid;
    stream_master_fd = master_fd;
    pthread_mutex_unlock(&stream_lock);

    emitf("STREAM_START %d", (int)pid);

    unsigned char buf[BUF_SIZE];
    while (1) {
        fd_set rfds;
        FD_ZERO(&rfds); FD_SET(master_fd, &rfds);
        struct timeval tv = { .tv_sec = 0, .tv_usec = 200000 };
        int rv = select(master_fd + 1, &rfds, NULL, NULL, &tv);
        if (rv > 0 && FD_ISSET(master_fd, &rfds)) {
            ssize_t r = read(master_fd, buf, sizeof(buf));
            if (r <= 0) break;
            size_t outlen = 0;
            char *b64 = b64_encode(buf, (size_t)r, &outlen);
            if (b64) {
                emitf("STREAM_DATA %s", b64);
                free(b64);
            }
        }
        int status;
        pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) {
            int code = 0;
            if (WIFEXITED(status)) code = WEXITSTATUS(status);
            emitf("STREAM_END %d", code);
            break;
        }
    }

    /* ensure child reaped */
    int status;
    waitpid(pid, &status, 0);

    close(master_fd);

    pthread_mutex_lock(&stream_lock);
    stream_running = 0;
    stream_child_pid = -1;
    stream_master_fd = -1;
    pthread_mutex_unlock(&stream_lock);

    free_argv_array(argv);
    return NULL;
}

/* helper starts stream thread for given NULL-terminated argv array (ownership transferred) */
static int start_stream_thread(char **argv) {
    pthread_mutex_lock(&stream_lock);
    if (stream_running) {
        pthread_mutex_unlock(&stream_lock);
        return -1; /* already running */
    }
    stream_running = 1; stream_child_pid = -1; stream_master_fd = -1;
    pthread_mutex_unlock(&stream_lock);

    struct stream_args *sa = calloc(1, sizeof(*sa));
    if (!sa) { pthread_mutex_lock(&stream_lock); stream_running = 0; pthread_mutex_unlock(&stream_lock); return -1; }
    sa->argv = argv;
    if (pthread_create(&stream_thread, NULL, stream_thread_func, sa) != 0) {
        free(sa);
        pthread_mutex_lock(&stream_lock); stream_running = 0; pthread_mutex_unlock(&stream_lock);
        return -1;
    }
    pthread_detach(stream_thread);
    return 0;
}

/* send a signal to the running PTY child (if any) */
static void send_signal_to_stream_child(int sig) {
    pthread_mutex_lock(&stream_lock);
    pid_t pid = stream_child_pid;
    pthread_mutex_unlock(&stream_lock);
    if (pid <= 0) return;

    /* prefer signalling the process group of the child */
    pid_t pg = getpgid(pid);
    if (pg > 0) {
        if (kill(-pg, sig) != 0) {
            /* fallback: send to single pid */
            kill(pid, sig);
        }
    } else {
        /* fallback */
        kill(pid, sig);
    }
}

/* --- write into running stream's PTY (thread-safe) --- */
static int send_input_to_stream(const char *data, size_t len, int add_newline) {
    if (!data) return -1;
    pthread_mutex_lock(&stream_lock);
    int fd = stream_master_fd;
    int running = stream_running && fd >= 0;
    pthread_mutex_unlock(&stream_lock);
    if (!running) return -1;

    ssize_t written = 0;
    const char *ptr = data;
    size_t towrite = len;

    while (towrite > 0) {
        ssize_t w = write(fd, ptr + written, towrite);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        written += w;
        towrite -= w;
    }

    if (add_newline) {
        const char nl = '\n';
        ssize_t w = write(fd, &nl, 1);
        if (w < 0 && errno != EINTR) return -1;
    }
    return 0;
}

/* --- helpers to check/prepare executable paths --- */
static int path_is_executable(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISREG(st.st_mode)) return 0;
    if (access(path, X_OK) == 0) return 1;
    return 0;
}

// Add this new function definition somewhere before main()
/* recursive removal helper */
static int rm_recursive_internal(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return -1;

    /* if it's a symlink or regular file or other non-dir, just unlink */
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) == 0) return 0;
        return -1;
    }

    /* directory: iterate */
    DIR *d = opendir(path);
    if (!d) return -1;
    struct dirent *ent;
    int rc = 0;
    while ((ent = readdir(d)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;

        char child[PATH_MAX];
        if ((size_t)snprintf(child, sizeof(child), "%s/%s", path, name) >= sizeof(child)) {
            rc = -1;
            errno = ENAMETOOLONG;
            break;
        }

        if (rm_recursive_internal(child) != 0) {
            rc = -1;
            break;
        }
    }
    closedir(d);
    if (rc != 0) return -1;

    /* now remove the directory itself */
    if (rmdir(path) != 0) return -1;
    return 0;
}

/* rm command: supports -r, -d, -pr
 * Now accepts argc/argv (like main) so dispatcher can pass flags + path.
 */
static void cmd_rm(int argc, char **argv)
{
    if (argc < 1) {
        emit_err("rm usage: rm [-r|-d|-pr] <path>");
        return;
    }

    int flag_r = 0, flag_d = 0, flag_pr = 0;
    char *target = NULL;

    /* parse flags: flags can be combined like -rd or separate */
    int i = 0;
    for (; i < argc; ++i) {
        char *tok = argv[i];
        if (!tok) continue;
        if (tok[0] != '-') {
            /* first non-flag token is the target */
            target = strdup(tok);
            ++i;
            break;
        }
        /* token starts with '-' -> parse flags */
        if (strcmp(tok, "-r") == 0) { flag_r = 1; continue; }
        if (strcmp(tok, "-d") == 0) { flag_d = 1; continue; }
        if (strcmp(tok, "-pr") == 0) { flag_pr = 1; continue; }
        /* combined short flags like -rd or -pr inside */
        for (size_t j = 1; tok[j]; ++j) {
            if (tok[j] == 'r') flag_r = 1;
            else if (tok[j] == 'd') flag_d = 1;
            else if (tok[j] == 'p' && tok[j+1] == 'r') { flag_pr = 1; break; }
        }
        /* continue scanning for target after flags */
    }

    /* if target not found yet, maybe it's the last arg */
    if (!target && i < argc) {
        if (argv[i]) target = strdup(argv[i]);
    }

    if (!target) {
        emit_err("rm missing path");
        return;
    }

    int res = -1;

    if (flag_pr) {
        /* remove file in parent directory: ../<target> */
        char path[PATH_MAX];
        if ((size_t)snprintf(path, sizeof(path), "../%s", target) >= sizeof(path)) {
            emit_err("path too long");
            goto out;
        }
        if (unlink(path) == 0) { emit_out("Removed file: "); emit_out(path); emit_ok("rm"); res = 0; }
        else { emit_err(strerror(errno)); res = -1; }
        goto out;
    }

    if (flag_r) {
        /* recursive delete (file or directory) */
        if (rm_recursive_internal(target) == 0) {
            emit_out("Removed recursively: "); emit_out(target); emit_ok("rm"); res = 0;
        } else {
            emit_err(strerror(errno));
            res = -1;
        }
        goto out;
    }

    if (flag_d) {
        /* remove empty directory only */
        if (rmdir(target) == 0) { emit_out("Removed directory: "); emit_out(target); emit_ok("rm"); res = 0; }
        else { emit_err(strerror(errno)); res = -1; }
        goto out;
    }

    /* default: remove file only (fail on directory) */
    {
        struct stat st;
        if (stat(target, &st) == 0 && S_ISDIR(st.st_mode)) {
            emit_err("target is a directory; use -r to remove recursively or -d to remove empty dir");
            res = -1;
            goto out;
        }
        if (unlink(target) == 0) {
            emit_out("Removed file: ");
            emit_out(target);
            emit_ok("rm");
            res = 0;
        } else {
            emit_err(strerror(errno));
            res = -1;
        }
    }

out:
    free(target);
    (void)res;
}



// Add this new function definition somewhere before main()
static void cmd_create(const char *filename) {
    if (!filename || filename[0] == '\0') {
        emit_err("create usage: create <filename>");
        return;
    }

    int fd = open(filename, O_CREAT | O_WRONLY, 0644);
    if (fd == -1) {
        emit_err(strerror(errno));
        return;
    }
    close(fd);

    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        emit_err("cannot get current directory");
        return;
    }

    char message[PATH_MAX + 100];
    snprintf(message, sizeof(message), "%s saved in %s", filename, cwd);
    emit_out(message);
    emit_ok("create");
}

/* --- run net tool under PTY --- */
static void cmd_net_run(int argc, char **argv) {
    char net_tool[PATH_MAX];
    snprintf(net_tool, sizeof(net_tool), "%s/net-twerk", TOOLS_DIR);
    if (access(net_tool, F_OK) != 0) { emit_err("net tool not found; build tools/net-twerk.c"); return; }
    struct stat st;
    if (stat(net_tool, &st) == 0) {
        if (!(st.st_mode & S_IXUSR)) {
            chmod(net_tool, st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
        }
    }
    /* build argv: net_tool + argv[0..argc-1] */
    char **pargv = calloc(argc + 2, sizeof(char*));
    if (!pargv) { emit_err("alloc failed"); return; }
    pargv[0] = strdup(net_tool);
    for (int i = 0; i < argc; ++i) pargv[i+1] = strdup(argv[i]);
    pargv[argc+1] = NULL;

    if (start_stream_thread(pargv) != 0) {
        free_argv_array(pargv);
        emit_err("another stream is running");
        return;
    }
    emit_ok("net started");
}

/* --- change directory (built-in) --- */
static void cmd_cd(const char *path) {
    const char *target = path;
    if (!target || target[0] == '\0') {
        const char *home = getenv("HOME");
        target = home ? home : ".";
    }
    if (chdir(target) == 0) {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            emit_out(cwd);    /* show new cwd */
            emit_ok("cd");
        } else {
            emit_ok("cd");
        }
    } else {
        emit_err(strerror(errno));
    }
}

// Add this new function definition somewhere before main()
static int make_path(char *path, int p_flag) {
    char *p, *slash;
    mode_t mode = 0755;
    int rc = 0;
    
    if (strcmp(path, "/") == 0 || strcmp(path, ".") == 0 || strcmp(path, "..") == 0) {
        errno = EEXIST;
        return -1;
    }

    if (p_flag) {
        p = strdup(path);
        if (!p) {
            emit_err("strdup failed");
            return -1;
        }
        slash = p;
        while ((slash = strchr(slash, '/')) != NULL) {
            if (slash != p) {
                *slash = '\0';
                if (mkdir(p, mode) != 0 && errno != EEXIST) {
                    emit_err(strerror(errno));
                    rc = -1;
                    break;
                }
                *slash = '/';
            }
            slash++;
        }
        free(p);
        if (rc != 0) return -1;
    }

    if (mkdir(path, mode) != 0 && errno != EEXIST) {
        emit_err(strerror(errno));
        return -1;
    }
    
    return 0;
}

// Add this new function definition somewhere with the other cmd_ functions, like cmd_ls or cmd_rm
static void cmd_mkdir(int argc, char **argv) {
    if (argc < 1) {
        emit_err("mkdir usage: mkdir [-p] <path>");
        return;
    }

    int p_flag = 1;
    int path_idx = 0;
    
    if (argc >= 2 && strcmp(argv[0], "-p") == 0) {
        p_flag = 1;
        path_idx = 1;
    }
    
    if (path_idx >= argc) {
        emit_err("mkdir usage: mkdir [-p] <path>");
        return;
    }

    if (make_path(argv[path_idx], p_flag) == 0) {
        emit_ok("directory created");
    }
}

/* --- run local net-runner (command name: netr) --- */
static void cmd_netr_run(int argc, char **argv) {
    char net_tool[PATH_MAX];

    /* prefer local ./net-runner (same folder as backend/frontend/netrunner) */
    snprintf(net_tool, sizeof(net_tool), "./net-runner");
    if (access(net_tool, X_OK) != 0) {
        /* fallback to tools dir for compatibility */
        snprintf(net_tool, sizeof(net_tool), "%s/net-runner", TOOLS_DIR);
        if (access(net_tool, X_OK) != 0) {
            emit_err("net-runner not found; build it in src or tools");
            return;
        }
    }

    /* ensure executable bit */
    struct stat st;
    if (stat(net_tool, &st) == 0) {
        if (!(st.st_mode & S_IXUSR)) {
            chmod(net_tool, st.st_mode | S_IXUSR | S_IXGRP | S_IXOTH);
        }
    }

    /* build argv: net_tool + argv[0..argc-1] */
    char **pargv = calloc(argc + 2, sizeof(char*));
    if (!pargv) { emit_err("alloc failed"); return; }
    pargv[0] = strdup(net_tool);
    for (int i = 0; i < argc; ++i) pargv[i+1] = strdup(argv[i]);
    pargv[argc+1] = NULL;

    if (start_stream_thread(pargv) != 0) {
        free_argv_array(pargv);
        emit_err("another stream is running");
        return;
    }
    emit_ok("netr started");
}

/* --- start cedit tool under PTY stream --- */
static void cmd_cedit(int argc, char **argv) {
    char cedit_tool[PATH_MAX];
    snprintf(cedit_tool, sizeof(cedit_tool), "%s/cedit", TOOLS_DIR);

    if (access(cedit_tool, X_OK) != 0) {
        emit_err("cedit not found in tools/");
        return;
    }

   /* Build pargv: cedit_tool [ user-args ... ] NULL
 * Resolve any relative filenames to absolute paths using backend's cwd.
 */
int cnt = argc + 1;
char **pargv = calloc(cnt + 1, sizeof(char*));
if (!pargv) { emit_err("alloc failed"); return; }
pargv[0] = strdup(cedit_tool);

/* get backend cwd once */
char cwd[PATH_MAX] = {0};
if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';

for (int i = 0; i < argc; ++i) {
    const char *a = argv[i];
    if (!a) {
        pargv[i+1] = strdup("");
        continue;
    }

    char resolved[PATH_MAX];
    /* absolute path or dot-relative or ~ expansion -> preserve/expand */
    if (a[0] == '/') {
        pargv[i+1] = strdup(a);
    } else if (a[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) home = "";
        snprintf(resolved, sizeof(resolved), "%s%s", home, a + 1); /* skip ~ */
        pargv[i+1] = strdup(resolved);
    } else if (a[0] == '.' && (a[1] == '/' || (a[1]=='.' && a[2]=='/'))) {
        /* keep relative with dot segments — but make absolute by prefixing cwd */
        snprintf(resolved, sizeof(resolved), "%s/%s", cwd, a);
        pargv[i+1] = strdup(resolved);
    } else {
        /* plain relative filename -> make it absolute by prefixing backend cwd */
        snprintf(resolved, sizeof(resolved), "%s/%s", cwd, a);
        pargv[i+1] = strdup(resolved);
    }
}

pargv[cnt] = NULL;


    emit_ok("cedit started");
}


/* --- list directories (lsdir) in current directory --- */
static void cmd_lsdir(void) {
    DIR *d = opendir(".");
    if (!d) {
        emit_err("cannot open current directory");
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.') continue; /* skip hidden by default */
        /* we want directories only */
        struct stat st;
        if (stat(ent->d_name, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                emit_out(ent->d_name);
            }
        }
    }
    closedir(d);
    emit_ok("lsdir");
}

/* --- start an interactive shell (or arbitrary command) in a PTY stream --- */
static void cmd_shell(int argc, char **argv) {
    char shell_tool[PATH_MAX];
    snprintf(shell_tool, sizeof(shell_tool), "%s/shell", TOOLS_DIR);

    if (access(shell_tool, X_OK) == 0) {
        /* Use tools/shell and pass through any user args.
           Build pargv: shell_tool [ user-args ... ] NULL
         */
        int cnt = argc + 1;
        char **pargv = calloc(cnt + 1, sizeof(char*));
        if (!pargv) { emit_err("alloc failed"); return; }
        pargv[0] = strdup(shell_tool);
        for (int i = 0; i < argc; ++i) pargv[i+1] = strdup(argv[i]);
        pargv[cnt] = NULL;
        if (start_stream_thread(pargv) != 0) {
            free_argv_array(pargv);
            emit_err("another stream is running");
            return;
        }
        emit_ok("shell started (tools/shell)");
        return;
    }

    /* fallback: if user provided explicit command, start it directly under PTY */
    if (argc >= 1 && argv && argv[0]) {
        char **pargv = calloc(argc + 1 + 1, sizeof(char*));
        if (!pargv) { emit_err("alloc failed"); return; }
        for (int i = 0; i < argc; ++i) pargv[i] = strdup(argv[i]);
        pargv[argc] = NULL;
        if (start_stream_thread(pargv) != 0) {
            free_argv_array(pargv);
            emit_err("another stream is running");
            return;
        }
        emit_ok("shell started");
        return;
    }

    /* No args: pick user's shell from env or fallback to /bin/sh and run it interactively */
    const char *shell = getenv("SHELL");
    if (!shell) shell = "/bin/sh";

    char **pargv = calloc(3, sizeof(char*));
    if (!pargv) { emit_err("alloc failed"); return; }
    pargv[0] = strdup(shell);
    pargv[1] = strdup("-i");
    pargv[2] = NULL;

    if (start_stream_thread(pargv) != 0) {
        free_argv_array(pargv);
        emit_err("another stream is running");
        return;
    }
    emit_ok("shell started");
}


/* --- run tools/pwd and emit its stdout as OUT lines --- */
static void cmd_pwd(void) {
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        emit_out(cwd);
        emit_ok("pwd");
    } else {
        emit_err(strerror(errno));
    }
}


/* --- run module executable under PTY (given path) --- */
static void cmd_module_run_by_path(const char *path, int argc, char **argv) {
    /* If path not executable, bail */
    if (!path_is_executable(path)) { emit_err("not executable"); return; }

    /* wrap with process-manager which launches memmgr */
    char **wrapped = build_pm_wrapped_argv(TOOLS_DIR, path, argv, argc,
                                           MEMMGR_DEFAULT_IMG_SIZE_MB,
                                           MEMMGR_DEFAULT_MEM_MB,
                                           MEMMGR_DEFAULT_SWAP_MB);
    if (!wrapped) { emit_err("alloc failed"); return; }

    if (start_stream_thread(wrapped) != 0) {
        /* start_stream_thread takes ownership on success; on failure we must free */
        free_argv_array(wrapped);
        emit_err("another stream is running or start failed");
        return;
    }
    emit_ok("module started (pm->vm)");
}




/* --- launch project by reading projects.json (naive parse like Python) --- */
static void cmd_project_launch(const char *name) {
    char *s = read_whole_file(PROJECTS_FILE);
    if (!s) { emit_err("no projects.json"); return; }
    char *p = strstr(s, name);
    if (!p) { free(s); emit_err("not found"); return; }
    char *path = strstr(p, "\"path\"");
    if (!path) { free(s); emit_err("no path entry"); return; }
    char *quote = strchr(path, '"');
    if (!quote) { free(s); emit_err("malformed"); return; }
    quote = strchr(quote+1, '"'); if (!quote) { free(s); emit_err("malformed"); return; }
    quote = strchr(quote+1, '"'); if (!quote) { free(s); emit_err("malformed"); return; }
    char *quote2 = strchr(quote+1, '"'); if (!quote2) { free(s); emit_err("malformed"); return; }
    size_t len = quote2 - (quote+1);
    char exe[PATH_MAX];
    if (len >= sizeof(exe)-1) { free(s); emit_err("path too long"); return; }
    strncpy(exe, quote+1, len); exe[len] = 0;
    free(s);

    /* If exe contains shell metacharacters, run with /bin/sh -c (still wrapped by memmgr) */
    if (strchr(exe, '|') || strchr(exe, '>') || strchr(exe, '<') || strchr(exe, '&')) {
        /* prepare prog argv: /bin/sh -c "<exe>" */
        char *prog_args_arr[2];
        prog_args_arr[0] = strdup("-c"); /* placeholder, will be provided in wrapper as separate strings */
        /* build wrapped: memmgr -- ... -- /bin/sh -c "<exe>" */
                /* build wrapped: pm start memmgr -- ... -- /bin/sh -c "<exe>" */
        char **wrapped = build_pm_wrapped_argv(TOOLS_DIR, "/bin/sh", (char**)NULL, 0,
                                               MEMMGR_DEFAULT_IMG_SIZE_MB,
                                               MEMMGR_DEFAULT_MEM_MB,
                                               MEMMGR_DEFAULT_SWAP_MB);

        if (!wrapped) { emit_err("alloc failed"); return; }

        /* we need to append "-c" and exe as additional args before NULL */
        /* compute current length */
        int cur = 0; while (wrapped[cur]) cur++;
        /* expand array */
        char **neww = realloc(wrapped, (cur + 3) * sizeof(char*));
        if (!neww) { free_argv_array(wrapped); emit_err("alloc failed"); return; }
        wrapped = neww;
        wrapped[cur++] = strdup("-c");
        wrapped[cur++] = strdup(exe);
        wrapped[cur] = NULL;

        if (start_stream_thread(wrapped) != 0) {
            free_argv_array(wrapped);
            emit_err("another stream is running");
            return;
        }
        emit_ok("project started (shell vm)");
        return;
    } else {
        if (!path_is_executable(exe)) { emit_err("executable not found or not executable"); return; }

        /* no shell meta chars: run exe directly, wrapped by memmgr */
        cmd_module_run_by_path(exe, 0, NULL);
        return;
    }
}

/* read command: read <filename> -- print file contents line-by-line */
static void cmd_read(const char *arg)
{
    if (!arg) {
        emit_err("read usage: read <filename>");
        return;
    }

    /* skip leading spaces */
    const char *p = arg;
    while (*p == ' ' || *p == '\t') ++p;
    if (*p == '\0') {
        emit_err("read usage: read <filename>");
        return;
    }

    /* check that target is not a directory */
    struct stat st;
    if (stat(p, &st) != 0) {
        emit_err(strerror(errno));
        return;
    }
    if (S_ISDIR(st.st_mode)) {
        emit_err("target is a directory");
        return;
    }

    FILE *f = fopen(p, "r");
    if (!f) {
        emit_err(strerror(errno));
        return;
    }

    char *line = NULL;
    size_t len = 0;
    ssize_t nread;
    while ((nread = getline(&line, &len, f)) != -1) {
        /* strip trailing newline for cleaner output (optional) */
        if (nread > 0 && line[nread - 1] == '\n') line[nread - 1] = '\0';
        emit_out(line);
    }

    free(line);
    fclose(f);

    /* If there was an error other than EOF, report it */
    if (ferror(f)) {
        emit_err(strerror(errno));
        return;
    }

    emit_ok("read");
}



/* --- compile module helper used by commands that expect it --- */
static int compile_module_and_get_exe(const char *name, char *outpath, size_t outcap) {
    char src[PATH_MAX];
    snprintf(src, sizeof(src), "%s/%s.c", MODULE_DIR, name);
    snprintf(outpath, outcap, "%s/%s", MODULE_DIR, name);
    struct stat st;
    if (stat(src, &st) != 0) return -1;
    pid_t p = fork();
    if (p == 0) {
        execlp("gcc", "gcc", "-Wall", "-O2", "-o", outpath, src, (char*)NULL);
        _exit(127);
    } else if (p > 0) {
        int status = 0; waitpid(p, &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            chmod(outpath, 0755);
            return 0;
        }
        return -1;
    }
    return -1;
}

/* --- memory-list helper: find swap usage for tools/data.img (reads /proc/swaps) --- */
static int get_swap_usage_for_image(const char *imgpath, unsigned long long *out_used_kb, unsigned long long *out_size_kb)
{
    char resolved[PATH_MAX];
    if (realpath(imgpath, resolved) == NULL) {
        /* fallback to provided path */
        strncpy(resolved, imgpath, sizeof(resolved)-1);
        resolved[sizeof(resolved)-1] = '\0';
    }

    /* 1) try to find a loopdevice backing the image via losetup -j <img> */
    char cmd[PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "losetup -j '%s' 2>/dev/null", resolved);

    FILE *fp = popen(cmd, "r");
    char loopdev[PATH_MAX] = {0};
    if (fp) {
        char line[512];
        if (fgets(line, sizeof(line), fp) != NULL) {
            /* line looks like: /dev/loop0: [0073]:12345 (/full/path/data.img) */
            char *colon = strchr(line, ':');
            if (colon) {
                size_t n = colon - line;
                if (n < sizeof(loopdev)) {
                    strncpy(loopdev, line, n);
                    loopdev[n] = '\0';
                }
            }
        }
        pclose(fp);
    }

    /* 2) parse /proc/swaps and look for either the loopdev or the image path */
    FILE *sf = fopen("/proc/swaps", "r");
    if (!sf) return -1;
    /* skip header line */
    char header[256];
    if (!fgets(header, sizeof(header), sf)) { fclose(sf); return -1; }

    char fname[PATH_MAX];
    char type[64];
    unsigned long long size_kb = 0;
    unsigned long long used_kb = 0;
    int prio = 0;
    int found = 0;
    while (fscanf(sf, "%255s %63s %llu %llu %d", fname, type, &size_kb, &used_kb, &prio) == 5) {
        if (loopdev[0] && strcmp(fname, loopdev) == 0) {
            found = 1; break;
        }
        /* some systems show the file path directly in /proc/swaps */
        if (strcmp(fname, resolved) == 0) {
            found = 1; break;
        }
        /* last-ditch: if fname contains the image basename, match that */
        if (strstr(fname, imgpath) != NULL) {
            found = 1; break;
        }
    }
    fclose(sf);

    if (!found) return -1;

    if (out_used_kb) *out_used_kb = used_kb;
    if (out_size_kb) *out_size_kb = size_kb;
    return 0;
}

/* --- lsmem command: emit how much of tools/data.img swap is used --- */
static void cmd_lsmem(void)
{
    char imgpath[PATH_MAX];
    snprintf(imgpath, sizeof(imgpath), "%s/data.img", TOOLS_DIR);

    unsigned long long used_kb = 0, size_kb = 0;
    if (get_swap_usage_for_image(imgpath, &used_kb, &size_kb) == 0 && size_kb > 0) {
        unsigned long long used_mb = (used_kb + 512) / 1024;
        unsigned long long total_mb = (size_kb + 512) / 1024;
        char out[128];
        snprintf(out, sizeof(out), "LSMEM %lluMB / %lluMB used (data.img)", used_mb, total_mb);
        emit_out(out);
    } else {
        /* if not active, report zero used and 1024MB total (match your 1GB image default) */
        emit_out("LSMEM 0MB / 1024MB used (data.img not active)");
    }
    emit_ok("lsmem");
}


/* --- main loop --- */
int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    char line[MAXLINE];

    emit_ok("backend ready");
    cmd_lsmem();

    while (fgets(line, sizeof(line), stdin)) {
        /* trim newline */
        size_t ln = strlen(line);
        while (ln && (line[ln-1]=='\n' || line[ln-1]=='\r')) { line[--ln]=0; }

        if (ln == 0) continue;

        /* Special control commands sent by frontends */
        if (strcmp(line, "SIGINT") == 0) {
            send_signal_to_stream_child(SIGINT);
            emit_ok("sent SIGINT");
            continue;
        }
        if (strcmp(line, "SIGTERM") == 0) {
            send_signal_to_stream_child(SIGTERM);
            emit_ok("sent SIGTERM");
            continue;
        }

        int tokc = 0;
        char **tok = tokenize(line, &tokc);
        if (!tok || tokc == 0) { emit_err("parse error"); free_tokens(tok, tokc); continue; }

        if (strcmp(tok[0], "help") == 0 || strcmp(tok[0], "?") == 0) {
            emit_out("Commands:");
            emit_out("  help                   - show this help");
            emit_out("  load <proj>            - launch a project (must be added first)");
            emit_out("  load --list            - list known projects");
            emit_out("  load --add <name> <path>  - add a project");
            emit_out("  load --remove <name>   - remove a project");
            emit_out("  module ...             - module commands (build/list/run/integrate)");
            emit_out("  net <args...>          - run tools/net-twerk with provided args | --show,--connect,--disconnect)");
            emit_out("  netr <args...>         - Much advanced networking commands | wget, curl, showp,ftp");
            emit_out("  wipe                   - clears the screen");
            emit_out("  pwd                    - displays your present working dir");
            emit_out("  ls                     - lists every file in the current dir");
            emit_out("  create <filename.ext>  - creates the file in the current dir");
            emit_out("  rm                     - removes/deletes files | -r (recursively delete) | -d (delete empty dir)");
            emit_out("  shell                  - use normal shell");
            emit_out("  shutdown                   - quit backend");
            emit_out("  lsmem                  - Displays how much vmemory is being used");
            emit_out("  proc --start [-- <prog> [args...]] - start a background job (via tools/cortez_pm)");
            emit_out("  proc --list                           - list background jobs (via tools/cortez_pm)");
            emit_out("  proc --kill <pid|jobid>               - kill a job by pid or jobid");
            emit_out("  cedit                  - Text Editor for editing files | cedit <filename>");
            emit_out("  mkdir <dir/subdir>     - Makes directories and their subdir.");
            emit_ok("<--------------------------------------------------------------------------------------->");
        } else if (strcmp(tok[0], "shutdown") == 0) {
            emit_ok("shutting down...");
            free_tokens(tok, tokc);
            break;
        } else if (strcmp(tok[0], "load") == 0) {
            if (tokc >= 2 && strcmp(tok[1], "--list") == 0) {
                cmd_project_list();
            } else if (tokc >= 3 && strcmp(tok[1], "--add") == 0) {
                cmd_project_add(tok[2], tokc >= 4 ? tok[3] : "");
            } else if (tokc >= 3 && strcmp(tok[1], "--remove") == 0) {
                cmd_project_remove(tok[2]);
            } else if (tokc >= 2) {
                cmd_project_launch(tok[1]);
            } else {
                emit_err("load usage");
            }
        } else if (strcmp(tok[0], "module") == 0) {
            if (tokc >= 2 && (strcmp(tok[1], "--list") == 0)) {
                cmd_module_list();
            } else if (tokc >= 3 && strcmp(tok[1], "--build") == 0) {
                cmd_module_build(tok[2]);
            } else if (tokc >= 4 && (strcmp(tok[1], "--add") == 0 || strcmp(tok[1], "add") == 0)) {
                cmd_module_add(tok[2], tok[3]);
            } else if (tokc >= 3 && (strcmp(tok[1], "--remove") == 0 || strcmp(tok[1], "remove") == 0)) {
                cmd_module_remove(tok[2]);
            } else if (tokc >= 2) {
                /* module <name> [args...] -> run module/<name> (compile if .c exists) */
                char exe[PATH_MAX];
                snprintf(exe, sizeof(exe), "%s/%s", MODULE_DIR, tok[1]);
                struct stat st;
                if (stat(exe, &st) != 0) {
                    /* try compile if .c exists */
                    char src[PATH_MAX];
                    snprintf(src, sizeof(src), "%s/%s.c", MODULE_DIR, tok[1]);
                    if (stat(src, &st) == 0) {
                        if (compile_module_and_get_exe(tok[1], exe, sizeof(exe)) != 0) {
                            emit_err("build failed");
                            free_tokens(tok, tokc);
                            continue;
                        }
                    } else {
                        emit_err("module not found");
                        free_tokens(tok, tokc);
                        continue;
                    }
                }
                /* build argv of arguments after name */
                int argn = tokc - 2;
                char **args = NULL;
                if (argn > 0) {
                    args = calloc(argn, sizeof(char*));
                    for (int i = 0; i < argn; ++i) args[i] = strdup(tok[2 + i]);
                }
                cmd_module_run_by_path(exe, argn, args);
                if (args) { for (int i=0;i<argn;i++) free(args[i]); free(args); }
            } else {
                emit_err("module usage");
            }
        } else if (strcmp(tok[0], "net") == 0) {
            if (tokc == 1) {
                emit_out("Usage: net <args...>   (this will run tools/net-twerk with the args)");
                emit_ok("net usage");
            } else {
                cmd_net_run(tokc - 1, &tok[1]);
            }
                } else if (strcmp(tok[0], "STDIN") == 0) {
            /* send remainder of the original line as input + newline */
            const char *rest = line + strlen(tok[0]);
            while (*rest == ' ') rest++;
            if (!*rest) {
                emit_err("no input");
            } else {
                if (send_input_to_stream(rest, strlen(rest), 1) == 0) emit_ok("stdin sent");
                else emit_err("no stream");
            }
        } else if (strcmp(tok[0], "WRITE") == 0) {
            /* send remainder raw (no newline) */
            const char *rest = line + strlen(tok[0]);
            while (*rest == ' ') rest++;
            if (!*rest) {
                emit_err("no input");
            } else {
                if (send_input_to_stream(rest, strlen(rest), 0) == 0) emit_ok("write sent");
                else emit_err("no stream");
            } 
        } else if (strcmp(tok[0], "wipe") == 0 || strcmp(tok[0], "clear") == 0) {
            /* ANSI: clear screen + move cursor home */
            const char *clr = "\x1b[2J\x1b[H";

            pthread_mutex_lock(&stream_lock);
            int running = stream_running && stream_master_fd >= 0;
            int fd = stream_master_fd;
            pthread_mutex_unlock(&stream_lock);

            if (running) {
                /* write directly to PTY so the child terminal clears */
                ssize_t r = write(fd, clr, strlen(clr));
                if (r >= 0) emit_ok("wiped");
                else emit_err("write failed");
            } else {
                /* no PTY -> emit the control sequence as OUT so frontends that honor ANSI will clear */
                emit_out(clr);
                emit_ok("wiped");
            }
        }  else if (strcmp(tok[0], "pwd") == 0) {
            cmd_pwd();
        } else if (strcmp(tok[0], "cd") == 0) {
    /* cd [path] */
    if (tokc >= 2) cmd_cd(tok[1]);
    else cmd_cd(NULL);
} else if (strcmp(tok[0], "lsdir") == 0) {
    cmd_lsdir();
} else if (strcmp(tok[0], "shell") == 0) {

    if (tokc >= 2) {
        cmd_shell(tokc - 1, &tok[1]);
    } else {
        cmd_shell(0, NULL);
    }
} else if (strcmp(tok[0], "netr") == 0) {
            if (tokc == 1) {
                emit_out("Usage: netr <options: wget,curl,ftp> <args>");
                emit_ok("netr usage");
            } else {
                /* pass arguments after the command name into cmd_netr_run */
                cmd_netr_run(tokc - 1, &tok[1]);
            }
        }else {
/* if a PTY-backed stream is active, forward this line as input to it
   (append newline so typical REPLs / cin/read calls get the line).
   HOWEVER: intercept some backend builtins even while a PTY shell is running:
   - cd [path]
   - pwd
   - lsdir
   These builtins act on the backend process and will emit OUT/OK/ERR as usual.
*/
pthread_mutex_lock(&stream_lock);
int running = stream_running && stream_master_fd >= 0;
pthread_mutex_unlock(&stream_lock);

if (running) {
    /* if user typed a backend builtin, run it in backend instead of forwarding */
    if (strcmp(tok[0], "cd") == 0) {
        if (tokc >= 2) cmd_cd(tok[1]);
        else cmd_cd(NULL);
    } else if (strcmp(tok[0], "pwd") == 0) {
        cmd_pwd();
    } else if (strcmp(tok[0], "lsdir") == 0) {
        cmd_lsdir();
    } else {
        /* not a builtin -> send into PTY child (interactive shell) */
        if (send_input_to_stream(line, strlen(line), 1) == 0) {
            emit_ok("stdin sent");
        } else {
            emit_err("no stream");
        }
    }
} else if(strcmp(tok[0],"lsmem") == 0){
    cmd_lsmem();
} else if (strcmp(tok[0], "proc") == 0) {
    if (tokc < 2) {
        emit_err("proc usage");
    } else if (strcmp(tok[1], "--list") == 0) {
        cmd_proc_list();
    } else if (strcmp(tok[1], "--kill") == 0) {
        if (tokc >= 3) cmd_proc_kill(tok[2]);
        else emit_err("proc --kill needs arg");
    } else if (strcmp(tok[1], "--start") == 0) {
        /* pass remaining tokens as argv to cmd_proc_start */
        int subc = tokc - 2;
        char **subv = NULL;
        if (subc > 0) {
            subv = &tok[2]; /* safe: tokenize memory remains until free_tokens called later */
        }
        cmd_proc_start(subc, subv);
    } else {
        emit_err("proc usage");
    }
}else if(strcmp(tok[0], "ls") == 0){
    cmd_ls();
} else if (strcmp(tok[0], "create") == 0) {
        if (tokc >= 2) {
            cmd_create(tok[1]);
        } else {
            emit_err("create usage: create <filename>");
        }
    }else if (strcmp(tok[0], "rm") == 0) {
        if (tokc >= 2) {
             cmd_rm(tokc - 1, &tok[1]);
        } else {
            emit_err("rm usage: rm <filename>");
        }
    } else if (strcmp(tok[0], "read") == 0) {
    cmd_read(tok[1]);
} else if (strcmp(tok[1],"cedit") == 0){
     if (tokc >= 2) {
            cmd_cedit(tokc - 1, &tok[1]);
        } else {
            emit_err("cedit usage: cedit <filename>");
        }
} else if (strcmp(tok[0], "mkdir") == 0) {
        cmd_mkdir(tokc - 1, &tok[1]);
    }else {
    /* no stream -> behave as before (echo into frontend) */
    emit_out(line);
    emit_ok("echo");
}
        }


        free_tokens(tok, tokc);
    } /* main loop */

    pthread_mutex_lock(&stream_lock);
    if (stream_running && stream_child_pid > 0) {
        pid_t pg = getpgid(stream_child_pid);
        if (pg > 0) kill(-pg, SIGTERM);
        else kill(stream_child_pid, SIGTERM);
        waitpid(stream_child_pid, NULL, 0);
    }
    pthread_mutex_unlock(&stream_lock);

    return 0;
}