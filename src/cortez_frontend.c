// src/cortez_frontend_crt.c

#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <math.h>
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <time.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_ttf.h>


#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

/* --- ASCII art (Loading Screen) --- */
static const char *ART_LINES[] = {
"                  ",
"          _____                   _______                   _____                _____                    _____                    _____          ",
"         /\\    \\                 /::\\    \\                 /\\    \\              /\\    \\                  /\\    \\                  /\\    \\         ",
"        /::\\    \\               /::::\\    \\               /::\\    \\            /::\\    \\                /::\\    \\                /::\\    \\        ",
"       /::::\\    \\             /::::::\\    \\             /::::\\    \\           \\:::\\    \\              /::::\\    \\               \\:::\\    \\       ",
"      /::::::\\    \\           /::::::::\\    \\           /::::::\\    \\           \\:::\\    \\            /::::::\\    \\               \\:::\\    \\      ",
"     /:::/\\:::\\    \\         /:::/~~\\:::\\    \\         /:::/\\:::\\    \\           \\:::\\    \\          /:::/\\:::\\    \\               \\:::\\    \\     ",
"    /:::/  \\:::\\    \\       /:::/    \\:::\\    \\       /:::/__\\:::\\    \\           \\:::\\    \\        /:::/__\\:::\\    \\               \\:::\\    \\    ",
"   /:::/    \\:::\\    \\     /:::/    / \\:::\\    \\     /::::\\   \\:::\\    \\          /::::\\    \\      /::::\\   \\:::\\    \\               \\:::\\    \\   ",
"  /:::/    / \\:::\\    \\   /:::/____/   \\:::\\____\\   /::::::\\   \\:::\\    \\        /::::::\\    \\    /::::::\\   \\:::\\    \\               \\:::\\    \\  ",
" /:::/    /   \\:::\\    \\ |:::|    |     |:::|    | /:::/\\:::\\   \\:::\\____\\      /:::/\\:::\\    \\  /:::/\\:::\\   \\:::\\    \\               \\:::\\    \\ ",
"/:::/____/     \\:::\\____\\|:::|____|     |:::|    |/:::/  \\:::\\   \\:::|    |    /:::/  \\:::\\____\\/:::/__\\:::\\   \\:::\\____\\_______________\\:::\\____| ",
"\\:::\\    \\      \\::/    / \\:::\\    \\   /:::/    / \\::/   |::::\\  /:::|____|   /:::/    \\::/    /\\:::\\   \\:::\\   \\::/    /\\::::::::::::::::::/    / ",
" \\:::\\    \\      \\/____/   \\:::\\    \\ /:::/    /   \\/____|:::::\\/:::/    /   /:::/    / \\/____/  \\:::\\   \\:::\\   \\/____/  \\::::::::::::::::/____/  ",
"  \\:::\\    \\                \\:::\\    /:::/    /          |:::::::::/    /   /:::/    /            \\:::\\   \\:::\\    \\       \\:::\\~~~~\\~~~~~~       ",
"   \\:::\\    \\                \\:::\\__/:::/    /           |::|\\::::/    /   /:::/    /              \\:::\\   \\:::\\____\\       \\:::\\    \\            ",
"    \\:::\\    \\                \\::::::::/    /            |::| \\::/____/    \\::/    /                \\:::\\   \\::/    /        \\:::\\    \\           ",
"     \\:::\\    \\                \\::::::/    /             |::|  ~|           \\/____/                  \\:::\\   \\/____/          \\:::\\    \\          ",
"      \\:::\\    \\                \\::::/    /              |::|   |                                     \\:::\\    \\               \\:::\\    \\         ",
"       \\:::\\____\\                \\::/____/               \\::|   |                                      \\:::\\____\\               \\:::\\____\\        ",
"        \\::/    /                 ~~                      \\:|   |                                       \\::/    /                \\::/    /        ",
"         \\/____/                                           \\|___|                                        \\/____/                  \\/____/         ",
NULL
};

#define FONT_SIZE 18
#define INPUT_HEIGHT 34
#define LINE_SPACING 6
SDL_Texture *corner_shadow = NULL;
SDL_Texture *side_glow_left = NULL;
SDL_Texture *side_glow_right = NULL;
int corner_sz = 160; 
int side_glow_w = 96; /* width of glow band on each side */


/* base64 helpers */
static int b64_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static size_t b64_decode(const char *in, unsigned char *out, size_t outcap) {
    size_t len = strlen(in), i=0,o=0;
    while (i < len) {
        int v[4] = {-1,-1,-1,-1};
        int k;
        for (k=0;k<4 && i < len;k++) {
            char c = in[i++];
            if (c=='=') { v[k] = -2; continue; }
            int val = b64_val(c);
            if (val < 0) { k--; continue; }
            v[k]=val;
        }
        if (v[0] < 0) break;
        uint32_t triple = ((v[0]&0x3F)<<18)|((v[1]&0x3F)<<12)|((v[2]&0x3F)<<6)|(v[3]&0x3F);
        if (v[2]==-2) { if (o+1<=outcap) out[o++]=(triple>>16)&0xFF; break; }
        else if (v[3]==-2) { if (o+2<=outcap) { out[o++]=(triple>>16)&0xFF; out[o++]=(triple>>8)&0xFF; } break; }
        else { if (o+3<=outcap) { out[o++]=(triple>>16)&0xFF; out[o++]=(triple>>8)&0xFF; out[o++]=triple&0xFF; } else break; }
    }
    return o;
}

/* strip common ANSI escape (CSI) sequences from a string in-place.
   Removes ESC [ ... <final> sequences and also drops stray \r characters. */
static void strip_ansi_sequences(char *s) {
    if (!s) return;
    char *dst = s;
    char *src = s;
    while (*src) {
        if (*src == '\x1b') {
            /* skip ESC */
            src++;
            /* if next is '[' it's a CSI sequence: skip until final byte (@-~) */
            if (*src == '[') {
                src++;
                while (*src && !(*src >= '@' && *src <= '~')) src++;
                if (*src) src++; /* consume final byte */
            } else {
                /* other ESC sequences: skip until a final byte */
                while (*src && !(*src >= '@' && *src <= '~')) src++;
                if (*src) src++;
            }
            continue;
        }
        /* drop carriage returns injected by PTY (we'll rely on '\n' for line breaks) */
        if (*src == '\r') { src++; continue; }
        *dst++ = *src++;
    }
    *dst = '\0';
}


/* small dynamic line buffer */
typedef struct { char **lines; int count; int cap; } LineBuf;
static void lb_init(LineBuf *lb) { lb->lines = NULL; lb->count = 0; lb->cap = 0; }

static void lb_clear(LineBuf *lb) {
    if (!lb) return;
    for (int i = 0; i < lb->count; ++i) {
        free(lb->lines[i]);
    }
    lb->count = 0;
    // keep allocated array around for reuse (avoid free/realloc churn)
}


static void lb_push(LineBuf *lb, const char *s) {
    if (!s) return;
    if (lb->count + 1 >= lb->cap) { lb->cap = lb->cap ? lb->cap * 2 : 256; lb->lines = realloc(lb->lines, lb->cap * sizeof(char*)); }
    lb->lines[lb->count++] = strdup(s);
    while (lb->count > 4000) { free(lb->lines[0]); memmove(&lb->lines[0], &lb->lines[1], (lb->count-1)*sizeof(char*)); lb->count--; }
}
static void lb_free(LineBuf *lb) { for (int i=0;i<lb->count;i++) free(lb->lines[i]); free(lb->lines); }

