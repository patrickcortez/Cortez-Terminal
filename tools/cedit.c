/* tools/cedit.c
 *
 * Minimal terminal text editor with:
 *  - cedit <filename>
 *  - arrow keys, insert, backspace, enter/newline
 *  - Ctrl-S to Save (opens a "Save? Enter to confirm, Esc to cancel" prompt)
 *  - Esc to request Quit (prompts if unsaved)
 *
 * No ncurses. Uses termios/raw mode and ANSI escapes.
 *
 * Build: gcc -O2 -std=c11 -Wall -Wextra -o cedit cedit.c
 * To statically link (if desired): add -static (may increase binary size).
 */

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <limits.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <sys/types.h>
#include <libgen.h>
#include <sys/stat.h>
#include <errno.h>

/* Control keys */
#define CTRL_KEY(k) ((k) & 0x1f)
enum keys {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

/* editor row */
typedef struct {
    char *chars;
    size_t len;
    size_t cap;
} erow;

/* editor state */
typedef struct {
    int cx, cy;          /* cursor x (col), y (row) */
    int rowoff, coloff;  /* offset for vertical/horizontal scrolling */
    int screenrows, screencols;
    erow *row;
    int numrows;
    int dirty;           /* change flag */
    char *filename;
    struct termios orig_termios;
} editor_t;

static editor_t E;

/* ------------ low-level terminal -------------- */

static void die(const char *s) {
    /* restore terminal and write error */
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

static void disable_raw_mode(void) {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios);
}

static void enable_raw_mode(void) {
    if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1) die("tcgetattr");
    atexit(disable_raw_mode);

    struct termios raw = E.orig_termios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1; /* 100ms timeout for read */

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1) die("tcsetattr");
}

/* read a key, return either ascii or special enum */
static int editor_read_key(void) {
    int nread;
    char c;
    while ((nread = read(STDIN_FILENO, &c, 1)) == 0);
    if (nread < 0) die("read");

    if (c == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                /* extended escape */
                char seq2;
                if (read(STDIN_FILENO, &seq2, 1) != 1) return '\x1b';
                if (seq2 == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    } else {
        return (unsigned char)c;
    }
}

/* get window size */
static int get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) return -1;
    *cols = ws.ws_col;
    *rows = ws.ws_row;
    return 0;
}

/* --------------- row helpers ------------------ */

static void erow_init(erow *r) {
    r->chars = NULL;
    r->len = 0;
    r->cap = 0;
}

static void erow_free(erow *r) {
    free(r->chars);
    r->chars = NULL;
    r->len = r->cap = 0;
}

static void erow_insert_char(erow *r, int at, int c) {
    if (at < 0) at = 0;
    if (at > (int)r->len) at = r->len;
    if (r->len + 1 >= r->cap) {
        r->cap = r->cap ? r->cap * 2 : 64;
        r->chars = realloc(r->chars, r->cap);
    }
    memmove(&r->chars[at + 1], &r->chars[at], r->len - at);
    r->chars[at] = (char)c;
    r->len++;
}

static void erow_append_string(erow *r, const char *s, size_t slen) {
    if (r->len + slen + 1 >= r->cap) {
        r->cap = r->len + slen + 16;
        r->chars = realloc(r->chars, r->cap);
    }
    memcpy(&r->chars[r->len], s, slen);
    r->len += slen;
}

static void erow_del_char(erow *r, int at) {
    if (at < 0 || at >= (int)r->len) return;
    memmove(&r->chars[at], &r->chars[at+1], r->len - at - 1);
    r->len--;
}

/* split a row at position (for Enter) */
static void erow_split(erow *r, int at, erow *newrow) {
    if (at < 0) at = 0;
    if (at > (int)r->len) at = r->len;
    erow_init(newrow);
    int right_len = r->len - at;
    if (right_len > 0) {
        newrow->chars = malloc(right_len + 1);
        memcpy(newrow->chars, &r->chars[at], right_len);
        newrow->len = right_len;
        newrow->cap = right_len + 1;
    }
    r->len = at;
}