/* push a long text into lb with wrapping to visible columns */
static void lb_push_wrapped(LineBuf *lb, const char *s, int maxcols) {
    if (!s) return;
    size_t L = strlen(s);
    if (L == 0) { lb_push(lb, ""); return; }
    const char *p = s;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t chunklen = nl ? (size_t)(nl - p) : strlen(p);
        if ((int)chunklen <= maxcols) {
            char *tmp = malloc(chunklen + 1);
            if (!tmp) return;
            memcpy(tmp, p, chunklen); tmp[chunklen]=0;
            lb_push(lb, tmp);
            free(tmp);
        } else {
            size_t remaining = chunklen;
            const char *q = p;
            while (remaining > 0) {
                size_t take = remaining > (size_t)maxcols ? (size_t)maxcols : remaining;
                size_t used = take;
                if (take == (size_t)maxcols) {
                    size_t i;
                    for (i = take; i > 0; --i) {
                        if (q[i-1] == ' ' || q[i-1] == '\t') { used = i; break; }
                    }
                    if (used == 0) used = take;
                }
                char *tmp = malloc(used + 1);
                if (!tmp) return;
                memcpy(tmp, q, used); tmp[used]=0;
                lb_push(lb, tmp);
                free(tmp);
                q += used;
                while (*q == ' ' || *q == '\t') { q++; remaining--; }
                remaining = chunklen - (size_t)(q - p);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
}

/* Create a soft circular corner shadow texture of size s x s.
   Black->transparent radial gradient, used at each corner.
*/
static SDL_Texture *create_corner_shadow(SDL_Renderer *rend, int s, int max_alpha)
{
    if (s <= 0) return NULL;
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, s, s, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return NULL;
    Uint32 *pix = (Uint32*)surf->pixels;
    float cx = (s-1)*0.0f; /* we'll use distance from corner (0,0) */
    float cy = (s-1)*0.0f;
    float maxd = sqrtf((float)(s*s) + (float)(s*s));
    for (int y = 0; y < s; ++y) {
        for (int x = 0; x < s; ++x) {
            float dx = (float)(s-1 - x); /* distance from bottom-right of this small quad when we flip */
            float dy = (float)(s-1 - y);
            /* distance from the corner (we'll flip copies so this formula OK) */
            float d = sqrtf((float)(x*x + y*y));
            float t = d / (float)s; /* 0..~1 */
            float alphaf = 0.0f;
            /* A quick falloff curve for a soft shadow */
            if (t < 0.95f) alphaf = (1.0f - powf(t, 2.4f)) * (float)max_alpha;
            else alphaf = 0.0f;
            if (alphaf < 0.0f) alphaf = 0.0f;
            if (alphaf > 255.0f) alphaf = 255.0f;
            Uint8 a = (Uint8)alphaf;
            Uint32 col = SDL_MapRGBA(surf->format, 0, 0, 0, a);
            pix[y * (surf->pitch/4) + x] = col;
        }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surf);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(surf);
    return tex;
}

/* Create a vertical side glow texture (w x h) that fades horizontally.
   If left==1 create left->right fade; if left==0 create right->left fade (same content may be flipped).
*/
static SDL_Texture *create_side_glow(SDL_Renderer *rend, int w, int h, int max_green, int left)
{
    if (w <= 0 || h <= 0) return NULL;
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return NULL;
    Uint32 *pix = (Uint32*)surf->pixels;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            int px = left ? x : (w - 1 - x);
            float t = (float)px / (float)(w - 1); /* 0 at inner edge -> 1 at outer edge */
            /* We want a small bright band near inner edge, fall off quickly */
            float alphaf = (1.0f - powf(t, 1.8f)) * 0.55f; /* scale down so glow is faint */
            if (alphaf < 0.0f) alphaf = 0.0f;
            if (alphaf > 1.0f) alphaf = 1.0f;
            Uint8 a = (Uint8)(alphaf * 200.0f); /* max ~200 */
            /* grazing green tint (low red/blue) */
            Uint8 r = 8;
            Uint8 g = (Uint8) ( (float)max_green * (0.6f + 0.4f * (1.0f - t)) ); /* mild variation */
            Uint8 b = 8;
            Uint32 col = SDL_MapRGBA(surf->format, r, g, b, a);
            pix[y * (surf->pitch/4) + x] = col;
        }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surf);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_ADD); /* additive for glow */
    SDL_FreeSurface(surf);
    return tex;
}


/* spawn backend (same as yours) */
static pid_t spawn_backend(const char *path, int *wfd, int *rfd, int *efd) {
    int to_child[2], from_child[2], err_pipe[2];
    if (pipe(to_child) < 0) return -1;
    if (pipe(from_child) < 0) { close(to_child[0]); close(to_child[1]); return -1; }
    if (pipe(err_pipe) < 0) { close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]); return -1; }
    pid_t pid = fork();
    if (pid < 0) { close(to_child[0]); close(to_child[1]); close(from_child[0]); close(from_child[1]); close(err_pipe[0]); close(err_pipe[1]); return -1; }
    if (pid == 0) {
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(to_child[0]); close(to_child[1]);
        close(from_child[0]); close(from_child[1]);
        close(err_pipe[0]); close(err_pipe[1]);
        execl(path, path, (char*)NULL);
        _exit(127);
    }
    close(to_child[0]); close(from_child[1]); close(err_pipe[1]);
    *wfd = to_child[1]; *rfd = from_child[0]; *efd = err_pipe[0];
    fcntl(*rfd, F_SETFL, O_NONBLOCK);
    fcntl(*efd, F_SETFL, O_NONBLOCK);
    return pid;
}
static int backend_write_line(int wfd, const char *line) {
    size_t L = strlen(line);
    if (write(wfd, line, L) != (ssize_t)L) return -1;
    if (write(wfd, "\n", 1) != 1) return -1;
    return 0;
}

/* --- create radial vignette texture (RGBA, black fading toward edges) --- */
static SDL_Texture *create_vignette(SDL_Renderer *rend, int w, int h) {
    if (w <= 0 || h <= 0) return NULL;
    SDL_Surface *surf = SDL_CreateRGBSurfaceWithFormat(0, w, h, 32, SDL_PIXELFORMAT_RGBA32);
    if (!surf) return NULL;
    Uint32 *pixels = (Uint32*)surf->pixels;
    float cx = (w-1) * 0.5f, cy = (h-1) * 0.5f;
    float maxd = sqrtf(cx*cx + cy*cy);
    for (int y=0;y<h;++y) {
        for (int x=0;x<w;++x) {
            float dx = (x - cx);
            float dy = (y - cy);
            float d = sqrtf(dx*dx + dy*dy) / maxd; // 0..1
            float a = 0.0f;
            if (d > 0.45f) {
                float t = (d - 0.45f) / (1.0f - 0.45f);
                a = powf(t, 1.6f) * 220.0f; // 0..220
            } else {
                a = 0.0f;
            }
            if (a > 255.0f) a = 255.0f;
            Uint8 aa = (Uint8)a;
            Uint32 col = SDL_MapRGBA(surf->format, 0, 0, 0, aa);
            pixels[y * (surf->pitch/4) + x] = col;
        }
    }
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surf);
    if (tex) SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_FreeSurface(surf);
    return tex;
}

/* Draw a simple bevelled frame (outer border) */
static void draw_frame(SDL_Renderer *r, int x, int y, int w, int h) {
    // outer rim
    SDL_Rect rect = { x-10, y-10, w+20, h+20 };
    SDL_SetRenderDrawColor(r, 40, 32, 28, 255);
    SDL_RenderFillRect(r, &rect);
    // inner bevel layers
    SDL_Rect r1 = { x-6, y-6, w+12, h+12 };
    SDL_SetRenderDrawColor(r, 70, 58, 48, 255);
    SDL_RenderFillRect(r, &r1);
    SDL_Rect r2 = { x-3, y-3, w+6, h+6 };
    SDL_SetRenderDrawColor(r, 30, 20, 18, 255);
    SDL_RenderFillRect(r, &r2);
    // decorative inner rim
    SDL_Rect rim = { x-1, y-1, w+2, h+2 };
    SDL_SetRenderDrawColor(r, 10, 6, 4, 255);
    SDL_RenderDrawRect(r, &rim);
}

static void draw_filled_circle(SDL_Renderer *rend, int cx, int cy, int radius,
                               Uint8 r, Uint8 g, Uint8 b, Uint8 a)
{
    if (radius <= 0) return;
    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(rend, r, g, b, a);
    for (int dy = -radius; dy <= radius; ++dy) {
        int dx = (int)floor(sqrt((double)radius*radius - (double)dy*dy));
        SDL_RenderDrawLine(rend, cx - dx, cy + dy, cx + dx, cy + dy);
    }
}

/*----------------LED Helpers--------------*/