/* --------------- buffer/file I/O ---------------- */

static void editor_open(const char *filename) {
    free(E.filename);
    /* Resolve ~ and relative paths into an absolute-ish path based on this process CWD. */
    char resolved[PATH_MAX];
    if (!filename) {
        E.filename = NULL;
    } else if (filename[0] == '/') {
        /* absolute already */
        E.filename = strdup(filename);
    } else if (filename[0] == '~') {
        const char *home = getenv("HOME");
        if (!home) {
            struct passwd *pw = getpwuid(getuid());
            home = pw ? pw->pw_dir : "";
        }
        snprintf(resolved, sizeof(resolved), "%s/%s", home, filename + 1);
        E.filename = strdup(resolved);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) cwd[0] = '\0';
        snprintf(resolved, sizeof(resolved), "%s/%s", cwd, filename);
        E.filename = strdup(resolved);
    }


    FILE *f = fopen(filename, "r");
    if (!f) {
        /* start empty (new file) */
        E.numrows = 0;
        E.row = NULL;
        return;
    }

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, f)) != -1) {
        /* strip newline */
        if (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) {
            while (linelen > 0 && (line[linelen - 1] == '\n' || line[linelen - 1] == '\r')) linelen--;
        }
        E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
        erow_init(&E.row[E.numrows]);
        if (linelen) erow_append_string(&E.row[E.numrows], line, linelen);
        E.numrows++;
    }
    free(line);
    fclose(f);
    E.dirty = 0;
}

/* Atomic save: write to a temp file in the target file's directory and rename() over it */
static int editor_save(void) {
    if (!E.filename) return -1;

    /* Duplicate filename because dirname() may modify the buffer. */
    char *pathdup = strdup(E.filename);
    if (!pathdup) return -1;
    char *dir = dirname(pathdup);
    if (!dir) { free(pathdup); return -1; }

    /* Build temp template in same directory */
    char tmp_template[PATH_MAX];
    /* Use a dot prefix to avoid showing temp files in dir listing */
    int n = snprintf(tmp_template, sizeof(tmp_template), "%s/.cedit.tmp.XXXXXX", dir);
    if (n < 0 || n >= (int)sizeof(tmp_template)) { free(pathdup); return -1; }

    int tmpfd = mkstemp(tmp_template);
    if (tmpfd == -1) { free(pathdup); return -1; }

    /* Write buffer lines to tmpfd */
    for (int i = 0; i < E.numrows; ++i) {
        if (E.row[i].len > 0) {
            ssize_t w = write(tmpfd, E.row[i].chars, E.row[i].len);
            if (w < 0) { close(tmpfd); unlink(tmp_template); free(pathdup); return -1; }
        }
        if (write(tmpfd, "\n", 1) != 1) { close(tmpfd); unlink(tmp_template); free(pathdup); return -1; }
    }

    /* flush and close */
    if (fsync(tmpfd) != 0) {
        /* non-fatal on some FS, but we attempt it */
    }
    close(tmpfd);

    /* rename over original atomically */
    if (rename(tmp_template, E.filename) != 0) {
        unlink(tmp_template);
        free(pathdup);
        return -1;
    }

    free(pathdup);
    E.dirty = 0;
    return 0;
}

/* insert new row at index */
static void editor_insert_row(int at, const char *s, size_t len) {
    if (at < 0 || at > E.numrows) return;
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    if (at < E.numrows) {
        memmove(&E.row[at+1], &E.row[at], sizeof(erow) * (E.numrows - at));
    }
    erow_init(&E.row[at]);
    if (len) erow_append_string(&E.row[at], s, len);
    E.numrows++;
}

/* delete row at index */
static void editor_del_row(int at) {
    if (at < 0 || at >= E.numrows) return;
    erow_free(&E.row[at]);
    if (at < E.numrows - 1) {
        memmove(&E.row[at], &E.row[at+1], sizeof(erow) * (E.numrows - at - 1));
    }
    E.numrows--;
    if (E.numrows == 0) {
        free(E.row);
        E.row = NULL;
    } else {
        E.row = realloc(E.row, sizeof(erow) * E.numrows);
    }
}