static void draw_led(SDL_Renderer *rend, int cx, int cy, int r,
                     Uint8 rr, Uint8 gg, Uint8 bb, int on)
{
    /* bezel / outer rim */
    draw_filled_circle(rend, cx, cy, r + 3,  36, 36, 36, 255);  /* bezel */
    draw_filled_circle(rend, cx, cy, r + 1,  12, 12, 12, 255);  /* inner rim */

    if (on) {
        /* soft glow */
        draw_filled_circle(rend, cx, cy, r * 2, rr, gg, bb, 60);
        /* bright core */
        draw_filled_circle(rend, cx, cy, r, rr, gg, bb, 220);
        /* small white highlight (upper-left) */
        draw_filled_circle(rend, cx - r/3, cy - r/3, r/3, 255, 255, 255, 200);
    } else {
        /* dark (off) core and a subtle specular */
        draw_filled_circle(rend, cx, cy, r, 24, 24, 24, 255);
        draw_filled_circle(rend, cx - r/3, cy - r/3, r/3, 80, 80, 80, 160);
    }
}
/* ----------------- end LED helpers ----------------- */

/* draw scanlines across given rect */
static void draw_scanlines(SDL_Renderer *r, int x, int y, int w, int h, int gap, int alpha) {
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    for (int yy = y; yy < y + h; yy += gap) {
        SDL_SetRenderDrawColor(r, 0, 0, 0, alpha); // subtle dark lines
        SDL_RenderDrawLine(r, x, yy, x + w, yy);
    }
}

/* draw a soft inner glow behind text: we approximate by drawing the same texture several times offset with low alpha */
static void render_text_with_glow(SDL_Renderer *rend, TTF_Font *font, const char *text, int px, int py) {
    if (!text || !*text) return;
    SDL_Color glowCol = {70, 255, 70, 80}; // green glow
    SDL_Color fg = {51, 255, 51, 255};

    SDL_Surface *surf = TTF_RenderUTF8_Blended(font, text, fg);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, surf);
    SDL_Texture *tex_glow = SDL_CreateTextureFromSurface(rend, surf); // reuse for glow passes
    SDL_FreeSurface(surf);
    if (!tex) { if (tex_glow) SDL_DestroyTexture(tex_glow); return; }
    SDL_SetTextureBlendMode(tex_glow, SDL_BLENDMODE_ADD);
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    int tw, th;
    SDL_QueryTexture(tex, NULL, NULL, &tw, &th);

    // glow passes: offsets with low alpha (additive)
    const int pass_offsets[6][2] = {{-2,0},{2,0},{0,-2},{0,2},{-1,-1},{1,1}};
    for (int i=0;i<6;++i) {
        SDL_Rect dst = { px + pass_offsets[i][0], py + pass_offsets[i][1], tw, th };
        SDL_SetTextureColorMod(tex_glow, glowCol.r, glowCol.g, glowCol.b);
        SDL_SetTextureAlphaMod(tex_glow, 60);
        SDL_RenderCopy(rend, tex_glow, NULL, &dst);
    }

    // final crisp text
    SDL_Rect final = { px, py, tw, th };
    SDL_RenderCopy(rend, tex, NULL, &final);

    SDL_DestroyTexture(tex_glow);
    SDL_DestroyTexture(tex);
}

static void render_worn_sticker(SDL_Renderer *rend, TTF_Font *font, const char *text, int px, int py)
{
    if (!text || !*text || !font) return;

    /* text color (dark/worn brown for contrast) */
    SDL_Color textCol = { 48, 30, 18, 255 };

    SDL_Surface *s = TTF_RenderUTF8_Blended(font, text, textCol);
    if (!s) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(rend, s);
    int tw = s->w, th = s->h;
    SDL_FreeSurface(s);
    if (!tex) return;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);

    /* DULL ORANGE BACKGROUND for sticker */
    SDL_Rect bg = { px - 8, py - 4, tw + 16, th + 8 };

    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
    /* base backing: dull orange */
    SDL_SetRenderDrawColor(rend, 160, 100, 50, 200);  /* dull orange, semi-transparent */
    SDL_RenderFillRect(rend, &bg);
    /* worn rim (muted brown/orange) */
    SDL_SetRenderDrawColor(rend, 120, 75, 45, 90);
    SDL_RenderDrawRect(rend, &bg);

    /* dark under-pass (gives partial missing-paint look) */
    SDL_SetTextureColorMod(tex, 40, 40, 40);
    SDL_SetTextureAlphaMod(tex, 90);
    SDL_Rect d1 = { px - 1, py + 1, tw, th };
    SDL_RenderCopy(rend, tex, NULL, &d1);

    /* main faded text (desaturated to match worn look) */
    SDL_SetTextureColorMod(tex, 200, 170, 130);
    SDL_SetTextureAlphaMod(tex, 210);
    SDL_Rect dst = { px, py, tw, th };
    SDL_RenderCopy(rend, tex, NULL, &dst);

    /* faint highlight pass */
    SDL_SetTextureColorMod(tex, 230, 220, 200);
    SDL_SetTextureAlphaMod(tex, 36);
    SDL_Rect d2 = { px + 1, py - 1, tw, th };
    SDL_RenderCopy(rend, tex, NULL, &d2);

    /* small deterministic scrape lines across sticker to add wear */
    SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
    for (int i = 0; i < 5; ++i) {
        int sy = bg.y + 4 + (i * (bg.h - 8) / 5);
        int sx0 = bg.x + 4 + ((i * 7) & 0x1F);
        int sx1 = bg.x + bg.w - 6 - ((i * 13) & 0x1F);
        if (sx1 <= sx0) continue;
        SDL_SetRenderDrawColor(rend, 0, 0, 0, 40); /* subtle darker scrape */
        SDL_RenderDrawLine(rend, sx0, sy, sx1, sy);
        if ((i & 1) == 0) {
            SDL_SetRenderDrawColor(rend, 255, 255, 255, 14);
            SDL_RenderDrawLine(rend, sx0 + 2, sy - 1, sx1 - 2, sy - 1);
        }
    }

    SDL_DestroyTexture(tex);
}

/* safer atomic save: create temp file in same dir using strrchr instead of dirname/basename */
static int atomic_save_lines(const char *abs_path, LineBuf *buf)
{
    if (!abs_path || !buf) return -1;

    /* find directory part */
    const char *slash = strrchr(abs_path, '/');
    char dir[PATH_MAX];
    char tmp_template[PATH_MAX];

    if (slash == NULL) {
        /* no slash -> use current directory */
        if (snprintf(dir, sizeof(dir), ".") >= (int)sizeof(dir)) return -1;
    } else {
        size_t dlen = (size_t)(slash - abs_path);
        if (dlen >= sizeof(dir)) return -1;
        memcpy(dir, abs_path, dlen);
        dir[dlen] = '\0';
    }

    /* build temp template "<dir>/.<basename>.tmpXXXXXX" */
    const char *basename = (slash ? slash + 1 : abs_path);
    if (snprintf(tmp_template, sizeof(tmp_template), "%s/.%s.tmpXXXXXX", dir, basename) >= (int)sizeof(tmp_template))
        return -1;

    /* mkstemp requires a mutable buffer */
    int fd = mkstemp(tmp_template);
    if (fd < 0) {
        /* fallback: try direct write to target (best-effort) */
        FILE *f = fopen(abs_path, "wb");
        if (!f) return -1;
        for (int i = 0; i < buf->count; ++i) {
            if (fwrite(buf->lines[i], 1, strlen(buf->lines[i]), f) != strlen(buf->lines[i])) { fclose(f); return -1; }
            if (i < buf->count - 1) fputc('\n', f);
        }
        fclose(f);
        return 0;
    }

    FILE *tf = fdopen(fd, "wb");
    if (!tf) { close(fd); unlink(tmp_template); return -1; }

    /* write lines */
    for (int i = 0; i < buf->count; ++i) {
        size_t w = fwrite(buf->lines[i], 1, strlen(buf->lines[i]), tf);
        if (w != strlen(buf->lines[i])) { fclose(tf); unlink(tmp_template); return -1; }
        if (i < buf->count - 1) fputc('\n', tf);
    }
    fflush(tf);

    /* ensure data hits disk */
    if (fsync(fd) != 0) {
        /* ignore fsync failure, but attempt rename anyway */
    }

    fclose(tf); /* closes fd */

    /* now atomically replace target */
    if (rename(tmp_template, abs_path) != 0) {
        unlink(tmp_template);
        return -1;
    }

    return 0;
}


/* ---------- improved frontend editor (atomic save + save prompt) ----------
   Ctrl+S -> show prompt "Save changes to <file>? (Y/n)".
   Y or Enter saves (atomic), N cancels.
   Esc exits editor (no prompt). */