/* -------------- editing operations ---------------- */

static void editor_insert_char_at(int row, int col, int c) {
    if (row < 0 || row > E.numrows) return;
    if (row == E.numrows) {
        editor_insert_row(E.numrows, "", 0);
    }
    erow_insert_char(&E.row[row], col, c);
    E.dirty = 1;
}

static void editor_insert_newline(void) {
    if (E.cx == 0) {
        editor_insert_row(E.cy, "", 0);
    } else {
        erow newrow;
        erow_split(&E.row[E.cy], E.cx, &newrow);
        editor_insert_row(E.cy + 1, newrow.chars, newrow.len);
        erow_free(&newrow);
    }
    E.cy++;
    E.cx = 0;
    E.dirty = 1;
}

static void editor_del_char_at(int row, int col) {
    if (row < 0 || row >= E.numrows) return;
    if (col == 0 && row == 0) return;
    if (col > 0) {
        erow_del_char(&E.row[row], col - 1);
        E.cx--;
    } else {
        /* join this row with previous */
        int prev_len = E.row[row - 1].len;
        erow_append_string(&E.row[row - 1], E.row[row].chars, E.row[row].len);
        editor_del_row(row);
        E.cy--;
        E.cx = prev_len;
    }
    E.dirty = 1;
}

/* -------------- rendering ------------------- */

static void editor_scroll(void) {
    if (E.cy < E.rowoff) E.rowoff = E.cy;
    if (E.cy >= E.rowoff + E.screenrows) E.rowoff = E.cy - E.screenrows + 1;
    if (E.cx < E.coloff) E.coloff = E.cx;
    if (E.cx >= E.coloff + E.screencols) E.coloff = E.cx - E.screencols + 1;
}

static void editor_draw_rows(void) {
    for (int y = 0; y < E.screenrows; ++y) {
        int filerow = y + E.rowoff;
        if (filerow >= E.numrows) {
            /* show ~ on blank lines */
            if (E.numrows == 0 && y == E.screenrows / 3) {
                const char *welcome = "cedit -- simple editor";
                int wlen = (int)strlen(welcome);
                int pad = (E.screencols - wlen) / 2;
                if (pad < 0) pad = 0;
                if (pad) {
                    write(STDOUT_FILENO, "~", 1);
                    pad--;
                } else {
                    write(STDOUT_FILENO, "~", 1);
                }
                for (int i = 0; i < pad; ++i) write(STDOUT_FILENO, " ", 1);
                write(STDOUT_FILENO, welcome, wlen);
            } else {
                write(STDOUT_FILENO, "~", 1);
            }
        } else {
            erow *r = &E.row[filerow];
            int len = r->len - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            if (len > 0) {
                ssize_t w = write(STDOUT_FILENO, &r->chars[E.coloff], len);
                (void)w;
            }
        }
        /* clear line */
        write(STDOUT_FILENO, "\x1b[K", 3);
        if (y < E.screenrows - 1) write(STDOUT_FILENO, "\r\n", 2);
    }
}

/* status/message bar */
static void editor_draw_statusbar(const char *msg) {
    char status[256];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s",
                       E.filename ? E.filename : "[No Name]",
                       E.numrows, E.dirty ? "(modified)" : "");
    if (len < 0) len = 0;
    if (len > E.screencols) len = E.screencols;
    /* inverse colors */
    write(STDOUT_FILENO, "\x1b[7m", 4);
    write(STDOUT_FILENO, status, len);
    for (int i = len; i < E.screencols; ++i) write(STDOUT_FILENO, " ", 1);
    write(STDOUT_FILENO, "\x1b[m", 3);
}