static void run_simple_editor(const char *filename, SDL_Renderer *rend, TTF_Font *font,
                              int screen_x, int screen_y, int screen_w, int screen_h, const char *backend_cwd,
                              int backend_running)
{

        /* --- CRT composition resources for the editor --- */
    SDL_Texture *screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888,
                                                SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16);
    if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND);
    SDL_Texture *vignette = create_vignette(rend, screen_w - 16, screen_h - 16);

    if (!filename || !*filename) return;

    /* resolve filename to absolute path (so saving goes where frontend expects) */
char abs_path[PATH_MAX];
if (filename[0] == '/') {
    strncpy(abs_path, filename, PATH_MAX-1);
    abs_path[PATH_MAX-1] = 0;
} else {
    snprintf(abs_path, sizeof(abs_path), "%s/%s", backend_cwd, filename);
}
    LineBuf ed; lb_init(&ed);

    /* load file lines into ed */
    FILE *f = fopen(abs_path, "rb");
    if (f) {
        char *line = NULL;
        size_t cap = 0;
        ssize_t n;
        while ((n = getline(&line, &cap, f)) >= 0) {
            while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) { line[n-1] = 0; --n; }
            lb_push(&ed, line);
        }
        free(line);
        fclose(f);
    } else {
        lb_push(&ed, "");
    }

    int cur_r = 0, cur_c = 0;
    if (ed.count == 0) lb_push(&ed, "");
    if (cur_r >= ed.count) cur_r = ed.count - 1;
    if (cur_c > (int)strlen(ed.lines[cur_r])) cur_c = strlen(ed.lines[cur_r]);

    SDL_StartTextInput();

    int editing = 1;
    int show_save_prompt = 0; /* 0=none, 1=prompt visible */
    int line_h = FONT_SIZE + LINE_SPACING;
    int inset_x = 18, inset_y = 18;
    int avail_h = (screen_h - 16) - (inset_y * 2) - INPUT_HEIGHT - 16;
    int visible_rows = avail_h / line_h;
    if (visible_rows < 3) visible_rows = 3;
    int scroll_top = 0;

    /* transient status message after save */
    char status_msg[256] = "";
    Uint32 status_until = 0; // SDL_GetTicks() timestamp until status visible
    

    while (editing) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { editing = 0; break; }
            else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                SDL_GetWindowSize(SDL_GetWindowFromID(ev.window.windowID), &screen_w, &screen_h);
                avail_h = (screen_h - 16) - (inset_y * 2) - INPUT_HEIGHT - 16;
                visible_rows = avail_h / line_h; if (visible_rows < 3) visible_rows = 3;
                if (screen_tex) { SDL_DestroyTexture(screen_tex); screen_tex = NULL; }
                if (screen_w - 16 > 0 && screen_h - 16 > 0) {
                    screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16);
                    if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND);
                }
                if (vignette) { SDL_DestroyTexture(vignette); vignette = NULL; }
                vignette = create_vignette(rend, screen_w - 16, screen_h - 16);

            } else if (ev.type == SDL_TEXTINPUT && !show_save_prompt) {
                const char *txt = ev.text.text;
                char *ln = ed.lines[cur_r];
                size_t len = strlen(ln);
                size_t add = strlen(txt);
                char *newln = malloc(len + add + 1);
                if (!newln) continue;
                memcpy(newln, ln, cur_c);
                memcpy(newln + cur_c, txt, add);
                memcpy(newln + cur_c + add, ln + cur_c, len - cur_c + 1);
                free(ed.lines[cur_r]);
                ed.lines[cur_r] = newln;
                cur_c += add;
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                int mods = ev.key.keysym.mod;

                if (show_save_prompt) {
                    /* handle prompt choices: Y/Enter => save, N => cancel */
                    if (k == SDLK_y || k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                        int res = atomic_save_lines(abs_path, &ed);
                        if (res == 0) {
                            snprintf(status_msg, sizeof(status_msg), "Wrote %d lines to %s", ed.count, abs_path);
                        } else {
                            snprintf(status_msg, sizeof(status_msg), "Save failed: %s", strerror(errno));
                        }
                        status_until = SDL_GetTicks() + 1500; /* show for 1.5s */
                        show_save_prompt = 0;
                    } else if (k == SDLK_n || k == SDLK_ESCAPE) {
                        /* cancel prompt */
                        show_save_prompt = 0;
                    }
                } else {
                    /* normal editor key handling */
                    if ((mods & KMOD_CTRL) && k == SDLK_s) { /* Ctrl-S: show save prompt */
                        show_save_prompt = 1;
                    } else if (k == SDLK_ESCAPE) {
                        editing = 0; break;
                    } else if (k == SDLK_BACKSPACE) {
                        if (cur_c > 0) {
                            char *ln = ed.lines[cur_r];
                            size_t len = strlen(ln);
                            memmove(ln + cur_c - 1, ln + cur_c, len - cur_c + 1);
                            cur_c--;
                        } else if (cur_r > 0) {
                            int prev = cur_r - 1;
                            size_t l0 = strlen(ed.lines[prev]);
                            size_t l1 = strlen(ed.lines[cur_r]);
                            char *newln = malloc(l0 + l1 + 1);
                            if (newln) {
                                memcpy(newln, ed.lines[prev], l0);
                                memcpy(newln + l0, ed.lines[cur_r], l1 + 1);
                                free(ed.lines[prev]);
                                ed.lines[prev] = newln;
                            }
                            free(ed.lines[cur_r]);
                            memmove(&ed.lines[cur_r], &ed.lines[cur_r+1], (ed.count - cur_r - 1) * sizeof(char*));
                            ed.count--;
                            cur_r = prev;
                            cur_c = l0;
                        }
                    } else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                        char *ln = ed.lines[cur_r];
                        size_t len = strlen(ln);
                        char *left = malloc(cur_c + 1);
                        char *right = malloc(len - cur_c + 1);
                        if (left && right) {
                            memcpy(left, ln, cur_c); left[cur_c] = 0;
                            memcpy(right, ln + cur_c, len - cur_c + 1);
                            free(ed.lines[cur_r]);
                            ed.lines[cur_r] = left;
                            if (ed.count + 1 >= ed.cap) { ed.cap = ed.cap ? ed.cap * 2 : 16; ed.lines = realloc(ed.lines, ed.cap * sizeof(char*)); }
                            memmove(&ed.lines[cur_r+2], &ed.lines[cur_r+1], (ed.count - cur_r - 1) * sizeof(char*));
                            ed.lines[cur_r+1] = right;
                            ed.count++;
                            cur_r++;
                            cur_c = 0;
                        } else { free(left); free(right); }
                    } else if (k == SDLK_UP) {
                        if (cur_r > 0) { cur_r--; if (cur_c > (int)strlen(ed.lines[cur_r])) cur_c = strlen(ed.lines[cur_r]); }
                        if (cur_r < scroll_top) scroll_top = cur_r;
                    } else if (k == SDLK_DOWN) {
                        if (cur_r + 1 < ed.count) { cur_r++; if (cur_c > (int)strlen(ed.lines[cur_r])) cur_c = strlen(ed.lines[cur_r]); }
                        if (cur_r >= scroll_top + visible_rows) scroll_top = cur_r - visible_rows + 1;
                    } else if (k == SDLK_LEFT) {
                        if (cur_c > 0) cur_c--;
                        else if (cur_r > 0) { cur_r--; cur_c = strlen(ed.lines[cur_r]); if (cur_r < scroll_top) scroll_top = cur_r; }
                    } else if (k == SDLK_RIGHT) {
                        if (cur_c < (int)strlen(ed.lines[cur_r])) cur_c++;
                        else if (cur_r + 1 < ed.count) { cur_r++; cur_c = 0; if (cur_r >= scroll_top + visible_rows) scroll_top = cur_r - visible_rows + 1; }
                    }
                }
            }
        }

         /* ---------------- rendering (editor -> screen_tex -> composite) ---------------- */
        /* render editor content into the offscreen CRT texture first */
        if (screen_tex) SDL_SetRenderTarget(rend, screen_tex);

        // inner CRT background (draw into render target)
        SDL_SetRenderDrawColor(rend, 6, 18, 6, 255);
        SDL_RenderClear(rend);

        // draw buffered editor lines with regular TTF (no glow to keep editor text readable)
        int e_line_h = FONT_SIZE + LINE_SPACING;
        int e_inset_x = 18;
        int e_inset_y = 18;
        int e_avail_h = (screen_h - 16) - (e_inset_y * 2) - INPUT_HEIGHT - 16;
        int e_max_visible = e_avail_h / e_line_h;
        if (e_max_visible < 1) e_max_visible = 1;
        int e_start = scroll_top;
        int e_end = e_start + visible_rows; if (e_end > ed.count) e_end = ed.count;
        int ey = e_inset_y;
for (int r = e_start; r < e_end; ++r) {
    render_text_with_glow(rend, font, ed.lines[r], e_inset_x, ey);
    ey += e_line_h;
}


        // cursor inside render target
        if (cur_r >= e_start && cur_r < e_end) {
            int cursor_x = e_inset_x + (int)(cur_c * (FONT_SIZE * 0.6f));
            int cursor_y = e_inset_y + (cur_r - e_start) * e_line_h;
            SDL_SetRenderDrawBlendMode(rend, SDL_BLENDMODE_BLEND);
            SDL_SetRenderDrawColor(rend, 180, 220, 180, 120);
            SDL_Rect cur = { cursor_x, cursor_y, (int)(FONT_SIZE * 0.6f), e_line_h };
            SDL_RenderFillRect(rend, &cur);
        }

        // status bar (render into target)
        char sb[256];
        snprintf(sb, sizeof(sb), "cedit: %s  (Ctrl-S save, Esc exit)  Ln %d Col %d", abs_path, cur_r+1, cur_c);
        SDL_Surface *ss = TTF_RenderUTF8_Blended(font, sb, (SDL_Color){200,180,140,255});
        render_text_with_glow(rend, font, sb, e_inset_x, (screen_h - 16) - e_inset_y - INPUT_HEIGHT - 6);


        // save prompt overlay rendering into render target (if visible)
        if (show_save_prompt) {
            char q[512];
            snprintf(q, sizeof(q), "Save changes to %s ? (Y/n)", abs_path);
            SDL_Surface *qs = TTF_RenderUTF8_Blended(font, q, (SDL_Color){255,255,255,255});
            if (qs) {
                SDL_Texture *qt = SDL_CreateTextureFromSurface(rend, qs);
                int qw = qs->w + 16;
                int qh = qs->h + 12;
                SDL_Rect box = { e_inset_x + ( (screen_w - 16) - qw)/2, ( (screen_h - 16) - qh)/2, qw, qh };
                SDL_SetRenderDrawColor(rend, 20, 40, 20, 220);
                SDL_RenderFillRect(rend, &box);
                SDL_SetRenderDrawColor(rend, 100, 100, 100, 180);
                SDL_RenderDrawRect(rend, &box);
                SDL_Rect qd = { box.x + 8, box.y + 6, qs->w, qs->h };
                SDL_RenderCopy(rend, qt, NULL, &qd);
                SDL_DestroyTexture(qt);
                SDL_FreeSurface(qs);
            }
        }

        // transient status (in render target)
        if (status_until && SDL_GetTicks() < status_until) {
            SDL_Surface *ms = TTF_RenderUTF8_Blended(font, status_msg, (SDL_Color){200,200,200,255});
            if (ms) {
                SDL_Texture *mt = SDL_CreateTextureFromSurface(rend, ms);
                SDL_Rect md = { e_inset_x, (screen_h - 16) - e_inset_x - INPUT_HEIGHT - 6 - 24, ms->w, ms->h };
                SDL_RenderCopy(rend, mt, NULL, &md);
                SDL_DestroyTexture(mt);
                SDL_FreeSurface(ms);
            }
        }

        // finished drawing editor contents into render target
        SDL_SetRenderTarget(rend, NULL);

        /* composite: apply flicker / color mod / vignette / scanlines and then draw LEDs & sticker */
        double now = SDL_GetTicks() / 1000.0;
        float base = 0.90f + 0.08f * sinf((float)(now * 6.0));
        if ((rand() % 1000) < 6) base -= ((rand() % 40) / 255.0f);
        if ((rand() % 1000) < 4) base += ((rand() % 30) / 255.0f);
        if (base < 0.5f) base = 0.5f;
        if (base > 1.05f) base = 1.05f;
        Uint8 alpha = (Uint8)(base * 255.0f);
        SDL_SetTextureAlphaMod(screen_tex, alpha);
        int green_mod = 180 + (int)(75.0f * (base - 0.9f) * 4.0f);
        if (green_mod < 180) green_mod = 180;
        if (green_mod > 255) green_mod = 255;
        SDL_SetTextureColorMod(screen_tex, 180, green_mod, 180);

        // small jitter
        int jitter_x = 0, jitter_y = 0;
        if ((rand() % 100) < 8) { jitter_x = (rand() % 3) - 1; jitter_y = (rand() % 3) - 1; }

        SDL_Rect dst = { screen_x + 8 + jitter_x, screen_y + 8 + jitter_y, screen_w - 16, screen_h - 16 };
        // background + frame
        SDL_SetRenderDrawColor(rend, 24, 20, 18, 255);
        SDL_RenderClear(rend);
        draw_frame(rend, screen_x, screen_y, screen_w, screen_h);

        // blit the CRT content
        SDL_RenderCopy(rend, screen_tex, NULL, &dst);

        // vignette overlay
        if (vignette) SDL_RenderCopy(rend, vignette, NULL, &dst);

        // scanlines
        int scan_alpha = 16 + (int)((1.0f - base) * 80.0f);
        if (scan_alpha < 8) scan_alpha = 8;
        draw_scanlines(rend, dst.x, dst.y, dst.w, dst.h, 3, scan_alpha);


/* LED positions*/
int led_r = 8;
int led_spacing = 36;
int center_x = screen_x + (screen_w / 2) - (led_spacing / 2);
int leds_y = screen_y + screen_h + 24;

/* clamp against actual renderer/window output height (not CRT height) */
int out_w = 0, out_h = 0;
SDL_GetRendererOutputSize(rend, &out_w, &out_h);
if (leds_y + led_r + 4 > out_h) leds_y = out_h - led_r - 8;

/* draw LEDs: green on when backend_running, red on when not */
draw_led(rend, center_x,               leds_y, led_r,  0, 220, 0, backend_running ? 1 : 0);
draw_led(rend, center_x + led_spacing, leds_y, led_r, 220,   0, 0, backend_running ? 0 : 1);

/* sticker anchored relative to LEDs exactly like main */
int sticker_x = screen_x + screen_w - 180;
int sticker_y = leds_y - 6;
render_worn_sticker(rend, font, "Cortez Tech .inc", sticker_x, sticker_y);


        SDL_RenderPresent(rend);
        SDL_Delay(16);

}
}