/* message prompt area (last line) - we will block waiting for response if prompt */
static int editor_prompt_yesno(const char *prompt, char *response, size_t rcap) {
    /* render prompt on bottom line and wait for Enter/Esc or y/n */
    char buf[256];
    int pos = 0;
    while (1) {
        /* draw prompt */
        write(STDOUT_FILENO, "\x1b[s", 3);         /* save cursor */
        char line[512];
        snprintf(line, sizeof(line), "\x1b[%d;1H\x1b[K%s", E.screenrows + 1, prompt);
        write(STDOUT_FILENO, line, strlen(line));
        write(STDOUT_FILENO, "\x1b[u", 3);         /* restore cursor */

        int c = editor_read_key();
        if (c == '\r' || c == '\n') {
            if (pos < (int)rcap) buf[pos] = '\0';
            if (response) strncpy(response, buf, rcap);
            return 1;
        } else if (c == '\x1b') {
            return 0;
        } else if (c == 'y' || c == 'Y') {
            if (response) strncpy(response, "y", rcap);
            return 1;
        } else if (c == 'n' || c == 'N') {
            if (response) strncpy(response, "n", rcap);
            return 0;
        } else {
            /* ignore other keys in this simple prompt */
        }
    }
    return 0;
}

/* blocking prompt used for Save confirmation where we need explicit Enter */
static int editor_confirm_save(void) {
    /* prompt: Save file? (Enter to confirm, Esc to cancel) */
    while (1) {
        /* draw message on last line */
        char buf[256];
        snprintf(buf, sizeof(buf), "\x1b[%d;1H\x1b[KSave file? (Enter=Yes, Esc=Cancel)", E.screenrows + 1);
        write(STDOUT_FILENO, buf, strlen(buf));
        int c = editor_read_key();
        if (c == '\r' || c == '\n') return 1;
        if (c == '\x1b') return 0;
    }
}

/* main refresh screen */
static void editor_refresh_screen(void) {
    editor_scroll();
    char buf[64];
    /* hide cursor while drawing */
    write(STDOUT_FILENO, "\x1b[?25l", 6);
    /* reposition home */
    snprintf(buf, sizeof(buf), "\x1b[H");
    write(STDOUT_FILENO, buf, strlen(buf));

    editor_draw_rows();

    /* draw status bar */
    write(STDOUT_FILENO, "\x1b[K", 3);
    editor_draw_statusbar(NULL);

    /* position the cursor */
    int cx = E.cx - E.coloff + 1;
    int cy = E.cy - E.rowoff + 1;
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cy, cx);
    write(STDOUT_FILENO, buf, strlen(buf));

    /* show cursor */
    write(STDOUT_FILENO, "\x1b[?25h", 6);
}

/* -------------- input handling ---------------- */

static void editor_move_cursor(int key) {
    erow *row = (E.cy >= 0 && E.cy < E.numrows) ? &E.row[E.cy] : NULL;
    switch (key) {
        case ARROW_LEFT:
            if (E.cx > 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].len;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < (int)row->len) {
                E.cx++;
            } else if (row && E.cx == (int)row->len) {
                if (E.cy + 1 < E.numrows) { E.cy++; E.cx = 0; }
            }
            break;
        case ARROW_UP:
            if (E.cy > 0) E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy + 1 < E.numrows) E.cy++;
            break;
    }
    /* clamp cx to row length */
    row = (E.cy >= 0 && E.cy < E.numrows) ? &E.row[E.cy] : NULL;
    int rowlen = row ? (int)row->len : 0;
    if (E.cx > rowlen) E.cx = rowlen;
}

/* process a single key (returns 0 to continue, 1 to exit editor) */
static int editor_process_keypress(void) {
    int c = editor_read_key();
    if (c == CTRL_KEY('q')) {
        /* quick quit without prompt (rare). We'll instead ask user to press Esc to quit */
        return 0;
    } else if (c == CTRL_KEY('s')) {
        /* Save: prompt confirm (Enter=save, Esc=cancel) */
        if (editor_confirm_save()) {
            if (editor_save() == 0) {
                /* show saved message briefly */
                char msg[128];
                snprintf(msg, sizeof(msg), "\x1b[%d;1H\x1b[KSaved to %s", E.screenrows + 1, E.filename ? E.filename : "[No Name]");
                write(STDOUT_FILENO, msg, strlen(msg));
                usleep(400000); /* 400ms */
            } else {
                char msg[128];
                snprintf(msg, sizeof(msg), "\x1b[%d;1H\x1b[KSave failed: %s", E.screenrows + 1, strerror(errno));
                write(STDOUT_FILENO, msg, strlen(msg));
                usleep(600000);
            }
        }
        return 0;
    } else if (c == '\r' || c == '\n') {
        editor_insert_newline();
    } else if (c == DEL_KEY || c == 127 || c == CTRL_KEY('h')) {
        /* DEL or Backspace */
        if (c == DEL_KEY) {
            /* delete at cursor (right) */
            erow *r = (E.cy >= 0 && E.cy < E.numrows) ? &E.row[E.cy] : NULL;
            if (r && E.cx < (int)r->len) {
                erow_del_char(r, E.cx);
                E.dirty = 1;
            }
        } else {
            editor_del_char_at(E.cy, E.cx);
        }
    } else if (c == ARROW_UP || c == ARROW_DOWN || c == ARROW_LEFT || c == ARROW_RIGHT) {
        editor_move_cursor(c);
    } else if (c == PAGE_UP || c == PAGE_DOWN) {
        if (c == PAGE_UP) {
            E.cy = E.rowoff;
        } else {
            E.cy = E.rowoff + E.screenrows - 1;
            if (E.cy > E.numrows - 1) E.cy = E.numrows - 1;
        }
    } else if (c == HOME_KEY) {
        E.cx = 0;
    } else if (c == END_KEY) {
        if (E.cy < E.numrows) E.cx = E.row[E.cy].len;
    } else if (c == '\x1b') {
        /* Escape pressed: if it's alone (not arrow), treat as Quit request */
        /* Ask quit: if dirty, ask to save or quit without saving */
        if (E.dirty) {
            /* prompt: Save changes? y=save and quit, n=quit without saving, any other cancels */
            char choice[8] = {0};
            write(STDOUT_FILENO, "\x1b[s", 3);
            char s[128];
            snprintf(s, sizeof(s), "\x1b[%d;1H\x1b[KUnsaved changes. (s=save and quit, q=quit without saving, any other = cancel)", E.screenrows + 1);
            write(STDOUT_FILENO, s, strlen(s));
            write(STDOUT_FILENO, "\x1b[u", 3);
            int k = editor_read_key();
            if (k == 's' || k == 'S') {
                if (editor_save() == 0) return 1;
                else return 0;
            } else if (k == 'q' || k == 'Q') {
                return 1;
            } else {
                /* cancel */
                return 0;
            }
        } else {
            return 1; /* no unsaved changes -> exit immediately */
        }
    } else if (c >= 32 && c <= 126) {
        /* printable characters */
        editor_insert_char_at(E.cy, E.cx, c);
        E.cx++;
    }

    /* clamp and continue */
    if (E.cy > E.numrows) E.cy = E.numrows;
    if (E.cx < 0) E.cx = 0;
    return 0;
}

/* ------------------ init/cleanup ------------------- */

static void editor_init(void) {
    E.cx = E.cy = 0;
    E.rowoff = E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.filename = NULL;
    E.dirty = 0;
    if (get_window_size(&E.screenrows, &E.screencols) == -1) {
        E.screenrows = 24;
        E.screencols = 80;
    }
    /* reserve 1 row for status/prompt */
    E.screenrows -= 1;
}

/* --------------- main ---------------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: cedit <filename>\n");
        return 1;
    }

    editor_init();
    enable_raw_mode();

    editor_open(argv[1]);
    /* ensure at least one empty row so cursor has somewhere to go */
    if (E.numrows == 0) {
        editor_insert_row(0, "", 0);
    }

    while (1) {
        /* render */
        /* clear screen and reposition */
        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        editor_refresh_screen();
        /* handle input */
        if (editor_process_keypress()) break;
    }

    /* cleanup: restore terminal and move cursor to bottom */
    disable_raw_mode();
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    /* free memory */
    if (E.row) {
        for (int i = 0; i < E.numrows; ++i) erow_free(&E.row[i]);
        free(E.row);
    }
    free(E.filename);
    return 0;
}