/* --- main --- */
int main(int argc, char **argv) {
    const char *backend_path = "./cortez_backend";
    char backend_cwd[PATH_MAX] = "."; // Default to current directory
    if (argc > 1) backend_path = argv[1];

    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_TIMER) != 0) { fprintf(stderr, "SDL_Init: %s\n", SDL_GetError()); return 1; }
    if (TTF_Init() != 0) { fprintf(stderr, "TTF_Init: %s\n", TTF_GetError()); SDL_Quit(); return 1; }

    // create window then go fullscreen desktop for consistent full-screen sizing
    SDL_Window *win = SDL_CreateWindow("Cortez Terminal (CRT)", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError()); TTF_Quit(); SDL_Quit(); return 1; }
    if (SDL_SetWindowFullscreen(win, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0) {
        fprintf(stderr, "SDL_SetWindowFullscreen: %s\n", SDL_GetError());
    }

    SDL_Renderer *rend = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!rend) { fprintf(stderr, "SDL_CreateRenderer: %s\n", SDL_GetError()); SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit(); return 1; }

    // font candidates
    const char *candidates[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/ubuntu/UbuntuMono-R.ttf",
        "/Library/Fonts/Andale Mono.ttf",
        "/usr/local/share/fonts/DejaVuSansMono.ttf",
        NULL
    };
    const char *font_path = NULL;
    for (int i=0; candidates[i]; ++i) {
        if (access(candidates[i], R_OK) == 0) { font_path = candidates[i]; break; }
    }
    if (!font_path) {
        fprintf(stderr, "No monospace TTF found. Install DejaVuSansMono or adjust source.\n");
        SDL_DestroyRenderer(rend); SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit(); return 1;
    }
    TTF_Font *font = TTF_OpenFont(font_path, FONT_SIZE);
    if (!font) { fprintf(stderr, "TTF_OpenFont: %s\n", TTF_GetError()); SDL_DestroyRenderer(rend); SDL_DestroyWindow(win); TTF_Quit(); SDL_Quit(); return 1; }

    SDL_StartTextInput();
    SDL_Color fg = {51,255,51,255};

    LineBuf lb; lb_init(&lb);

    int win_w = 1280, win_h = 720;
    SDL_GetWindowSize(win, &win_w, &win_h);

    // compute CRT screen rect as percentage of window so it scales nicely
    float pad_pct = 0.06f; // 6% padding each side
    int screen_x = (int)(win_w * pad_pct);
    int screen_y = (int)(win_h * pad_pct * 0.6f); // top padding slightly smaller
    int screen_w = win_w - 2 * screen_x;
    int screen_h = win_h - screen_y - (int)(win_h * pad_pct); // bottom padding
    if (screen_w < 200) screen_w = win_w - 2*screen_x;
    if (screen_h < 160) screen_h = win_h - screen_y - (int)(win_h * pad_pct);

    // create render target for CRT screen content (inner area)
    SDL_Texture *screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16);
    if (screen_tex) { SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND); }

    // create vignette texture for current screen size
    SDL_Texture *vignette = create_vignette(rend, screen_w - 16, screen_h - 16);

    // draw background and initial frame before animation
    SDL_SetRenderDrawColor(rend, 24, 20, 18, 255);
    SDL_RenderClear(rend);
    draw_frame(rend, screen_x, screen_y, screen_w, screen_h);
    SDL_RenderPresent(rend);

    // slowly reveal ART_LINES inside the CRT render target
    int art_x = 28;
    int art_y = 18;
    for (int i = 0; ART_LINES[i]; ++i) {
        const char *line = ART_LINES[i];
        size_t len = strlen(line);
        char *buf = malloc(len + 1);
        if (!buf) break;
        for (size_t k=0;k<len;k++) buf[k] = ' ';
        buf[len] = 0;
        for (size_t c=0;c<len;c++) {
            buf[c] = line[c];

            // render into screen_tex (so it gets vignette / scanlines / flicker later)
            if (screen_tex) SDL_SetRenderTarget(rend, screen_tex);
            SDL_SetRenderDrawColor(rend, 6, 18, 6, 255); // deep greenish inside screen
            SDL_RenderClear(rend);

            // previously filled art lines
            int yy = art_y;
            for (int j=0;j<i;++j) {
                if (!ART_LINES[j]) continue;
                SDL_Surface *s = TTF_RenderUTF8_Blended(font, ART_LINES[j], fg);
                if (s) {
                    SDL_Texture *t = SDL_CreateTextureFromSurface(rend, s);
                    if (t) {
                        SDL_Rect dst = { art_x, yy, s->w, s->h };
                        SDL_RenderCopy(rend, t, NULL, &dst);
                        SDL_DestroyTexture(t);
                    }
                    SDL_FreeSurface(s);
                }
                yy += FONT_SIZE + LINE_SPACING;
            }
            // current partial line
            SDL_Surface *cs = TTF_RenderUTF8_Blended(font, buf, fg);
            if (cs) {
                SDL_Texture *ct = SDL_CreateTextureFromSurface(rend, cs);
                if (ct) {
                    SDL_Rect dst = { art_x, yy, cs->w, cs->h };
                    SDL_RenderCopy(rend, ct, NULL, &dst);
                    SDL_DestroyTexture(ct);
                }
                SDL_FreeSurface(cs);
            }

            // done drawing into target, switch back to default and composite
            SDL_SetRenderTarget(rend, NULL);

            // draw the static frame (non-flickering)
            SDL_SetRenderDrawColor(rend, 24, 20, 18, 255);
            SDL_RenderClear(rend);
            draw_frame(rend, screen_x, screen_y, screen_w, screen_h);

            // compute jitter & flicker for animation (small)
            double now = SDL_GetTicks() / 1000.0;
            float flick = 0.96f + 0.04f * sinf((float)(now * 12.0 + (float)c * 0.3f));
            Uint8 alpha = (Uint8)(flick * 255.0f);
            SDL_SetTextureAlphaMod(screen_tex, alpha);

            // blit screen_tex into scr area
            SDL_Rect dst = { screen_x + 8, screen_y + 8, screen_w - 16, screen_h - 16 };
            SDL_RenderCopy(rend, screen_tex, NULL, &dst);

            // vignette on top of inner screen
            if (vignette) SDL_RenderCopy(rend, vignette, NULL, &dst);

            // subtle animated scanlines (alpha modulated)
            int scan_alpha = 18 + (rand() % 8);
            draw_scanlines(rend, dst.x, dst.y, dst.w, dst.h, 3, scan_alpha);

            int led_r = 8;
                int led_spacing = 36;
                int center_x = screen_x + (screen_w / 2) - (led_spacing / 2);

                /* put LEDs a bit lower during animation */
                int leds_y = screen_y + screen_h + 24;    /* lowered a little compared to +12 */
                if (leds_y + led_r + 4 > win_h) leds_y = win_h - led_r - 8;

                /* green off (0), red on (1) while loading */
                draw_led(rend, center_x,               leds_y, led_r,  0, 220, 0, 0);
                draw_led(rend, center_x + led_spacing, leds_y, led_r, 220,   0, 0, 1);

             
            int sticker_x = screen_x + screen_w - 180; /* tweak -180 to move further left/right */
            int sticker_y = leds_y - 6;                /* aligns roughly with LEDs baseline */
            render_worn_sticker(rend, font, "Cortez Tech .inc", sticker_x, sticker_y);
        

            SDL_RenderPresent(rend);

            SDL_Delay(6);
            // keep window responsive
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { free(buf); goto after_anim; }
                else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                    SDL_GetWindowSize(win, &win_w, &win_h);
                    screen_x = (int)(win_w * pad_pct);
                    screen_y = (int)(win_h * pad_pct * 0.6f);
                    screen_w = win_w - 2 * screen_x;
                    screen_h = win_h - screen_y - (int)(win_h * pad_pct);
                    if (screen_w < 200) screen_w = win_w - 2*screen_x;
                    if (screen_h < 160) screen_h = win_h - screen_y - (int)(win_h * pad_pct);

                    if (screen_tex) { SDL_DestroyTexture(screen_tex); screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16); if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND); }
                    if (vignette) { SDL_DestroyTexture(vignette); vignette = create_vignette(rend, screen_w - 16, screen_h - 16); }
                    art_x = 28;
                    art_y = 18;
                }
            }
        }
        free(buf);
        SDL_Delay(30);
    }
after_anim:

    // push initial welcome lines INTO THE CRT (lb is rendered inside CRT)
    lb_push(&lb, "");
    lb_push(&lb, "Welcome to Cortez Terminal (CRT)");
    lb_push(&lb, "Type 'help' and press Enter. Backend: not started yet.");
    lb_push(&lb, "");

    int wfd=-1, rfd=-1, efd=-1; pid_t backend_pid=-1;
    // indicate starting backend inside the CRT buffer
    lb_push(&lb, "[starting backend...]");
    if (access(backend_path, X_OK) == 0) {
        backend_pid = spawn_backend(backend_path, &wfd, &rfd, &efd);
        if (backend_pid <= 0) {
            lb_push(&lb, "[failed to spawn backend]");
            backend_pid = -1;
        } else {
            SDL_Delay(120);
            lb_push(&lb, "[backend started]");
        }
    } else {
        lb_push(&lb, "[backend binary not found or not executable: build it with 'make' in src/]");
    }

    char inputbuf[8192] = {0}; size_t ipos = 0; int scroll = 0; int running = 1;
    char proto[32768]; size_t proto_len = 0;

    // main loop
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) running = 0;
            else if (ev.type == SDL_WINDOWEVENT && ev.window.event == SDL_WINDOWEVENT_RESIZED) {
                SDL_GetWindowSize(win, &win_w, &win_h);
                screen_x = (int)(win_w * pad_pct);
                screen_y = (int)(win_h * pad_pct * 0.6f);
                screen_w = win_w - 2 * screen_x;
                screen_h = win_h - screen_y - (int)(win_h * pad_pct);
                if (screen_w < 200) screen_w = win_w - 2*screen_x;
                if (screen_h < 160) screen_h = win_h - screen_y - (int)(win_h * pad_pct);
                if (screen_tex) { SDL_DestroyTexture(screen_tex); screen_tex = SDL_CreateTexture(rend, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET, screen_w - 16, screen_h - 16); if (screen_tex) SDL_SetTextureBlendMode(screen_tex, SDL_BLENDMODE_BLEND); }
                if (vignette) { SDL_DestroyTexture(vignette); vignette = create_vignette(rend, screen_w - 16, screen_h - 16); }
            } else if (ev.type == SDL_TEXTINPUT) {
                const char *txt = ev.text.text;
                for (int i=0; txt[i]; ++i) { if (ipos + 1 < (int)sizeof(inputbuf)) { inputbuf[ipos++] = txt[i]; inputbuf[ipos]=0; } }
            } else if (ev.type == SDL_KEYDOWN) {
                SDL_Keycode k = ev.key.keysym.sym;
                if (k == SDLK_BACKSPACE) { if (ipos > 0) { ipos--; inputbuf[ipos]=0; } }
                else if (k == SDLK_RETURN || k == SDLK_KP_ENTER) {
                    if (ipos > 0) {
                        inputbuf[ipos]=0;

                       if (strncmp(inputbuf, "cedit ", 6) == 0) {
    const char *fn = inputbuf + 6;

    // --- FIX STARTS HERE ---
    // Skip leading whitespace from the filename
    while (*fn == ' ') {
        fn++;
    }

    // Create a temporary buffer to hold a potentially cleaned-up filename
    char cleaned_fn[PATH_MAX];
    strncpy(cleaned_fn, fn, sizeof(cleaned_fn) - 1);
    cleaned_fn[sizeof(cleaned_fn) - 1] = '\0';

    // Check for and remove the "[ok] " prefix
    if (strncmp(cleaned_fn, "[ok] ", 5) == 0) {
        // Move the rest of the string to the beginning
        memmove(cleaned_fn, cleaned_fn + 5, strlen(cleaned_fn + 5) + 1);
    }

    run_simple_editor(cleaned_fn, rend, font, screen_x, screen_y, screen_w, screen_h, backend_cwd, (backend_pid > 0) ? 1 : 0);
    /* after editor returns, don't send to backend */
} else if (strcmp(inputbuf, "cedit") == 0) {
    /*typed just 'cedit' without filename -> open empty scratch */
       run_simple_editor("Untitled.txt", rend, font, screen_x, screen_y, screen_w, screen_h, backend_cwd, (backend_pid > 0) ? 1 : 0);
}else {
                        /* normal behavior: forward to backend */
                            if (wfd >= 0) backend_write_line(wfd, inputbuf);
                                 else { lb_push(&lb, "[no backend]"); lb_push(&lb, inputbuf); }
    }
    ipos = 0;
} else {
    if (wfd >= 0) backend_write_line(wfd, "");
}

                } else if (k == SDLK_c && (ev.key.keysym.mod & KMOD_CTRL)) {
                    if (wfd >= 0) backend_write_line(wfd, "SIGINT");
                } else if (k == SDLK_UP) { scroll++; }
                else if (k == SDLK_DOWN) { if (scroll>0) scroll--; }
            }
        }

        // read backend fds (same improved handling)
        if (rfd >= 0) {
            char buf[4096];
            ssize_t rd = read(rfd, buf, sizeof(buf));
            if (rd > 0) {
                if (proto_len + rd < sizeof(proto)-1) {
                    memcpy(proto + proto_len, buf, rd); proto_len += rd; proto[proto_len]=0;
                    char *nl;
                    while ((nl = memchr(proto, '\n', proto_len)) != NULL) {
                        size_t linelen = nl - proto;
                        if (linelen >= 16383) linelen = 16383;
                        char line[16384]; memcpy(line, proto, linelen); line[linelen]=0;
                        size_t remain = proto_len - (linelen + 1);
                        memmove(proto, nl+1, remain); proto_len = remain; proto[proto_len]=0;

                        int approx_char_w = (int)(FONT_SIZE * 0.6f);
                        if (approx_char_w <= 0) approx_char_w = 8;
                        int maxcols = (screen_w - 40) / approx_char_w;
                        if (maxcols < 40) maxcols = 40;
                        if (maxcols > 1000) maxcols = 1000;

if (strncmp(line, "OK cd", 5) == 0) {
    // The line before a successful 'cd' is the new directory
    if (lb.count > 0) {
        const char* path_line = lb.lines[lb.count - 1];
        // FIX: Check for and skip the "OUT " prefix
        if (strncmp(path_line, "OUT ", 4) == 0) {
            strncpy(backend_cwd, path_line + 4, PATH_MAX - 1);
        } else {
            // Fallback for safety, copy the whole line if no prefix
            strncpy(backend_cwd, path_line, PATH_MAX - 1);
        }
        backend_cwd[PATH_MAX - 1] = '\0';
    }
}

                        if (strncmp(line, "OK ", 3) == 0) {
                            const char *payload = line + 3;
                            size_t n = strlen(payload) + 6;
                            char *tmp = malloc(n);
                            if (tmp) { snprintf(tmp, n, "[ok] %s", payload); lb_push_wrapped(&lb, tmp, maxcols); free(tmp); }
                        } else if (strncmp(line, "ERR ", 4) == 0) {
                            const char *payload = line + 4;
                            size_t n = strlen(payload) + 7;
                            char *tmp = malloc(n);
                            if (tmp) { snprintf(tmp, n, "[err] %s", payload); lb_push_wrapped(&lb, tmp, maxcols); free(tmp); }
                        } else if (strncmp(line, "STREAM_START ", 13) == 0) {
                            const char *payload = line + 13;
                            size_t n = strlen(payload) + 20;
                            char *tmp = malloc(n);
                            if (tmp) { snprintf(tmp, n, "[stream start %s]", payload); lb_push_wrapped(&lb, tmp, maxcols); free(tmp); }
                        } else if (strncmp(line, "STREAM_DATA ", 12) == 0) {
                            const char *b64 = line + 12; unsigned char dec[16384];
                            size_t dsz = b64_decode(b64, dec, sizeof(dec));
                            if (dsz) {
                                char *tmp = malloc(dsz + 1);
                                if (tmp) {
                                    memcpy(tmp, dec, dsz);
                                    tmp[dsz] = 0;

                                    strip_ansi_sequences(tmp);

                                    /* strip/handle ANSI clear sequences so we actually clear the frontend view */
                                    const char *seqs[] = { "\x1b[2J\x1b[H", "\x1b[2J", "\x1b[H", NULL };
                                    for (int si = 0; seqs[si]; ++si) {
                                        char *pos;
                                        while ((pos = strstr(tmp, seqs[si])) != NULL) {
                                        /* clear the buffer and remove the sequence from tmp */
                                        lb_clear(&lb);
                                        size_t sl = strlen(seqs[si]);
                                        size_t tail = strlen(pos + sl);
                                        memmove(pos, pos + sl, tail + 1);
                                        }
                            }

        /* now push any remaining printable text, split on newlines */
        char *p = tmp; char *q;
        while ((q = strchr(p, '\n')) != NULL) { *q = 0; if (*p) lb_push_wrapped(&lb, p, maxcols); p = q+1; }
        if (*p) lb_push_wrapped(&lb, p, maxcols);

        free(tmp);
    }
}

}else if (strncmp(line, "STREAM_END ", 11) == 0) {
                            const char *payload = line + 11;
                            size_t n = strlen(payload) + 16;
                            char *tmp = malloc(n);
                            if (tmp) { snprintf(tmp, n, "[stream end %s]", payload); lb_push_wrapped(&lb, tmp, maxcols); free(tmp); }
                        } else {
    /* if the backend emitted raw ANSI clear as an OUT line, treat it as wipe */
    if (strstr(line, "\x1b[2J") || strstr(line, "\x1b[H")) {
        lb_clear(&lb);
        /* remove the sequences and push any remaining text */
        char *tmp = strdup(line);
        const char *seqs[] = { "\x1b[2J\x1b[H", "\x1b[2J", "\x1b[H", NULL };
        for (int si = 0; seqs[si]; ++si) {
            char *pos;
            while ((pos = strstr(tmp, seqs[si])) != NULL) {
                size_t sl = strlen(seqs[si]); size_t tail = strlen(pos + sl);
                memmove(pos, pos + sl, tail + 1);
            }
        }
        if (strlen(tmp) > 0) lb_push_wrapped(&lb, tmp, maxcols);
        free(tmp);
    } else {
        lb_push_wrapped(&lb, line, maxcols);
    }
}

                    }
                } else { proto_len = 0; }
            } else if (rd == 0) { lb_push(&lb, "[backend closed]"); close(rfd); rfd = -1; }
        }
        if (efd >= 0) {
            char ebuf[512]; ssize_t er = read(efd, ebuf, sizeof(ebuf)-1);
            if (er > 0) { ebuf[er]=0; char tmp[1024]; snprintf(tmp,sizeof(tmp), "[backend-stderr] %s", ebuf); lb_push(&lb, tmp); }
            else if (er == 0) { close(efd); efd = -1; }
        }

        // Render main scene
        SDL_SetRenderDrawColor(rend, 24, 20, 18, 255);
        SDL_RenderClear(rend);

        // recompute screen rect (safety)
        screen_w = win_w - 2 * screen_x;
        screen_h = win_h - screen_y - (int)(win_h * pad_pct);
        if (screen_w < 200) screen_w = win_w - 2*screen_x;
        if (screen_h < 160) screen_h = win_h - screen_y - (int)(win_h * pad_pct);

        // draw static frame (no flicker)
        draw_frame(rend, screen_x, screen_y, screen_w, screen_h);

        // draw CRT contents into screen_tex
        if (screen_tex) SDL_SetRenderTarget(rend, screen_tex);
        SDL_SetRenderDrawColor(rend, 6, 18, 6, 255);
        SDL_RenderClear(rend);

        // draw buffered lines with glow inside render target
        int line_h = FONT_SIZE + LINE_SPACING;
        int scr_inset_x = 18;
        int scr_inset_y = 18;
        int avail_h = (screen_h - 16) - (scr_inset_y * 2) - INPUT_HEIGHT - 16;
        int max_visible = avail_h / line_h;
        if (max_visible < 1) max_visible = 1;
        int start = lb.count - max_visible - scroll;
        if (start < 0) start = 0;
        int yy = scr_inset_y;
        // NOTE: when rendering into render target we re-use render_text_with_glow (it creates textures)
        for (int i = start; i < lb.count && yy + line_h < (screen_h - 16) - scr_inset_y; ++i) {
            render_text_with_glow(rend, font, lb.lines[i], scr_inset_x, yy);
            yy += line_h;
        }

        // input box inside the render target
        SDL_Rect ibox_rt = { scr_inset_x - 6, (screen_h - 16) - scr_inset_y - INPUT_HEIGHT - 6, (screen_w - 16) - (scr_inset_x - 6)*2, INPUT_HEIGHT };
        SDL_SetRenderDrawColor(rend, 0, 28, 0, 220);
        SDL_RenderFillRect(rend, &ibox_rt);
        char promptline[8192];
        snprintf(promptline, sizeof(promptline), "> %s", inputbuf);
        SDL_Surface *ps = TTF_RenderUTF8_Blended(font, promptline, fg);
        if (ps) {
            SDL_Texture *pt = SDL_CreateTextureFromSurface(rend, ps);
            if (pt) {
                SDL_Rect pd = { ibox_rt.x + 8, ibox_rt.y + (ibox_rt.h - ps->h)/2, ps->w, ps->h };
                SDL_RenderCopy(rend, pt, NULL, &pd);
                SDL_DestroyTexture(pt);
            }
            SDL_FreeSurface(ps);
        }

        // done drawing to screen_tex
        SDL_SetRenderTarget(rend, NULL);

        // Flicker & jitter - apply to screen_tex when blitting to screen
        double now = SDL_GetTicks() / 1000.0;
        float base = 0.90f + 0.08f * sinf((float)(now * 6.0));
        // occasional random flare/drop
        if ((rand() % 1000) < 6) base -= ((rand() % 40) / 255.0f);
        if ((rand() % 1000) < 4) base += ((rand() % 30) / 255.0f);
        if (base < 0.5f) base = 0.5f;
        if (base > 1.05f) base = 1.05f;
        Uint8 alpha = (Uint8)(base * 255.0f);
        SDL_SetTextureAlphaMod(screen_tex, alpha);
        // slight color modulation (dim / brighten green channel a bit)
        int green_mod = 200 + (int)(55.0f * (base - 0.9f) * 4.0f);
        if (green_mod < 180) green_mod = 180;
        if (green_mod > 255) green_mod = 255;
        SDL_SetTextureColorMod(screen_tex, 180, green_mod, 180);

        // jitter destination position occasionally
        int jitter_x = 0, jitter_y = 0;
        if ((rand() % 100) < 8) {
            jitter_x = (rand() % 3) - 1;
            jitter_y = (rand() % 3) - 1;
        }

        SDL_Rect dst = { screen_x + 8 + jitter_x, screen_y + 8 + jitter_y, screen_w - 16, screen_h - 16 };
        SDL_RenderCopy(rend, screen_tex, NULL, &dst);

        // vignette overlay
        if (vignette) SDL_RenderCopy(rend, vignette, NULL, &dst);

        // scanlines on top (alpha modulated by flicker)
        int scan_alpha = 16 + (int)((1.0f - base) * 80.0f);
        if (scan_alpha < 8) scan_alpha = 8;
        draw_scanlines(rend, dst.x, dst.y, dst.w, dst.h, 3, scan_alpha);

        /* LED positions -- anchored to frame center and clamped to window */
        int led_r = 8;
        int led_spacing = 36;

        /* Use frame center so LEDs move with the frame when window is resized */
        int center_x = screen_x + (screen_w / 2) - (led_spacing / 2);

        /* place just below the frame; keep a small margin and clamp to window height */
        int leds_y = screen_y + screen_h + 24;   /* 12 px below frame (tweak if you want) */
        if (leds_y + led_r + 4 > win_h)          /* clamp if too low */
            leds_y = win_h - led_r - 8;

        /* draw (green on when backend_pid>0, red on when not) */
        draw_led(rend, center_x,              leds_y, led_r,  0, 220, 0,  (backend_pid > 0) ? 1 : 0);
        draw_led(rend, center_x + led_spacing, leds_y, led_r, 220, 0, 0,  (backend_pid <= 0) ? 1 : 0);

         
            int sticker_x = screen_x + screen_w - 180; /* tweak -180 to move further left/right */
            int sticker_y = leds_y - 6;                /* aligns roughly with LEDs baseline */
            render_worn_sticker(rend, font, "Cortez Tech .inc", sticker_x, sticker_y);
        

        SDL_RenderPresent(rend);
        SDL_Delay(10);
    }

    if (screen_tex) SDL_DestroyTexture(screen_tex);
    if (vignette) SDL_DestroyTexture(vignette);
    if (wfd >= 0) close(wfd);
    if (rfd >= 0) close(rfd);
    if (efd >= 0) close(efd);
    if (backend_pid > 0) {
        kill(backend_pid, SIGTERM);
        waitpid(backend_pid, NULL, 0);
    }

    int led_r = 8;
                int led_spacing = 36;
                int center_x = screen_x + (screen_w / 2) - (led_spacing / 2);

                /* put LEDs a bit lower during animation */
                int leds_y = screen_y + screen_h + 24;    /* lowered a little compared to +12 */
                if (leds_y + led_r + 4 > win_h) leds_y = win_h - led_r - 8;

                /* green off (0), red on (1) while loading */
                draw_led(rend, center_x,               leds_y, led_r,  0, 220, 0, 0);
                draw_led(rend, center_x + led_spacing, leds_y, led_r, 220,   0, 0, 1);

             
            int sticker_x = screen_x + screen_w - 180; /* tweak -180 to move further left/right */
            int sticker_y = leds_y - 6;                /* aligns roughly with LEDs baseline */

    lb_free(&lb);
    TTF_CloseFont(font);
    SDL_DestroyRenderer(rend);
    SDL_DestroyWindow(win);
    TTF_Quit();
    SDL_Quit();
    return 0;
}
