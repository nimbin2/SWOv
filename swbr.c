/* SwBr (swbr) — a floating layer-shell bar for Sway
 *
 * Sits on top of the tiled windows at the screen edge, does not steal space
 * from them. Hover it and press space to fold it into a thin signal strip.
 *
 * Dependencies:
 *   - libwayland-client   (the only library you link)
 *   - stb_truetype.h      (vendored single-header — smooth fonts)
 * The wlr-layer-shell protocol glue is vendored below, so there is no
 * wayland-scanner step and nothing else to install.
 *
 * Build:
 *   cc -std=c11 -O2 -Wall -Wextra -o swbr swbr.c -lwayland-client -lm
 *
 * Runtime deps: a running Sway session ($SWAYSOCK). No jq, no shell-outs:
 * the program talks to the Sway IPC socket directly. fontconfig (fc-match)
 * is used if present to pick a font, otherwise known paths are tried.
 *
 * See --help for every option (all config keys also work on the command line).
 */

/* ---------------------------------------------------------------------------
 * How this file is laid out (top to bottom):
 *   1. small helpers        colors, string trim, hex-color + tilde parsing
 *   2. layer-shell glue     vendored wlr-layer-shell-unstable-v1 marshalling
 *   3. JSON                 the tiny parser used for sway's replies
 *   4. sway ipc             request/subscribe socket, workspaces, mode
 *   5. Config               the struct, defaults, key=value setter, file loader
 *   6. text (Font)          stb_truetype glyph cache, software blitting
 *   7. paint                ARGB32 buffer, rounded rectangles, clipping
 *   8. markup               the pango subset (<span foreground=..>, <b>, &amp;)
 *   9. status               the status_command child process and its lines
 *  10. wayland              globals, one Bar per output, shm, pointer, keys
 *  11. render               the expanded bar and the collapsed signal strip
 *  12. usage()/dump_config  --help text and the default config generator
 *  13. main()               setup, then the poll loop
 * ------------------------------------------------------------------------- */

#define _POSIX_C_SOURCE 200809L
#define _GNU_SOURCE

#include <wayland-client.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "sw_theme.h"

#define APP_ID          "swbr"
#define SWBR_VERSION  "0.9.0"
#ifndef SWBR_BUILD                 /* set by the Makefile: md5 of this file */
#define SWBR_BUILD "unknown"
#endif
#define MAX_WORKSPACES  64
#define MAX_OUTPUTS     8
#define MAX_RUNS        128
#define MAX_BINDS       10
#define MAX_CELLS       24

enum { SLIM_AUTO, SLIM_TICK, SLIM_BAR, SLIM_CLOCK, SLIM_PRESENCE, SLIM_OFF };

/* ------------------------------------------------------------------ util */

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("swbr: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

static void *xmalloc(size_t n)
{
    void *p = malloc(n ? n : 1);
    if (!p) die("out of memory");
    return p;
}

static void *xrealloc(void *p, size_t n)
{
    void *q = realloc(p, n ? n : 1);
    if (!q) die("out of memory");
    return q;
}

static void str_set(char *dst, size_t cap, const char *src)
{
    if (!cap) return;
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = 0;
}

static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    if (!*s) return s;
    char *e = s + strlen(s) - 1;
    while (e > s && isspace((unsigned char)*e)) *e-- = 0;
    return s;
}

static bool file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static int   clampi(int v, int lo, int hi)       { return v < lo ? lo : (v > hi ? hi : v); }

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

typedef struct { uint8_t r, g, b, a; } Col;

static void parse_color(const char *s, Col *out)
{
    if (*s == '#') s++;
    unsigned r, g, b, a = 255;
    if (strlen(s) >= 8 && sscanf(s, "%2x%2x%2x%2x", &r, &g, &b, &a) == 4) { }
    else if (sscanf(s, "%2x%2x%2x", &r, &g, &b) == 3) { }
    else return;
    out->r = (uint8_t)r; out->g = (uint8_t)g; out->b = (uint8_t)b; out->a = (uint8_t)a;
}

/* expand a leading ~/ to $HOME (no shell involved) */
static void expand_tilde(const char *in, char *out, size_t n)
{
    if (in[0] == '~' && (in[1] == '/' || in[1] == 0)) {
        const char *home = getenv("HOME");
        if (home) { snprintf(out, n, "%s%s", home, in + 1); return; }
    }
    snprintf(out, n, "%s", in);
}

/* ------------------------------------------------- wlr-layer-shell glue */
/* Vendored equivalent of what wayland-scanner emits for
 * wlr-layer-shell-unstable-v1.xml (Copyright (c) 2017 Drew DeVault, MIT-ish;
 * see the protocol file for the full notice). Only the requests and events
 * swbr actually uses are wrapped; get_popup is present in the table so the
 * opcodes line up, but is never called, hence its NULL type entry. */

enum zwlr_layer_shell_v1_layer {
    ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND = 0,
    ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM     = 1,
    ZWLR_LAYER_SHELL_V1_LAYER_TOP        = 2,
    ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY    = 3
};
enum zwlr_layer_surface_v1_anchor {
    ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP    = 1,
    ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM = 2,
    ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT   = 4,
    ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT  = 8
};
enum zwlr_layer_surface_v1_keyboard_interactivity {
    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE      = 0,
    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE = 1,
    ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND = 2
};

struct zwlr_layer_shell_v1;
struct zwlr_layer_surface_v1;

extern const struct wl_interface zwlr_layer_surface_v1_interface;

static const struct wl_interface *swbr_ls_types[] = {
    NULL, NULL, NULL, NULL,
    &zwlr_layer_surface_v1_interface,
    &wl_surface_interface,
    &wl_output_interface,
    NULL, NULL, NULL,
};

static const struct wl_message zwlr_layer_shell_v1_requests[] = {
    { "get_layer_surface", "no?ous", swbr_ls_types + 4 },
    { "destroy", "3", swbr_ls_types + 0 },
};
static const struct wl_interface zwlr_layer_shell_v1_interface = {
    "zwlr_layer_shell_v1", 4, 2, zwlr_layer_shell_v1_requests, 0, NULL,
};

static const struct wl_message zwlr_layer_surface_v1_requests[] = {
    { "set_size", "uu", swbr_ls_types + 0 },
    { "set_anchor", "u", swbr_ls_types + 0 },
    { "set_exclusive_zone", "i", swbr_ls_types + 0 },
    { "set_margin", "iiii", swbr_ls_types + 0 },
    { "set_keyboard_interactivity", "u", swbr_ls_types + 0 },
    { "get_popup", "o", swbr_ls_types + 9 },
    { "ack_configure", "u", swbr_ls_types + 0 },
    { "destroy", "", swbr_ls_types + 0 },
    { "set_layer", "2u", swbr_ls_types + 0 },
};
static const struct wl_message zwlr_layer_surface_v1_events[] = {
    { "configure", "uuu", swbr_ls_types + 0 },
    { "closed", "", swbr_ls_types + 0 },
};
const struct wl_interface zwlr_layer_surface_v1_interface = {
    "zwlr_layer_surface_v1", 4, 9, zwlr_layer_surface_v1_requests,
    2, zwlr_layer_surface_v1_events,
};

struct zwlr_layer_surface_v1_listener {
    void (*configure)(void *data, struct zwlr_layer_surface_v1 *s,
                      uint32_t serial, uint32_t width, uint32_t height);
    void (*closed)(void *data, struct zwlr_layer_surface_v1 *s);
};

#define LS_REQ_SET_SIZE        0
#define LS_REQ_SET_ANCHOR      1
#define LS_REQ_SET_EXCLUSIVE   2
#define LS_REQ_SET_MARGIN      3
#define LS_REQ_SET_KBD         4
#define LS_REQ_ACK_CONFIGURE   6
#define LS_REQ_DESTROY         7

static struct zwlr_layer_surface_v1 *
zwlr_layer_shell_v1_get_layer_surface(struct zwlr_layer_shell_v1 *shell,
                                      struct wl_surface *surface,
                                      struct wl_output *output,
                                      uint32_t layer, const char *ns)
{
    struct wl_proxy *id = wl_proxy_marshal_flags(
        (struct wl_proxy *)shell, 0, &zwlr_layer_surface_v1_interface,
        wl_proxy_get_version((struct wl_proxy *)shell), 0,
        NULL, surface, output, layer, ns);
    return (struct zwlr_layer_surface_v1 *)id;
}

static int zwlr_layer_surface_v1_add_listener(struct zwlr_layer_surface_v1 *s,
                                              const struct zwlr_layer_surface_v1_listener *l,
                                              void *data)
{
    return wl_proxy_add_listener((struct wl_proxy *)s, (void (**)(void))l, data);
}

static void ls_set_size(struct zwlr_layer_surface_v1 *s, uint32_t w, uint32_t h)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_SET_SIZE, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s), 0, w, h); }
static void ls_set_anchor(struct zwlr_layer_surface_v1 *s, uint32_t a)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_SET_ANCHOR, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s), 0, a); }
static void ls_set_exclusive_zone(struct zwlr_layer_surface_v1 *s, int32_t z)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_SET_EXCLUSIVE, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s), 0, z); }
static void ls_set_margin(struct zwlr_layer_surface_v1 *s, int32_t t, int32_t r,
                          int32_t b, int32_t l)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_SET_MARGIN, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s), 0, t, r, b, l); }
static void ls_set_keyboard(struct zwlr_layer_surface_v1 *s, uint32_t k)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_SET_KBD, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s), 0, k); }
static void ls_ack_configure(struct zwlr_layer_surface_v1 *s, uint32_t serial)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_ACK_CONFIGURE, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s), 0, serial); }
static void ls_destroy(struct zwlr_layer_surface_v1 *s)
{ wl_proxy_marshal_flags((struct wl_proxy *)s, LS_REQ_DESTROY, NULL,
                         wl_proxy_get_version((struct wl_proxy *)s),
                         WL_MARSHAL_FLAG_DESTROY); }

/* ------------------------------------------------------------------ json */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JV {
    JType type;
    bool b;
    double num;
    char *str;
    struct JV **items;
    char **keys;
    int count, cap;
} JV;

static JV *jnew(JType t)
{
    JV *v = (JV *)xmalloc(sizeof(JV));
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

static void jfree(JV *v)
{
    if (!v) return;
    for (int i = 0; i < v->count; ++i) {
        if (v->keys) free(v->keys[i]);
        jfree(v->items[i]);
    }
    free(v->items);
    free(v->keys);
    free(v->str);
    free(v);
}

static void jpush(JV *v, char *key, JV *child)
{
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = (JV **)xrealloc(v->items, (size_t)v->cap * sizeof(JV *));
        v->keys  = (char **)xrealloc(v->keys, (size_t)v->cap * sizeof(char *));
    }
    v->keys[v->count] = key;
    v->items[v->count++] = child;
}

static const char *jskip_ws(const char *p)
{
    while (*p && (unsigned char)*p <= ' ') p++;
    return p;
}

static void utf8_append(char **s, int *len, int *cap, unsigned cp)
{
    char tmp[4];
    int n = 0;
    if (cp < 0x80) { tmp[n++] = (char)cp; }
    else if (cp < 0x800) { tmp[n++] = (char)(0xC0 | (cp >> 6)); tmp[n++] = (char)(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
        tmp[n++] = (char)(0xE0 | (cp >> 12));
        tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        tmp[n++] = (char)(0xF0 | (cp >> 18));
        tmp[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[n++] = (char)(0x80 | (cp & 0x3F));
    }
    while (*len + n + 1 > *cap) { *cap *= 2; *s = (char *)xrealloc(*s, (size_t)*cap); }
    memcpy(*s + *len, tmp, (size_t)n);
    *len += n;
    (*s)[*len] = 0;
}

static const char *jparse_string(const char *p, char **out)
{
    p++;                                  /* opening quote */
    int cap = 32, len = 0;
    char *s = (char *)xmalloc((size_t)cap);
    s[0] = 0;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            unsigned cp;
            switch (*p++) {
            case 'n': cp = '\n'; break;
            case 't': cp = '\t'; break;
            case 'r': cp = '\r'; break;
            case 'b': cp = '\b'; break;
            case 'f': cp = '\f'; break;
            case '/': cp = '/';  break;
            case '\\': cp = '\\'; break;
            case '"': cp = '"';  break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4 && isxdigit((unsigned char)*p); ++i) {
                    char c = *p++;
                    v = v * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0'
                                            : (tolower(c) - 'a' + 10));
                }
                cp = v;
                if (cp >= 0xD800 && cp <= 0xDBFF && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo = 0;
                    for (int i = 0; i < 4 && isxdigit((unsigned char)*p); ++i) {
                        char c = *p++;
                        lo = lo * 16 + (unsigned)(isdigit((unsigned char)c) ? c - '0'
                                                  : (tolower(c) - 'a' + 10));
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                break;
            }
            default:
                if (!p[-1]) { free(s); *out = NULL; return NULL; }
                cp = (unsigned char)p[-1];
                break;
            }
            utf8_append(&s, &len, &cap, cp);
        } else {
            if (len + 2 > cap) { cap *= 2; s = (char *)xrealloc(s, (size_t)cap); }
            s[len++] = *p++;
            s[len] = 0;
        }
    }
    if (*p != '"') { free(s); *out = NULL; return NULL; }
    *out = s;
    return p + 1;
}

static const char *jparse_value(const char *p, JV **out);

static const char *jparse_container(const char *p, JV **out, bool is_obj)
{
    JV *v = jnew(is_obj ? J_OBJ : J_ARR);
    p = jskip_ws(p + 1);
    char close = is_obj ? '}' : ']';
    if (*p == close) { *out = v; return p + 1; }

    for (;;) {
        char *key = NULL;
        if (is_obj) {
            p = jskip_ws(p);
            if (*p != '"') { jfree(v); return NULL; }
            p = jparse_string(p, &key);
            if (!p) { jfree(v); return NULL; }
            p = jskip_ws(p);
            if (*p != ':') { free(key); jfree(v); return NULL; }
            p++;
        }
        JV *child = NULL;
        p = jparse_value(jskip_ws(p), &child);
        if (!p) { free(key); jfree(v); return NULL; }
        jpush(v, key, child);

        p = jskip_ws(p);
        if (*p == ',') { p++; continue; }
        if (*p == close) { p++; break; }
        jfree(v);
        return NULL;
    }
    *out = v;
    return p;
}

static const char *jparse_value(const char *p, JV **out)
{
    p = jskip_ws(p);
    switch (*p) {
    case '{': return jparse_container(p, out, true);
    case '[': return jparse_container(p, out, false);
    case '"': {
        JV *v = jnew(J_STR);
        p = jparse_string(p, &v->str);
        if (!p) { jfree(v); return NULL; }
        *out = v;
        return p;
    }
    case 't':
        if (strncmp(p, "true", 4)) return NULL;
        *out = jnew(J_BOOL); (*out)->b = true; return p + 4;
    case 'f':
        if (strncmp(p, "false", 5)) return NULL;
        *out = jnew(J_BOOL); (*out)->b = false; return p + 5;
    case 'n':
        if (strncmp(p, "null", 4)) return NULL;
        *out = jnew(J_NULL); return p + 4;
    default: {
        char *end = NULL;
        double d = strtod(p, &end);
        if (end == p) return NULL;
        JV *v = jnew(J_NUM);
        v->num = d;
        *out = v;
        return end;
    }
    }
}

static JV *jparse(const char *text)
{
    JV *v = NULL;
    const char *p = jparse_value(text, &v);
    if (!p) { jfree(v); return NULL; }
    return v;
}

static JV *jget(const JV *o, const char *key)
{
    if (!o || o->type != J_OBJ) return NULL;
    for (int i = 0; i < o->count; ++i)
        if (o->keys[i] && strcmp(o->keys[i], key) == 0) return o->items[i];
    return NULL;
}

static const char *jstr(const JV *o, const char *key, const char *def)
{
    JV *v = jget(o, key);
    return (v && v->type == J_STR) ? v->str : def;
}

static int jint(const JV *o, const char *key, int def)
{
    JV *v = jget(o, key);
    return (v && v->type == J_NUM) ? (int)v->num : def;
}

static bool jbool(const JV *o, const char *key, bool def)
{
    JV *v = jget(o, key);
    return (v && v->type == J_BOOL) ? v->b : def;
}

/* -------------------------------------------------------------- sway ipc */

enum {
    IPC_RUN_COMMAND    = 0,
    IPC_GET_WORKSPACES = 1,
    IPC_SUBSCRIBE      = 2,
    IPC_GET_OUTPUTS    = 3
};

static int sway_fd = -1;      /* request socket */
static int sway_evt_fd = -1;  /* event socket   */

static int sway_connect(void)
{
    const char *path = getenv("SWAYSOCK");
    if (!path || !*path) path = getenv("I3SOCK");
    if (!path || !*path) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    str_set(addr.sun_path, sizeof(addr.sun_path), path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool write_all(int fd, const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n) {
        ssize_t w = write(fd, p, n);
        if (w < 0) { if (errno == EINTR) continue; return false; }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static bool read_all(int fd, void *buf, size_t n)
{
    char *p = (char *)buf;
    while (n) {
        ssize_t r = read(fd, p, n);
        if (r < 0) { if (errno == EINTR) continue; return false; }
        if (r == 0) return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

/* Returns a malloc'ed, NUL-terminated payload (caller frees), or NULL. */
static char *sway_request(uint32_t type, const char *payload)
{
    if (sway_fd < 0) return NULL;

    size_t plen = payload ? strlen(payload) : 0;
    char hdr[14];
    memcpy(hdr, "i3-ipc", 6);
    uint32_t l = (uint32_t)plen, t = type;
    memcpy(hdr + 6, &l, 4);
    memcpy(hdr + 10, &t, 4);

    if (!write_all(sway_fd, hdr, sizeof(hdr))) return NULL;
    if (plen && !write_all(sway_fd, payload, plen)) return NULL;

    char rhdr[14];
    if (!read_all(sway_fd, rhdr, sizeof(rhdr))) return NULL;
    if (memcmp(rhdr, "i3-ipc", 6) != 0) return NULL;

    uint32_t rlen = 0;
    memcpy(&rlen, rhdr + 6, 4);
    if (rlen > (64u << 20)) return NULL;

    char *body = (char *)xmalloc(rlen + 1);
    if (rlen && !read_all(sway_fd, body, rlen)) { free(body); return NULL; }
    body[rlen] = 0;
    return body;
}

static JV *sway_query(uint32_t type)
{
    char *body = sway_request(type, NULL);
    if (!body) return NULL;
    JV *v = jparse(body);
    free(body);
    return v;
}

static bool sway_cmd(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return false;

    char *cmd = (char *)xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(cmd, (size_t)n + 1, fmt, ap);
    va_end(ap);

    char *reply = sway_request(IPC_RUN_COMMAND, cmd);
    free(cmd);
    if (!reply) return false;

    bool ok = true;
    JV *v = jparse(reply);
    if (v && v->type == J_ARR)
        for (int i = 0; i < v->count; ++i)
            if (!jbool(v->items[i], "success", true)) ok = false;
    jfree(v);
    free(reply);
    return ok;
}

/* A second socket carries workspace/mode events, so the bar repaints when
 * sway says something changed instead of polling the tree. */
static bool sway_subscribe_events(void)
{
    sway_evt_fd = sway_connect();
    if (sway_evt_fd < 0) return false;

    const char *payload = "[\"workspace\",\"mode\"]";
    char hdr[14];
    memcpy(hdr, "i3-ipc", 6);
    uint32_t l = (uint32_t)strlen(payload), t = IPC_SUBSCRIBE;
    memcpy(hdr + 6, &l, 4);
    memcpy(hdr + 10, &t, 4);

    if (!write_all(sway_evt_fd, hdr, sizeof(hdr)) ||
        !write_all(sway_evt_fd, payload, l)) {
        close(sway_evt_fd);
        sway_evt_fd = -1;
        return false;
    }
    char rhdr[14];
    uint32_t rlen = 0;
    if (!read_all(sway_evt_fd, rhdr, sizeof(rhdr))) {
        close(sway_evt_fd); sway_evt_fd = -1; return false;
    }
    memcpy(&rlen, rhdr + 6, 4);
    if (rlen && rlen < (1u << 20)) {
        char *body = (char *)xmalloc(rlen + 1);
        read_all(sway_evt_fd, body, rlen);
        free(body);
    }
    int flags = fcntl(sway_evt_fd, F_GETFL, 0);
    if (flags >= 0) fcntl(sway_evt_fd, F_SETFL, flags | O_NONBLOCK);
    return true;
}

/* true when sway reported at least one change since the last call. The mode
 * name is picked out of "mode" events on the way past. */
static char sway_mode[128] = "default";

static bool sway_events_pending(void)
{
    if (sway_evt_fd < 0) return false;
    bool any = false;

    for (;;) {
        char hdr[14];
        ssize_t r = recv(sway_evt_fd, hdr, sizeof(hdr), MSG_DONTWAIT);
        if (r <= 0) break;
        if ((size_t)r < sizeof(hdr) &&
            !read_all(sway_evt_fd, hdr + r, sizeof(hdr) - (size_t)r)) break;

        uint32_t rlen = 0, rtype = 0;
        memcpy(&rlen, hdr + 6, 4);
        memcpy(&rtype, hdr + 10, 4);
        if (rlen > (16u << 20)) break;

        char *body = (char *)xmalloc(rlen + 1);
        if (rlen && !read_all(sway_evt_fd, body, rlen)) { free(body); break; }
        body[rlen] = 0;

        /* 0x80000002 = mode, 0x80000000 = workspace */
        if (rtype == 0x80000002u) {
            JV *v = jparse(body);
            if (v) str_set(sway_mode, sizeof(sway_mode), jstr(v, "change", "default"));
            jfree(v);
        }
        free(body);
        any = true;
    }
    return any;
}

/* ---------------------------------------------------------- workspaces */

typedef struct {
    int  num;
    char name[128];      /* full name, e.g. "3:web" */
    char label[128];     /* what we draw: number only, or the full name */
    char output[64];
    bool focused, visible, urgent;
} Ws;

static Ws  ws_list[MAX_WORKSPACES];
static int ws_count = 0;

/* "3:web" -> "3" ; a name without a leading number keeps the name */
static void ws_make_label(Ws *w, bool full_names)
{
    if (full_names) { str_set(w->label, sizeof(w->label), w->name); return; }
    const char *colon = strchr(w->name, ':');
    if (colon && colon != w->name) {
        size_t n = (size_t)(colon - w->name);
        if (n >= sizeof(w->label)) n = sizeof(w->label) - 1;
        memcpy(w->label, w->name, n);
        w->label[n] = 0;
        return;
    }
    if (w->num >= 0) snprintf(w->label, sizeof(w->label), "%d", w->num);
    else             str_set(w->label, sizeof(w->label), w->name);
}

static void ws_reload(bool full_names)
{
    JV *r = sway_query(IPC_GET_WORKSPACES);
    if (!r || r->type != J_ARR) { jfree(r); return; }

    ws_count = 0;
    for (int i = 0; i < r->count && ws_count < MAX_WORKSPACES; ++i) {
        JV *o = r->items[i];
        Ws *w = &ws_list[ws_count++];
        memset(w, 0, sizeof(*w));
        w->num = jint(o, "num", -1);
        str_set(w->name, sizeof(w->name), jstr(o, "name", "?"));
        str_set(w->output, sizeof(w->output), jstr(o, "output", ""));
        w->focused = jbool(o, "focused", false);
        w->visible = jbool(o, "visible", false);
        w->urgent  = jbool(o, "urgent", false);
        ws_make_label(w, full_names);
    }
    jfree(r);
}

/* ---------------------------------------------------------------- config */

/* A cell is one command whose output is drawn on the right hand side.
 * Declared with `cell=NAME`, configured with `NAME.field=value`. */
typedef struct {
    char  name[32];
    char  cmd[2048];
    int   interval;              /* seconds; 0 = run once; -1 = keep streaming */
    char  fmt[128];              /* %s is replaced by the output */
    char  empty[128];            /* drawn when the command prints nothing */
    int   gap;                   /* -1 = cell_gap */
    int   pad;                   /* inner padding, left and right */
    int   slim;                  /* how it shows in the folded strip */
    float slim_min, slim_max;    /* value range a gauge maps, default 0..100 */
    int   slim_w;                /* custom width in the folded strip */
    Col   slim_color;            /* overrides the colour on the strip */
    bool  has_slim_color;
    Col   color, bg;
    bool  has_color, has_bg;
    Col   warn_col, crit_col;
    float warn_th, crit_th;
    int   warn_dir, crit_dir;    /* +1 = at or above, -1 = at or below */
    bool  has_warn, has_crit;
    int   min_w, max_w;
    char  align[8];
    int   sep, hide_empty, markup;
    char  pos[8];                /* which group: left | center | right */
    int   scroll;                /* too long -> ".." , and scroll while hovered */
    float scroll_px;
    uint32_t scroll_t;
    char  bind[MAX_BINDS][256];

    /* runtime */
    pid_t pid;
    int   fd;
    char  out[1024];             /* the last complete output */
    char  buf[2048];
    size_t len;
    uint32_t next_run;
} Cell;

typedef struct {
    /* placement */
    char layer[16], position[16];
    int  height, exclusive, margin, side_margin, min_width;
    char align_x[8];
    float radius;
    char outputs[512];

    /* workspaces */
    int  ws_names, ws_pad, ws_gap, ws_min_w, ws_click, scroll_workspace, ws_inset;
    float ws_radius;
    int  mode_show;

    /* text */
    char font[PATH_MAX], font_alt[PATH_MAX];
    float ui_scale, text_px, ws_px, text_y;
    int  pad_x, pad_right, markup;
    int  scroll_speed, scroll_pause;
    char layout[1024];

    /* status */
    char status_command[4096];
    int  interval;
    Cell cell[MAX_CELLS];
    int  cell_count;
    char msg_fifo[PATH_MAX], msg_target[32];
    int  msg_timeout, msg_flash;
    Col  msg_info, msg_warn, msg_error;
    int  cell_gap, cell_inset, slim_ws_slots;
    float cell_radius;
    char separator[16];
    Col  separator_color;

    /* collapse */
    char hide_key[32];
    int  hide_keycode, hover_keys, collapsed_px, anim_ms, signals, start_collapsed;

    /* mouse bindings, index = button number (1..9) */
    char bind[MAX_BINDS][512];

    /* colors */
    Col bg, text, dim, accent, hl, urgent, outline, running;
    Col ws_bg, ws_fg, ws_focused_bg, ws_focused_fg;
    Col ws_visible_bg, ws_visible_fg, ws_urgent_bg, ws_urgent_fg;
    Col mode_bg, mode_fg;
} Config;

static void config_defaults(Config *c)
{
    memset(c, 0, sizeof(*c));
    str_set(c->layer, sizeof(c->layer), "top");
    str_set(c->position, sizeof(c->position), "top");
    c->height = 0;                 /* 0 = derive from the font size */
    c->exclusive = 0;
    c->margin = 0;
    c->side_margin = 0;
    c->min_width = 800;
    str_set(c->align_x, sizeof(c->align_x), "center");
    c->radius = 10.0f;

    c->ws_names = 0;
    c->ws_pad = 11;
    c->ws_gap = 4;
    c->ws_inset = 0;
    c->ws_min_w = 0;
    c->ws_click = 1;
    c->scroll_workspace = 0;
    c->ws_radius = 0.0f;           /* square: the button spans the full height */
    c->mode_show = 1;

    c->ui_scale = 1.0f;
    c->text_px = 19.0f;
    c->ws_px = 19.0f;
    c->text_y = -2.0f;
    c->pad_x = 12;
    c->pad_right = 20;
    c->scroll_speed = 45;
    c->scroll_pause = 900;
    c->markup = 1;

    c->interval = 1;
    c->cell_gap = 14;
    c->cell_inset = 3;
    c->slim_ws_slots = 10;
    c->cell_radius = 5.0f;
    c->msg_timeout = 8;
    c->msg_flash = 1;
    c->msg_info  = (Col){ 0x89, 0xaf, 0xc4, 0xff };
    c->msg_warn  = (Col){ 0xcb, 0x9b, 0x00, 0xff };
    c->msg_error = (Col){ 0xe0, 0x53, 0x3c, 0xff };
    str_set(c->separator, sizeof(c->separator), "");
    c->separator_color = (Col){ 0x5a, 0x6b, 0x7a, 0xff };

    str_set(c->hide_key, sizeof(c->hide_key), "space");
    c->hide_keycode = 0;
    c->hover_keys = 1;
    c->collapsed_px = 5;
    c->anim_ms = 120;
    c->signals = 1;
    c->start_collapsed = 0;

    c->bg            = (Col){ 0x26, 0x31, 0x3f, 0xe6 };
    c->text          = (Col){ 0xe8, 0xe8, 0xe8, 0xff };
    c->dim           = (Col){ 0x5a, 0x6b, 0x7a, 0xff };
    c->accent        = (Col){ 0x89, 0xaf, 0xc4, 0xff };
    c->hl            = (Col){ 0xcb, 0x9b, 0x00, 0xff };
    c->urgent        = (Col){ 0xe0, 0x53, 0x3c, 0xff };
    c->running       = (Col){ 0x2f, 0xbf, 0x71, 0xff };
    c->outline       = (Col){ 0x0a, 0x0e, 0x14, 0x99 };
    c->ws_bg         = (Col){ 0x1e, 0x27, 0x33, 0x00 };
    c->ws_fg         = (Col){ 0xb3, 0xc0, 0xcd, 0xff };
    c->ws_focused_bg = (Col){ 0xcb, 0x9b, 0x00, 0xff };
    c->ws_focused_fg = (Col){ 0x14, 0x14, 0x14, 0xff };
    c->ws_visible_bg = (Col){ 0x3c, 0x4a, 0x5c, 0xff };
    c->ws_visible_fg = (Col){ 0xe1, 0xee, 0xff, 0xff };
    c->ws_urgent_bg  = (Col){ 0xe0, 0x53, 0x3c, 0xff };
    c->ws_urgent_fg  = (Col){ 0x14, 0x14, 0x14, 0xff };
    c->mode_bg       = (Col){ 0xcb, 0x9b, 0x00, 0xff };
    c->mode_fg       = (Col){ 0x14, 0x14, 0x14, 0xff };
}

/* ------------------------------------------------------------------ cells */

static Cell *cell_find(Config *c, const char *name)
{
    for (int i = 0; i < c->cell_count; ++i)
        if (!strcmp(c->cell[i].name, name)) return &c->cell[i];
    return NULL;
}

static Cell *cell_add(Config *c, const char *name)
{
    Cell *e = cell_find(c, name);
    if (e) return e;
    if (c->cell_count >= MAX_CELLS) return NULL;
    e = &c->cell[c->cell_count++];
    memset(e, 0, sizeof(*e));
    str_set(e->name, sizeof(e->name), name);
    e->interval = 2;
    e->hide_empty = 1;
    e->gap = -1;
    e->slim_min = 0.0f;
    e->slim_max = 100.0f;
    e->markup = 1;
    e->sep = 1;
    e->fd = -1;
    e->pid = -1;
    str_set(e->align, sizeof(e->align), "right");
    str_set(e->pos, sizeof(e->pos), "right");
    return e;
}

/* "70:cecb00" or "<30:ff2222" — direction, threshold, colour */
static void cell_threshold(const char *v, float *th, int *dir, Col *col, bool *has)
{
    *dir = 1;
    if (*v == '<') { *dir = -1; v++; }
    else if (*v == '>') { *dir = 1; v++; }
    *th = (float)atof(v);
    const char *colon = strchr(v, ':');
    if (colon) parse_color(colon + 1, col);
    *has = true;
}

static void cell_set(Config *c, const char *name, const char *k, const char *v)
{
    Cell *e = cell_add(c, name);
    if (!e) return;
    if (!strcmp(k, "cmd") || !strcmp(k, "command")) str_set(e->cmd, sizeof(e->cmd), v);
    else if (!strcmp(k, "interval")) e->interval = atoi(v);
    else if (!strcmp(k, "fmt") || !strcmp(k, "format")) str_set(e->fmt, sizeof(e->fmt), v);
    else if (!strcmp(k, "empty")) str_set(e->empty, sizeof(e->empty), v);
    else if (!strcmp(k, "gap")) e->gap = atoi(v);
    else if (!strcmp(k, "pad")) e->pad = atoi(v);
    else if (!strcmp(k, "slim_min")) e->slim_min = (float)atof(v);
    else if (!strcmp(k, "slim_max")) e->slim_max = (float)atof(v);
    else if (!strcmp(k, "slim_w")) e->slim_w = atoi(v);
    else if (!strcmp(k, "slim_color")) {
        e->slim_color.a = 255;
        parse_color(v, &e->slim_color);
        e->has_slim_color = true;
    }
    else if (!strcmp(k, "slim")) {
        if (!strcasecmp(v, "bar")) e->slim = SLIM_BAR;
        else if (!strcasecmp(v, "clock")) e->slim = SLIM_CLOCK;
        else if (!strcasecmp(v, "tick")) e->slim = SLIM_TICK;
        else if (!strcasecmp(v, "presence") || !strcasecmp(v, "running") ||
                 !strcasecmp(v, "app")) e->slim = SLIM_PRESENCE;
        else if (!strcasecmp(v, "off") || !strcmp(v, "0")) e->slim = SLIM_OFF;
        else e->slim = SLIM_AUTO;
    }
    else if (!strcmp(k, "color") || !strcmp(k, "fg")) { parse_color(v, &e->color); e->has_color = true; }
    else if (!strcmp(k, "bg")) { e->bg.a = 255; parse_color(v, &e->bg); e->has_bg = true; }
    else if (!strcmp(k, "warn")) cell_threshold(v, &e->warn_th, &e->warn_dir, &e->warn_col, &e->has_warn);
    else if (!strcmp(k, "crit")) cell_threshold(v, &e->crit_th, &e->crit_dir, &e->crit_col, &e->has_crit);
    else if (!strcmp(k, "min_w") || !strcmp(k, "width")) e->min_w = atoi(v);
    else if (!strcmp(k, "max_w")) e->max_w = atoi(v);
    else if (!strcmp(k, "align")) str_set(e->align, sizeof(e->align), v);
    else if (!strcmp(k, "pos") || !strcmp(k, "place")) str_set(e->pos, sizeof(e->pos), v);
    else if (!strcmp(k, "scroll") || !strcmp(k, "text")) e->scroll = atoi(v);
    else if (!strcmp(k, "sep") || !strcmp(k, "separator")) e->sep = atoi(v);
    else if (!strcmp(k, "hide_empty")) e->hide_empty = atoi(v);
    else if (!strcmp(k, "markup")) e->markup = atoi(v);
    else if (!strncmp(k, "button", 6) && isdigit((unsigned char)k[6]) && !k[7]) {
        int b = k[6] - '0';
        if (b >= 1 && b < MAX_BINDS) str_set(e->bind[b], sizeof(e->bind[b]), v);
    }
    else fprintf(stderr, "swbr: unknown cell key '%s.%s'\n", name, k);
}

static void config_set(Config *c, const char *k, const char *v)
{
    const char *dot = strchr(k, '.');
    if (dot && dot != k) {                       /* NAME.field=value */
        char name[32];
        size_t n = (size_t)(dot - k);
        if (n >= sizeof(name)) n = sizeof(name) - 1;
        memcpy(name, k, n);
        name[n] = 0;
        cell_set(c, name, dot + 1, v);
        return;
    }
    if (!strcmp(k, "cell")) { cell_add(c, v); return; }
    if (!strcmp(k, "cell_gap")) { c->cell_gap = atoi(v); return; }
    if (!strcmp(k, "cell_inset")) { c->cell_inset = atoi(v); return; }
    if (!strcmp(k, "slim_ws_slots")) { c->slim_ws_slots = atoi(v); return; }
    if (!strcmp(k, "cell_radius")) { c->cell_radius = (float)atof(v); return; }
    if (!strcmp(k, "separator")) { str_set(c->separator, sizeof(c->separator), v); return; }
    if (!strcmp(k, "separator_color")) { parse_color(v, &c->separator_color); return; }

    if (!strcmp(k, "layer")) str_set(c->layer, sizeof(c->layer), v);
    else if (!strcmp(k, "position")) str_set(c->position, sizeof(c->position), v);
    else if (!strcmp(k, "height")) c->height = atoi(v);
    else if (!strcmp(k, "exclusive") || !strcmp(k, "reserve_space")) c->exclusive = atoi(v);
    else if (!strcmp(k, "margin")) c->margin = atoi(v);
    else if (!strcmp(k, "side_margin")) c->side_margin = atoi(v);
    else if (!strcmp(k, "min_width") || !strcmp(k, "width") ||
             !strcmp(k, "max_width")) c->min_width = atoi(v);
    else if (!strcmp(k, "align_x")) str_set(c->align_x, sizeof(c->align_x), v);
    else if (!strcmp(k, "radius")) c->radius = (float)atof(v);
    else if (!strcmp(k, "outputs")) str_set(c->outputs, sizeof(c->outputs), v);
    else if (!strcmp(k, "ws_names") || !strcmp(k, "workspace_names")) c->ws_names = atoi(v);
    else if (!strcmp(k, "ws_pad")) c->ws_pad = atoi(v);
    else if (!strcmp(k, "ws_gap")) c->ws_gap = atoi(v);
    else if (!strcmp(k, "ws_min_w")) c->ws_min_w = atoi(v);
    else if (!strcmp(k, "ws_inset")) c->ws_inset = atoi(v);
    else if (!strcmp(k, "ws_click")) c->ws_click = atoi(v);
    else if (!strcmp(k, "ws_radius")) c->ws_radius = (float)atof(v);
    else if (!strcmp(k, "scroll_workspace")) c->scroll_workspace = atoi(v);
    else if (!strcmp(k, "mode_show")) c->mode_show = atoi(v);
    else if (!strcmp(k, "font")) str_set(c->font, sizeof(c->font), v);
    else if (!strcmp(k, "font_alt")) str_set(c->font_alt, sizeof(c->font_alt), v);
    else if (!strcmp(k, "ui_scale") || !strcmp(k, "font_scale") || !strcmp(k, "text_scale"))
        c->ui_scale = (float)atof(v);
    else if (!strcmp(k, "text_px")) c->text_px = (float)atof(v);
    else if (!strcmp(k, "ws_px")) c->ws_px = (float)atof(v);
    else if (!strcmp(k, "pad_x")) c->pad_x = atoi(v);
    else if (!strcmp(k, "pad_right")) c->pad_right = atoi(v);
    else if (!strcmp(k, "scroll_speed")) c->scroll_speed = atoi(v);
    else if (!strcmp(k, "scroll_pause")) c->scroll_pause = atoi(v);
    else if (!strcmp(k, "text_y")) c->text_y = (float)atof(v);
    else if (!strcmp(k, "markup")) c->markup = atoi(v);
    else if (!strcmp(k, "status_command")) str_set(c->status_command, sizeof(c->status_command), v);
    else if (!strcmp(k, "interval")) c->interval = atoi(v);
    else if (!strcmp(k, "bar") || !strcmp(k, "layout"))
        str_set(c->layout, sizeof(c->layout), v);
    else if (!strcmp(k, "msg_fifo")) str_set(c->msg_fifo, sizeof(c->msg_fifo), v);
    else if (!strcmp(k, "msg_target")) str_set(c->msg_target, sizeof(c->msg_target), v);
    else if (!strcmp(k, "msg_timeout")) c->msg_timeout = atoi(v);
    else if (!strcmp(k, "msg_flash")) c->msg_flash = atoi(v);
    else if (!strcmp(k, "msg_info")) parse_color(v, &c->msg_info);
    else if (!strcmp(k, "msg_warn")) parse_color(v, &c->msg_warn);
    else if (!strcmp(k, "msg_error")) parse_color(v, &c->msg_error);
    else if (!strcmp(k, "hide_key")) str_set(c->hide_key, sizeof(c->hide_key), v);
    else if (!strcmp(k, "hide_keycode")) c->hide_keycode = atoi(v);
    else if (!strcmp(k, "hover_keys")) c->hover_keys = atoi(v);
    else if (!strcmp(k, "collapsed_px")) c->collapsed_px = atoi(v);
    else if (!strcmp(k, "anim_ms")) c->anim_ms = atoi(v);
    else if (!strcmp(k, "signals")) c->signals = atoi(v);
    else if (!strcmp(k, "start_collapsed")) c->start_collapsed = atoi(v);
    else if (!strncmp(k, "button", 6) && isdigit((unsigned char)k[6]) && !k[7]) {
        int b = k[6] - '0';
        if (b >= 1 && b < MAX_BINDS) str_set(c->bind[b], sizeof(c->bind[b]), v);
    }
    else if (!strcmp(k, "bg") || !strcmp(k, "background")) parse_color(v, &c->bg);
    else if (!strcmp(k, "text") || !strcmp(k, "statusline")) parse_color(v, &c->text);
    else if (!strcmp(k, "dim")) parse_color(v, &c->dim);
    else if (!strcmp(k, "accent")) parse_color(v, &c->accent);
    else if (!strcmp(k, "hl")) parse_color(v, &c->hl);
    else if (!strcmp(k, "urgent")) parse_color(v, &c->urgent);
    else if (!strcmp(k, "running")) parse_color(v, &c->running);
    else if (!strcmp(k, "outline")) parse_color(v, &c->outline);
    else if (!strcmp(k, "ws_bg")) parse_color(v, &c->ws_bg);
    else if (!strcmp(k, "ws_fg")) parse_color(v, &c->ws_fg);
    else if (!strcmp(k, "ws_focused_bg")) parse_color(v, &c->ws_focused_bg);
    else if (!strcmp(k, "ws_focused_fg")) parse_color(v, &c->ws_focused_fg);
    else if (!strcmp(k, "ws_visible_bg")) parse_color(v, &c->ws_visible_bg);
    else if (!strcmp(k, "ws_visible_fg")) parse_color(v, &c->ws_visible_fg);
    else if (!strcmp(k, "ws_urgent_bg")) parse_color(v, &c->ws_urgent_bg);
    else if (!strcmp(k, "ws_urgent_fg")) parse_color(v, &c->ws_urgent_fg);
    else if (!strcmp(k, "mode_bg")) parse_color(v, &c->mode_bg);
    else if (!strcmp(k, "mode_fg")) parse_color(v, &c->mode_fg);
    else fprintf(stderr, "swbr: unknown key '%s'\n", k);
}

/* Cut a trailing "  # comment" off a value. Quotes are respected, so the
 * '#' inside  foreground="#19cb00"  or inside a quoted sed script survives.
 * Only a '#'
 * that follows whitespace starts a comment, so  fmt=%s#  is safe too. */
static void strip_comment(char *v)
{
    char q = 0;
    for (char *p = v; *p; ++p) {
        if (q) { if (*p == q) q = 0; continue; }
        if (*p == '\'' || *p == '"') { q = *p; continue; }
        if (*p == '#' && p > v && isspace((unsigned char)p[-1])) { *p = 0; return; }
    }
}

/* the shared ~/.config/sw/config, translated into swbr's own keys */
static void config_set_shared(void *ud, const char *k, const char *v)
{
    config_set((Config *)ud, k, v);
}

static void config_load(Config *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "swbr: no config at %s\n", path); return; }
    char line[8192];
    while (fgets(line, sizeof(line), f)) {
        char *s = trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *v = eq + 1;
        strip_comment(v);
        config_set(c, trim(s), trim(v));
    }
    fclose(f);
}

static Config cfg;

/* ------------------------------------------------------------------ text */
/* stb_truetype, rasterised straight into our own ARGB buffer. Glyphs are
 * cached per size on first use, so any codepoint works — the status line is
 * full of box drawing characters and symbols. */

typedef struct {
    unsigned char *data;
    stbtt_fontinfo info;
    bool ok;
} FontFile;

static FontFile font_pri, font_alt;

typedef struct {
    unsigned cp;                 /* 0 = free slot */
    int w, h, x0, y0;
    float adv;
    unsigned char *bmp;
} Glyph;

typedef struct {
    int px;
    float sc_pri, sc_alt, ascent, lineh;
    Glyph *tab;
    int cap, n;
    bool used;
} Font;

#define FONT_CACHE 12
static Font font_cache[FONT_CACHE];

static bool font_file_load(FontFile *ff, const char *path)
{
    memset(ff, 0, sizeof(*ff));
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return false; }
    ff->data = (unsigned char *)xmalloc((size_t)sz);
    if (fread(ff->data, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(ff->data); ff->data = NULL; return false;
    }
    fclose(f);
    if (!stbtt_InitFont(&ff->info, ff->data, stbtt_GetFontOffsetForIndex(ff->data, 0))) {
        free(ff->data); ff->data = NULL; return false;
    }
    ff->ok = true;
    return true;
}

static bool try_font_paths(char *out, size_t n)
{
    const char *cand[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/TTF/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/noto/NotoSansMono-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeMono.ttf", NULL
    };
    for (int i = 0; cand[i]; ++i)
        if (file_exists(cand[i])) { snprintf(out, n, "%s", cand[i]); return true; }
    return false;
}

/* Ask fontconfig (via its fc-match CLI) to resolve a family or pattern to an
 * actual .ttf path. No library is linked; if fc-match is missing we fall back
 * to the known paths above. */
static bool fc_match(const char *pattern, char *out, size_t n)
{
    if (!pattern || !*pattern) pattern = "monospace";
    char safe[256];
    size_t j = 0;
    for (size_t i = 0; pattern[i] && j < sizeof(safe) - 1; ++i) {
        char ch = pattern[i];
        if (ch == '"' || ch == '`' || ch == '$' || ch == '\\' || ch == ';' ||
            ch == '|' || ch == '&' || ch == '\n' || ch == '\r') continue;
        safe[j++] = ch;
    }
    safe[j] = 0;
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "fc-match --format=%%{file} \"%s\" 2>/dev/null", safe);
    FILE *p = popen(cmd, "r");
    if (!p) return false;
    char buf[PATH_MAX];
    size_t r = fread(buf, 1, sizeof(buf) - 1, p);
    buf[r] = 0;
    pclose(p);
    char *s = trim(buf);
    if (*s && file_exists(s)) { snprintf(out, n, "%s", s); return true; }
    return false;
}

static char font_path_used[PATH_MAX];
static char font_alt_used[PATH_MAX];
static void fonts_init(void)
{
    char path[PATH_MAX] = "";
    if (*cfg.font) {
        expand_tilde(cfg.font, path, sizeof(path));
        if (!file_exists(path)) fc_match(cfg.font, path, sizeof(path));
    }
    if (!*path || !file_exists(path)) fc_match("monospace", path, sizeof(path));
    if (!*path || !file_exists(path)) try_font_paths(path, sizeof(path));
    if (*path) { font_file_load(&font_pri, path); str_set(font_path_used, PATH_MAX, path); }
    if (!font_pri.ok) fprintf(stderr, "swbr: no usable font found\n");

    char alt[PATH_MAX] = "";
    if (*cfg.font_alt) {
        expand_tilde(cfg.font_alt, alt, sizeof(alt));
        if (!file_exists(alt)) fc_match(cfg.font_alt, alt, sizeof(alt));
    } else {
        /* a symbol font, if the desktop has one — covers glyphs a mono font
         * usually lacks. Silently skipped when absent. */
        if (!fc_match("Noto Sans Symbols 2", alt, sizeof(alt)))
            fc_match("Noto Sans Symbols2", alt, sizeof(alt));
    }
    if (*alt && file_exists(alt) && (!*path || strcmp(alt, path))) {
        font_file_load(&font_alt, alt);
        str_set(font_alt_used, PATH_MAX, alt);
    }
}

static void glyph_tab_init(Font *f, int cap)
{
    f->cap = cap;
    f->n = 0;
    f->tab = (Glyph *)xmalloc((size_t)cap * sizeof(Glyph));
    memset(f->tab, 0, (size_t)cap * sizeof(Glyph));
}

static Glyph *glyph_slot(Font *f, unsigned cp)
{
    unsigned h = cp * 2654435761u;
    for (;;) {
        unsigned i = h & (unsigned)(f->cap - 1);
        for (int probe = 0; probe < f->cap; ++probe) {
            Glyph *g = &f->tab[(i + (unsigned)probe) & (unsigned)(f->cap - 1)];
            if (g->cp == cp) return g;
            if (g->cp == 0) return g;
        }
        /* full: grow and rehash */
        Glyph *old = f->tab;
        int oldcap = f->cap;
        glyph_tab_init(f, oldcap * 2);
        for (int k = 0; k < oldcap; ++k) {
            if (!old[k].cp) continue;
            Glyph *g = glyph_slot(f, old[k].cp);
            *g = old[k];
            f->n++;
        }
        free(old);
    }
}

static Glyph *glyph_get(Font *f, unsigned cp)
{
    Glyph *g = glyph_slot(f, cp);
    if (g->cp == cp) return g;

    FontFile *ff = &font_pri;
    float sc = f->sc_pri;
    int gi = font_pri.ok ? stbtt_FindGlyphIndex(&font_pri.info, (int)cp) : 0;
    if (!gi && font_alt.ok) {
        int gi2 = stbtt_FindGlyphIndex(&font_alt.info, (int)cp);
        if (gi2) { ff = &font_alt; sc = f->sc_alt; gi = gi2; }
    }
    memset(g, 0, sizeof(*g));
    g->cp = cp;
    f->n++;
    if (!ff->ok || !gi) {
        g->adv = (float)f->px * 0.5f;      /* missing: leave a blank */
        return g;
    }
    int aw, lsb;
    stbtt_GetGlyphHMetrics(&ff->info, gi, &aw, &lsb);
    g->adv = (float)aw * sc;
    int x0, y0, x1, y1;
    stbtt_GetGlyphBitmapBox(&ff->info, gi, sc, sc, &x0, &y0, &x1, &y1);
    int w = x1 - x0, h = y1 - y0;
    if (w > 0 && h > 0 && w < 512 && h < 512) {
        g->bmp = (unsigned char *)xmalloc((size_t)w * (size_t)h);
        stbtt_MakeGlyphBitmap(&ff->info, g->bmp, w, h, w, sc, sc, gi);
        g->w = w; g->h = h; g->x0 = x0; g->y0 = y0;
    }
    return g;
}

/* One Font per pixel size (and therefore per output scale). */
static Font *font_get(int px)
{
    if (px < 4) px = 4;
    for (int i = 0; i < FONT_CACHE; ++i)
        if (font_cache[i].used && font_cache[i].px == px) return &font_cache[i];

    int slot = -1;
    for (int i = 0; i < FONT_CACHE; ++i) if (!font_cache[i].used) { slot = i; break; }
    if (slot < 0) slot = 0;                       /* recycle the first one */
    Font *f = &font_cache[slot];
    if (f->used) {
        for (int k = 0; k < f->cap; ++k) free(f->tab[k].bmp);
        free(f->tab);
        memset(f, 0, sizeof(*f));
    }
    f->used = true;
    f->px = px;
    f->sc_pri = font_pri.ok ? stbtt_ScaleForPixelHeight(&font_pri.info, (float)px) : 1.0f;
    f->sc_alt = font_alt.ok ? stbtt_ScaleForPixelHeight(&font_alt.info, (float)px) : 1.0f;
    int asc = 0, desc = 0, gap = 0;
    if (font_pri.ok) stbtt_GetFontVMetrics(&font_pri.info, &asc, &desc, &gap);
    f->ascent = (float)asc * f->sc_pri;
    f->lineh = (float)(asc - desc) * f->sc_pri;
    if (f->ascent <= 0) { f->ascent = (float)px * 0.8f; f->lineh = (float)px; }
    glyph_tab_init(f, 256);
    return f;
}

static unsigned utf8_next(const char **p)
{
    const unsigned char *s = (const unsigned char *)*p;
    unsigned cp;
    int n;
    if (s[0] < 0x80) { cp = s[0]; n = 1; }
    else if ((s[0] & 0xE0) == 0xC0) { cp = s[0] & 0x1Fu; n = 2; }
    else if ((s[0] & 0xF0) == 0xE0) { cp = s[0] & 0x0Fu; n = 3; }
    else if ((s[0] & 0xF8) == 0xF0) { cp = s[0] & 0x07u; n = 4; }
    else { *p = (const char *)(s + 1); return 0xFFFD; }
    for (int i = 1; i < n; ++i) {
        if ((s[i] & 0xC0) != 0x80) { n = i; break; }
        cp = (cp << 6) | (s[i] & 0x3Fu);
    }
    *p = (const char *)(s + n);
    return cp;
}

static float text_width(Font *f, const char *s)
{
    float w = 0;
    const char *p = s;
    while (*p) {
        unsigned cp = utf8_next(&p);
        if (cp == 0xFE0F || cp == 0x200D) continue;   /* variation selectors */
        w += glyph_get(f, cp)->adv;
    }
    return w;
}

/* ----------------------------------------------------------------- paint */
/* ARGB8888, premultiplied — that is what wl_shm wants. */

typedef struct {
    uint32_t *px;
    int w, h;
    int cx0, cy0, cx1, cy1;      /* clip rectangle */
} Canvas;

static void canvas_clip(Canvas *cv, int x0, int y0, int x1, int y1)
{
    cv->cx0 = clampi(x0, 0, cv->w);
    cv->cy0 = clampi(y0, 0, cv->h);
    cv->cx1 = clampi(x1, 0, cv->w);
    cv->cy1 = clampi(y1, 0, cv->h);
}

static void canvas_clip_all(Canvas *cv) { canvas_clip(cv, 0, 0, cv->w, cv->h); }

static inline void blend(Canvas *cv, int x, int y, Col c, float cov)
{
    if (x < cv->cx0 || x >= cv->cx1 || y < cv->cy0 || y >= cv->cy1) return;
    float a = (float)c.a / 255.0f * cov;
    if (a <= 0.0f) return;
    uint32_t d = cv->px[y * cv->w + x];
    float da = (float)((d >> 24) & 0xff) / 255.0f;
    float dr = (float)((d >> 16) & 0xff) / 255.0f;
    float dg = (float)((d >> 8) & 0xff) / 255.0f;
    float db = (float)(d & 0xff) / 255.0f;
    float sr = (float)c.r / 255.0f * a;
    float sg = (float)c.g / 255.0f * a;
    float sb = (float)c.b / 255.0f * a;
    float ia = 1.0f - a;
    float orr = sr + dr * ia, og = sg + dg * ia, ob = sb + db * ia, oa = a + da * ia;
    cv->px[y * cv->w + x] =
        ((uint32_t)(orr * 255.0f + 0.5f) << 16) |
        ((uint32_t)(og  * 255.0f + 0.5f) << 8)  |
        ((uint32_t)(ob  * 255.0f + 0.5f))       |
        ((uint32_t)(oa  * 255.0f + 0.5f) << 24);
}

static void fill_rect(Canvas *cv, float x, float y, float w, float h, Col c)
{
    if (w <= 0 || h <= 0 || !c.a) return;
    int x0 = (int)floorf(x), y0 = (int)floorf(y);
    int x1 = (int)ceilf(x + w), y1 = (int)ceilf(y + h);
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx) {
            float cxs = clampf(x + w, (float)xx, (float)xx + 1) - clampf(x, (float)xx, (float)xx + 1);
            float cys = clampf(y + h, (float)yy, (float)yy + 1) - clampf(y, (float)yy, (float)yy + 1);
            blend(cv, xx, yy, c, cxs * cys);
        }
}

/* Rounded box with a radius per corner (tl, tr, br, bl), anti-aliased through
 * the usual signed distance trick. A corner radius of 0 is a square corner —
 * that is how the bar stays flush with the screen edge. */
static void fill_round(Canvas *cv, float x, float y, float w, float h,
                       float rtl, float rtr, float rbr, float rbl, Col c)
{
    if (w <= 0 || h <= 0 || !c.a) return;
    float hw = w * 0.5f, hh = h * 0.5f;
    float ccx = x + hw, ccy = y + hh;
    float cap = fminf(hw, hh);
    rtl = clampf(rtl, 0, cap); rtr = clampf(rtr, 0, cap);
    rbr = clampf(rbr, 0, cap); rbl = clampf(rbl, 0, cap);

    int x0 = (int)floorf(x) - 1, y0 = (int)floorf(y) - 1;
    int x1 = (int)ceilf(x + w) + 1, y1 = (int)ceilf(y + h) + 1;
    for (int yy = y0; yy < y1; ++yy) {
        if (yy < cv->cy0 || yy >= cv->cy1) continue;
        for (int xx = x0; xx < x1; ++xx) {
            if (xx < cv->cx0 || xx >= cv->cx1) continue;
            float px = (float)xx + 0.5f - ccx, py = (float)yy + 0.5f - ccy;
            float r = px < 0 ? (py < 0 ? rtl : rbl) : (py < 0 ? rtr : rbr);
            float qx = fabsf(px) - (hw - r), qy = fabsf(py) - (hh - r);
            float mx = fmaxf(qx, 0.0f), my = fmaxf(qy, 0.0f);
            float d = sqrtf(mx * mx + my * my) + fminf(fmaxf(qx, qy), 0.0f) - r;
            float cov = clampf(0.5f - d, 0.0f, 1.0f);
            if (cov > 0) blend(cv, xx, yy, c, cov);
        }
    }
}

static void text_draw(Canvas *cv, Font *f, float x, float baseline, Col c, const char *s)
{
    const char *p = s;
    float pen = x;
    while (*p) {
        unsigned cp = utf8_next(&p);
        if (cp == 0xFE0F || cp == 0x200D) continue;
        Glyph *g = glyph_get(f, cp);
        if (g->bmp) {
            int gx = (int)lroundf(pen) + g->x0;
            int gy = (int)lroundf(baseline) + g->y0;
            for (int yy = 0; yy < g->h; ++yy) {
                int ty = gy + yy;
                if (ty < cv->cy0 || ty >= cv->cy1) continue;
                const unsigned char *row = g->bmp + (size_t)yy * (size_t)g->w;
                for (int xx = 0; xx < g->w; ++xx) {
                    unsigned char a = row[xx];
                    if (a) blend(cv, gx + xx, ty, c, (float)a / 255.0f);
                }
            }
        }
        pen += g->adv;
    }
}

/* ---------------------------------------------------------------- markup */
/* The subset of pango markup swaybar users actually write:
 *   <span foreground="#rrggbb" background="#..">..</span>, <b>, <i>, <u>, <tt>
 *   and the five XML entities. Anything else is passed through as text. */

typedef struct {
    char text[512];
    Col  fg, bg;
    bool has_fg, has_bg;
} Run;

typedef struct {
    Run v[MAX_RUNS];
    int n;
} Runs;

typedef struct { Col fg, bg; bool has_fg, has_bg; } Style;

static void runs_push(Runs *rs, const char *buf, int len, Style *st)
{
    if (len <= 0 || rs->n >= MAX_RUNS) return;
    Run *r = &rs->v[rs->n++];
    memset(r, 0, sizeof(*r));
    if (len > (int)sizeof(r->text) - 1) len = (int)sizeof(r->text) - 1;
    memcpy(r->text, buf, (size_t)len);
    r->text[len] = 0;
    r->fg = st->fg; r->bg = st->bg;
    r->has_fg = st->has_fg; r->has_bg = st->has_bg;
}

static void entity_append(char *buf, int *len, int cap, const char *ent, int elen)
{
    char tmp[8];
    const char *out = NULL;
    if (elen == 3 && !strncmp(ent, "amp", 3)) out = "&";
    else if (elen == 2 && !strncmp(ent, "lt", 2)) out = "<";
    else if (elen == 2 && !strncmp(ent, "gt", 2)) out = ">";
    else if (elen == 4 && !strncmp(ent, "quot", 4)) out = "\"";
    else if (elen == 4 && !strncmp(ent, "apos", 4)) out = "'";
    else if (elen > 1 && ent[0] == '#') {
        bool hex = (ent[1] == 'x' || ent[1] == 'X');
        unsigned cp = (unsigned)strtoul(ent + (hex ? 2 : 1), NULL, hex ? 16 : 10);
        int n = 0;
        if (cp && cp < 0x110000) {
            if (cp < 0x80) tmp[n++] = (char)cp;
            else if (cp < 0x800) {
                tmp[n++] = (char)(0xC0 | (cp >> 6));
                tmp[n++] = (char)(0x80 | (cp & 0x3F));
            } else if (cp < 0x10000) {
                tmp[n++] = (char)(0xE0 | (cp >> 12));
                tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                tmp[n++] = (char)(0x80 | (cp & 0x3F));
            } else {
                tmp[n++] = (char)(0xF0 | (cp >> 18));
                tmp[n++] = (char)(0x80 | ((cp >> 12) & 0x3F));
                tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
                tmp[n++] = (char)(0x80 | (cp & 0x3F));
            }
        }
        tmp[n] = 0;
        out = tmp;
    }
    if (!out) return;
    int n = (int)strlen(out);
    if (*len + n < cap) { memcpy(buf + *len, out, (size_t)n); *len += n; }
}

static void tag_attr(const char *tag, const char *name, char *out, size_t cap)
{
    *out = 0;
    size_t nl = strlen(name);
    for (const char *p = tag; *p; ++p) {
        if (strncasecmp(p, name, nl)) continue;
        const char *q = p + nl;
        while (*q == ' ') q++;
        if (*q != '=') continue;
        q++;
        while (*q == ' ') q++;
        char quote = 0;
        if (*q == '"' || *q == '\'') quote = *q++;
        size_t j = 0;
        while (*q && j + 1 < cap && (quote ? *q != quote : (*q != ' ' && *q != '>')))
            out[j++] = *q++;
        out[j] = 0;
        return;
    }
}

static void markup_parse(const char *s, Runs *rs, Col def_fg)
{
    rs->n = 0;
    Style stack[16];
    int sp = 0;
    stack[0] = (Style){ def_fg, def_fg, false, false };

    char buf[512];
    int len = 0;

    if (!cfg.markup) {
        Style st = stack[0];
        runs_push(rs, s, (int)strlen(s), &st);
        return;
    }

    for (const char *p = s; *p; ) {
        if (*p == '<') {
            const char *end = strchr(p, '>');
            if (!end) { if (len < (int)sizeof(buf) - 1) buf[len++] = *p++; continue; }
            char tag[512];
            size_t tl = (size_t)(end - p - 1);
            if (tl >= sizeof(tag)) tl = sizeof(tag) - 1;
            memcpy(tag, p + 1, tl);
            tag[tl] = 0;
            p = end + 1;

            runs_push(rs, buf, len, &stack[sp]);
            len = 0;

            if (tag[0] == '/') {
                if (sp > 0) sp--;
            } else if (!strncasecmp(tag, "span", 4)) {
                Style st = stack[sp];
                char val[64];
                tag_attr(tag, "foreground", val, sizeof(val));
                if (!*val) tag_attr(tag, "fgcolor", val, sizeof(val));
                if (!*val) tag_attr(tag, "color", val, sizeof(val));
                if (*val) { st.fg = def_fg; st.fg.a = 255; parse_color(val, &st.fg); st.has_fg = true; }
                tag_attr(tag, "background", val, sizeof(val));
                if (!*val) tag_attr(tag, "bgcolor", val, sizeof(val));
                if (*val) { st.bg.a = 255; parse_color(val, &st.bg); st.has_bg = true; }
                if (sp < 15) stack[++sp] = st;
            } else {
                if (sp < 15) stack[sp + 1] = stack[sp], sp++;   /* <b>, <i>, .. */
            }
            continue;
        }
        if (*p == '&') {
            const char *semi = strchr(p, ';');
            if (semi && semi - p < 12) {
                entity_append(buf, &len, (int)sizeof(buf), p + 1, (int)(semi - p - 1));
                p = semi + 1;
                continue;
            }
        }
        if (len < (int)sizeof(buf) - 1) buf[len++] = *p;
        p++;
    }
    runs_push(rs, buf, len, &stack[sp]);
}

/* ---------------------------------------------------------------- status */
/* status_command runs under sh. Every line it prints replaces the status
 * text. If the command exits (a one-shot script), it is started again after
 * `interval` seconds — so both styles work: a script that loops forever, and
 * a script that prints once and quits. */

static pid_t   status_pid = -1;
static int     status_fd = -1;
static char    status_line[4096] = "";
static char    status_buf[8192];
static size_t  status_len = 0;
static uint32_t status_next_spawn = 0;

static void status_spawn(void)
{
    if (!*cfg.status_command) return;
    int fds[2];
    if (pipe(fds) < 0) return;

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        if (fds[1] != STDOUT_FILENO) close(fds[1]);
        setsid();
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", "-c", cfg.status_command, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    int fl = fcntl(fds[0], F_GETFL, 0);
    if (fl >= 0) fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    status_fd = fds[0];
    status_pid = pid;
    status_len = 0;
}

static void status_stop(void)
{
    if (status_pid > 0) { kill(-status_pid, SIGTERM); waitpid(status_pid, NULL, WNOHANG); }
    if (status_fd >= 0) close(status_fd);
    status_pid = -1;
    status_fd = -1;
}

/* true when a new complete line arrived */
static bool status_read(void)
{
    if (status_fd < 0) return false;
    bool got = false;
    for (;;) {
        ssize_t r = read(status_fd, status_buf + status_len,
                         sizeof(status_buf) - status_len - 1);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;                                   /* EAGAIN */
        }
        if (r == 0) {                                /* child closed stdout */
            close(status_fd);
            status_fd = -1;
            if (status_pid > 0) { waitpid(status_pid, NULL, 0); status_pid = -1; }
            status_next_spawn = now_ms() + (uint32_t)(cfg.interval > 0 ? cfg.interval * 1000 : 1000);
            break;
        }
        status_len += (size_t)r;
        status_buf[status_len] = 0;

        char *nl;
        while ((nl = memchr(status_buf, '\n', status_len)) != NULL) {
            *nl = 0;
            str_set(status_line, sizeof(status_line), status_buf);
            got = true;
            size_t used = (size_t)(nl - status_buf) + 1;
            memmove(status_buf, status_buf + used, status_len - used);
            status_len -= used;
            status_buf[status_len] = 0;
        }
        if (status_len >= sizeof(status_buf) - 1) status_len = 0;   /* runaway line */
    }
    return got;
}

/* run a mouse binding, detached */
static void run_command(const char *cmd)
{
    if (!cmd || !*cmd) return;
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        setsid();
        if (fork() != 0) _exit(0);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    waitpid(pid, NULL, 0);
}




/* ---------------------------------------------------------------- layout */
/* One line decides what sits where:
 *
 *     bar={workspaces||cmus,volume,(clock,date)}
 *
 * "||" splits groups — one part is left only, two are left and right, three
 * are left, centre and right. "," separates items. Parentheses glue items
 * together with no separator between them. Names are cell names plus the two
 * built-ins, "workspaces" and "mode". Because it assigns position, order and
 * separators, put this line last in the config. */

enum { IT_CELL, IT_WS, IT_MODE };
enum { G_LEFT, G_CENTER, G_RIGHT, G_COUNT };

typedef struct { int kind, cell, sep; } Item;

static Item layout_item[G_COUNT][MAX_CELLS + 4];
static int  layout_n[G_COUNT];

static void layout_push(int g, int kind, int cell, int sep)
{
    if (layout_n[g] >= MAX_CELLS + 4) return;
    Item *it = &layout_item[g][layout_n[g]++];
    it->kind = kind;
    it->cell = cell;
    it->sep = sep;
}

static void layout_default(void)
{
    for (int g = 0; g < G_COUNT; ++g) layout_n[g] = 0;
    layout_push(G_LEFT, IT_WS, -1, 0);
    layout_push(G_LEFT, IT_MODE, -1, 0);
    for (int i = 0; i < cfg.cell_count; ++i) {
        int g = !strcmp(cfg.cell[i].pos, "left") ? G_LEFT
              : !strcmp(cfg.cell[i].pos, "center") ? G_CENTER : G_RIGHT;
        layout_push(g, IT_CELL, i, cfg.cell[i].sep);
    }
}

static void layout_name(int g, char *name, bool joined)
{
    char *n = trim(name);
    if (!*n) return;
    if (!strcasecmp(n, "workspaces") || !strcasecmp(n, "ws")) {
        layout_push(g, IT_WS, -1, joined ? 0 : 1);
        return;
    }
    if (!strcasecmp(n, "mode")) { layout_push(g, IT_MODE, -1, joined ? 0 : 1); return; }
    for (int i = 0; i < cfg.cell_count; ++i)
        if (!strcmp(cfg.cell[i].name, n)) {
            cfg.cell[i].sep = joined ? 0 : 1;
            layout_push(g, IT_CELL, i, cfg.cell[i].sep);
            return;
        }
    fprintf(stderr, "swbr: bar= names unknown item '%s'\n", n);
}

static void layout_parse(const char *spec)
{
    if (!*spec) { layout_default(); return; }
    for (int g = 0; g < G_COUNT; ++g) layout_n[g] = 0;

    char buf[1024];
    str_set(buf, sizeof(buf), spec);
    char *p = trim(buf);
    if (*p == '{') p++;
    char *end = p + strlen(p);
    while (end > p && (end[-1] == '}' || isspace((unsigned char)end[-1]))) *--end = 0;

    /* split the groups on "||" */
    char *parts[G_COUNT] = { NULL, NULL, NULL };
    int np = 0;
    parts[np++] = p;
    for (char *q = p; *q && np < G_COUNT; ++q)
        if (q[0] == '|' && q[1] == '|') { *q = 0; parts[np++] = q + 2; }

    int gmap1[1] = { G_LEFT };
    int gmap2[2] = { G_LEFT, G_RIGHT };
    int gmap3[3] = { G_LEFT, G_CENTER, G_RIGHT };
    int *gmap = np == 1 ? gmap1 : (np == 2 ? gmap2 : gmap3);

    for (int i = 0; i < np; ++i) {
        int g = gmap[i];
        char name[64];
        int nl = 0;
        bool joined = false;
        for (char *q = parts[i]; ; ++q) {
            if (*q == '(') { joined = true; nl = 0; continue; }
            if (*q == ',' || *q == ')' || *q == 0) {
                name[nl < (int)sizeof(name) ? nl : (int)sizeof(name) - 1] = 0;
                bool last_of_group = (*q == ')');
                if (nl) layout_name(g, name, joined && !last_of_group);
                nl = 0;
                if (*q == ')') joined = false;
                if (*q == 0) break;
                continue;
            }
            if (nl < (int)sizeof(name) - 1) name[nl++] = *q;
        }
    }
}

/* ---------------------------------------------------------------- message */
/* A named pipe the rest of the system can shout into:
 *     echo "warn: backup is running" > $XDG_RUNTIME_DIR/swbr.fifo
 *     swbr --msg "error: disk full"
 *     swbr --msg clear
 * The pipe is opened read-write so it never reports end of file when the
 * writer goes away. A message can take over a cell (msg_target), which is how
 * the music title turns into the warning while one is active. */

static char msg_text[512] = "";
static int  msg_level = 0;                    /* 0 info, 1 warn, 2 error */
static uint32_t msg_until = 0;                /* 0 = stays until cleared */
static char msg_path[PATH_MAX] = "";
static int  msg_fd = -1;
static char msg_buf[1024];
static size_t msg_len = 0;

static void msg_resolve_path(void)
{
    if (*cfg.msg_fifo) { expand_tilde(cfg.msg_fifo, msg_path, sizeof(msg_path)); return; }
    const char *rd = getenv("XDG_RUNTIME_DIR");
    if (rd && *rd) snprintf(msg_path, sizeof(msg_path), "%s/swbr.fifo", rd);
    else snprintf(msg_path, sizeof(msg_path), "/tmp/swbr-%d.fifo", (int)getuid());
}

static bool msg_active(void)
{
    if (!*msg_text) return false;
    if (msg_until && (int32_t)(now_ms() - msg_until) >= 0) return false;
    return true;
}

static Col msg_color(void)
{
    return msg_level >= 2 ? cfg.msg_error : (msg_level == 1 ? cfg.msg_warn : cfg.msg_info);
}

static void msg_set(const char *line)
{
    const char *t = line;
    int lvl = 0;
    if (!strncasecmp(t, "error:", 6))        { lvl = 2; t += 6; }
    else if (!strncasecmp(t, "err:", 4))     { lvl = 2; t += 4; }
    else if (!strncasecmp(t, "warning:", 8)) { lvl = 1; t += 8; }
    else if (!strncasecmp(t, "warn:", 5))    { lvl = 1; t += 5; }
    else if (!strncasecmp(t, "info:", 5))    { lvl = 0; t += 5; }
    while (*t == ' ' || *t == '\t') t++;

    if (!*t || !strcasecmp(t, "clear") || !strcasecmp(t, "-")) {
        msg_text[0] = 0;
        msg_until = 0;
        return;
    }
    str_set(msg_text, sizeof(msg_text), t);
    msg_level = lvl;
    msg_until = cfg.msg_timeout > 0 ? now_ms() + (uint32_t)cfg.msg_timeout * 1000u : 0;
}

static void msg_open(void)
{
    msg_resolve_path();
    struct stat st;
    if (stat(msg_path, &st) == 0 && !S_ISFIFO(st.st_mode)) {
        fprintf(stderr, "swbr: %s exists and is not a fifo\n", msg_path);
        return;
    }
    if (mkfifo(msg_path, 0600) < 0 && errno != EEXIST) {
        fprintf(stderr, "swbr: cannot create %s: %s\n", msg_path, strerror(errno));
        return;
    }
    msg_fd = open(msg_path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
}

static bool msg_read(void)
{
    if (msg_fd < 0) return false;
    bool got = false;
    for (;;) {
        ssize_t r = read(msg_fd, msg_buf + msg_len, sizeof(msg_buf) - msg_len - 1);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        msg_len += (size_t)r;
        msg_buf[msg_len] = 0;
        char *nl;
        while ((nl = memchr(msg_buf, '\n', msg_len)) != NULL) {
            size_t used = (size_t)(nl - msg_buf) + 1;
            *nl = 0;
            msg_set(trim(msg_buf));
            memmove(msg_buf, msg_buf + used, msg_len - used);
            msg_len -= used;
            msg_buf[msg_len] = 0;
            got = true;
        }
        if (msg_len >= sizeof(msg_buf) - 1) msg_len = 0;
    }
    return got;
}

/* swbr --msg TEXT : hand the line to the running bar and exit */
static int msg_send(const char *text)
{
    msg_resolve_path();
    int fd = open(msg_path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "swbr: no bar listening on %s (%s)\n", msg_path, strerror(errno));
        return 1;
    }
    char line[600];
    snprintf(line, sizeof(line), "%s\n", text);
    ssize_t w = write(fd, line, strlen(line));
    close(fd);
    return w > 0 ? 0 : 1;
}

/* ------------------------------------------------------------ cell runtime */
/* Every cell is its own little process on its own schedule. They are read
 * without blocking, so a slow command never stalls the bar — its old value
 * just stays on screen until the new one arrives. */

static void cell_spawn(Cell *e)
{
    if (!*e->cmd || e->pid > 0) return;
    int fds[2];
    if (pipe(fds) < 0) return;
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return; }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        if (fds[1] != STDOUT_FILENO) close(fds[1]);
        setsid();
        signal(SIGPIPE, SIG_DFL);
        execl("/bin/sh", "sh", "-c", e->cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    int fl = fcntl(fds[0], F_GETFL, 0);
    if (fl >= 0) fcntl(fds[0], F_SETFL, fl | O_NONBLOCK);
    e->fd = fds[0];
    e->pid = pid;
    e->len = 0;
}

static void cell_finish(Cell *e)
{
    e->buf[e->len] = 0;
    char *t = trim(e->buf);
    for (char *q = t; *q; ++q) if (*q == '\n' || *q == '\r' || *q == '\t') *q = ' ';
    str_set(e->out, sizeof(e->out), t);
    e->len = 0;
}

/* true when a cell got a new value */
static bool cells_read(void)
{
    bool got = false;
    for (int i = 0; i < cfg.cell_count; ++i) {
        Cell *e = &cfg.cell[i];
        if (e->fd < 0) continue;
        for (;;) {
            ssize_t r = read(e->fd, e->buf + e->len, sizeof(e->buf) - e->len - 1);
            if (r < 0) { if (errno == EINTR) continue; break; }
            if (r == 0) {                                   /* command finished */
                cell_finish(e);
                close(e->fd);
                e->fd = -1;
                if (e->pid > 0) { waitpid(e->pid, NULL, 0); e->pid = -1; }
                e->next_run = now_ms() +
                    (uint32_t)(e->interval > 0 ? e->interval * 1000 : 1000);
                got = true;
                break;
            }
            e->len += (size_t)r;
            e->buf[e->len] = 0;

            if (e->interval < 0) {                          /* streaming */
                char *nl;
                while ((nl = memchr(e->buf, '\n', e->len)) != NULL) {
                    size_t used = (size_t)(nl - e->buf) + 1;
                    *nl = 0;
                    str_set(e->out, sizeof(e->out), trim(e->buf));
                    memmove(e->buf, e->buf + used, e->len - used);
                    e->len -= used;
                    e->buf[e->len] = 0;
                    got = true;
                }
            }
            if (e->len >= sizeof(e->buf) - 1) { cell_finish(e); got = true; }
        }
    }
    return got;
}

static void cells_tick(void)
{
    uint32_t t = now_ms();
    for (int i = 0; i < cfg.cell_count; ++i) {
        Cell *e = &cfg.cell[i];
        if (e->pid > 0 || !*e->cmd) continue;
        if (e->interval == 0 && e->next_run) continue;       /* run once */
        if (e->next_run && (int32_t)(t - e->next_run) < 0) continue;
        e->next_run = t + 1;
        cell_spawn(e);
    }
}

static void cells_stop(void)
{
    for (int i = 0; i < cfg.cell_count; ++i) {
        Cell *e = &cfg.cell[i];
        if (e->pid > 0) { kill(-e->pid, SIGTERM); waitpid(e->pid, NULL, WNOHANG); }
        if (e->fd >= 0) close(e->fd);
        e->pid = -1;
        e->fd = -1;
    }
}

/* the text a cell draws: its output run through fmt */
static void cell_text(Cell *e, char *out, size_t cap)
{
    if (!*e->fmt) { str_set(out, cap, e->out); return; }
    const char *pc = strstr(e->fmt, "%s");
    if (!pc) { str_set(out, cap, e->fmt); return; }
    snprintf(out, cap, "%.*s%s%s", (int)(pc - e->fmt), e->fmt, e->out, pc + 2);
}

/* the first number in the output, ignoring markup tags */
static bool cell_number(const char *s, float *out)
{
    bool in_tag = false;
    for (const char *p = s; *p; ++p) {
        if (*p == '<') { in_tag = true; continue; }
        if (*p == '>') { in_tag = false; continue; }
        if (in_tag) continue;
        if (isdigit((unsigned char)*p) ||
            ((*p == '-' || *p == '.') && isdigit((unsigned char)p[1]))) {
            *out = (float)atof(p);
            return true;
        }
    }
    return false;
}

static Col cell_color(Cell *e)
{
    Col base = e->has_color ? e->color : cfg.text;
    float n;
    if ((e->has_warn || e->has_crit) && cell_number(e->out, &n)) {
        if (e->has_warn && (e->warn_dir > 0 ? n >= e->warn_th : n <= e->warn_th))
            base = e->warn_col;
        if (e->has_crit && (e->crit_dir > 0 ? n >= e->crit_th : n <= e->crit_th))
            base = e->crit_col;
    }
    return base;
}

static bool cell_is_msg(Cell *e)
{
    return *cfg.msg_target && !strcmp(cfg.msg_target, e->name) && msg_active();
}

static bool cell_visible(Cell *e)
{
    if (cell_is_msg(e)) return true;
    if (!*e->cmd && !*e->fmt && !*e->empty) return false;
    if (*e->out) return true;
    return *e->empty || !e->hide_empty;      /* a placeholder stays clickable */
}

/* what a cell draws right now, and in which colour */
static void cell_display(Cell *e, char *out, size_t cap, Col *col)
{
    if (cell_is_msg(e)) { str_set(out, cap, msg_text); *col = msg_color(); return; }
    if (!*e->out && *e->empty) { str_set(out, cap, e->empty); *col = cell_color(e); return; }
    cell_text(e, out, cap);
    *col = cell_color(e);
}

/* --------------------------------------------------------------- wayland */

struct zwlr_layer_shell_v1;

typedef struct Bar Bar;

typedef struct {
    uint32_t id;
    struct wl_output *wl;
    char name[64];
    int scale, mode_w, mode_h;
    Bar *bar;
    bool alive;
} Output;

struct Bar {
    Output *out;
    struct wl_surface *surf;
    struct zwlr_layer_surface_v1 *ls;
    struct wl_callback *frame;

    int w, h, want_w;            /* logical surface size */
    int scale;
    bool configured, dirty, closed;

    /* shm: one pool, two buffers */
    struct wl_shm_pool *pool;
    void *pool_data;
    size_t pool_size;
    struct wl_buffer *buf[2];
    void *bufmem[2];
    bool busy[2];
    int stride, bw, bh;

    /* collapse state */
    bool collapsed;
    float vis, anim_from, anim_to;
    uint32_t anim_start;

    /* pointer */
    bool hovered, kbd_grab;
    double px, py;

    /* workspace hit boxes, rebuilt on every render */
    struct { float x0, x1; char name[128]; } hit[MAX_WORKSPACES];
    int hits;
    struct { float x0, x1; int cell; } chit[MAX_CELLS];
    int chits;
};

static struct wl_display    *dpy;
static struct wl_registry   *registry;
static struct wl_compositor *compositor;
static struct wl_shm        *shm;
static struct wl_seat       *seat;
static struct wl_pointer    *pointer;
static struct wl_keyboard   *keyboard;
static struct zwlr_layer_shell_v1 *layer_shell;

static Output outputs[MAX_OUTPUTS];
static int    output_count = 0;
static Bar   *pointer_bar = NULL;
static bool   running = true;

static void bar_render(Bar *b);
static void bar_damage(Bar *b) { if (b) { b->dirty = true; } }

static void damage_all(void)
{
    for (int i = 0; i < output_count; ++i)
        if (outputs[i].bar) bar_damage(outputs[i].bar);
}

static bool output_wanted(const char *name)
{
    if (!*cfg.outputs) return true;
    char tmp[512];
    str_set(tmp, sizeof(tmp), cfg.outputs);
    for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
        char *t = trim(tok);
        if (!strcasecmp(t, name) || !strcmp(t, "*")) return true;
    }
    return false;
}

/* -------- sizes (logical pixels, multiplied by the output scale) -------- */

static int bar_height_logical(void)
{
    if (cfg.height > 0) return cfg.height;
    float base = fmaxf(cfg.text_px, cfg.ws_px) * cfg.ui_scale;
    int h = (int)lroundf(base * 1.35f) + 4;
    return h < 10 ? 10 : h;
}

static float bar_visible_logical(Bar *b)
{
    float full = (float)bar_height_logical();
    float col = (float)clampi(cfg.collapsed_px, 1, (int)full);
    return col + (full - col) * clampf(b->vis, 0.0f, 1.0f);
}

/* -------------------------------------------------------------- buffers */

static void buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    Bar *b = (Bar *)data;
    for (int i = 0; i < 2; ++i)
        if (b->buf[i] == wl_buffer) b->busy[i] = false;
}
static const struct wl_buffer_listener buffer_listener = { buffer_release };

static void bar_free_buffers(Bar *b)
{
    for (int i = 0; i < 2; ++i) {
        if (b->buf[i]) wl_buffer_destroy(b->buf[i]);
        b->buf[i] = NULL;
        b->busy[i] = false;
        b->bufmem[i] = NULL;
    }
    if (b->pool) wl_shm_pool_destroy(b->pool);
    b->pool = NULL;
    if (b->pool_data) munmap(b->pool_data, b->pool_size);
    b->pool_data = NULL;
    b->pool_size = 0;
}

static int anon_shm(size_t size)
{
    int fd = memfd_create("swbr", MFD_CLOEXEC);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

static bool bar_alloc(Bar *b)
{
    int bw = b->w * b->scale, bh = b->h * b->scale;
    if (bw <= 0 || bh <= 0) return false;
    if (b->pool && b->bw == bw && b->bh == bh) return true;

    bar_free_buffers(b);
    b->bw = bw;
    b->bh = bh;
    b->stride = bw * 4;
    size_t one = (size_t)b->stride * (size_t)bh;
    size_t total = one * 2;

    int fd = anon_shm(total);
    if (fd < 0) return false;
    void *mem = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mem == MAP_FAILED) { close(fd); return false; }

    b->pool_data = mem;
    b->pool_size = total;
    b->pool = wl_shm_create_pool(shm, fd, (int32_t)total);
    close(fd);
    for (int i = 0; i < 2; ++i) {
        b->buf[i] = wl_shm_pool_create_buffer(b->pool, (int32_t)(one * (size_t)i),
                                              bw, bh, b->stride, WL_SHM_FORMAT_ARGB8888);
        b->bufmem[i] = (char *)mem + one * (size_t)i;
        b->busy[i] = false;
        wl_buffer_add_listener(b->buf[i], &buffer_listener, b);
    }
    return true;
}

/* ---------------------------------------------------------------- render */

/* Baseline that puts the ink optically centred in a band: the em box is
 * centred, then nudged by text_y (logical px, negative = up). */
static float baseline_for(Font *f, float y0, float vis, float s)
{
    return y0 + vis * 0.5f + (f->ascent - f->lineh * 0.5f) + cfg.text_y * s;
}

static Col col_scale_alpha(Col c, float f)
{
    c.a = (uint8_t)clampi((int)lroundf((float)c.a * clampf(f, 0.0f, 1.0f)), 0, 255);
    return c;
}

static bool ws_on_bar(Bar *b, Ws *w)
{
    if (!b->out || !*b->out->name || !*w->output) return true;
    return strcmp(w->output, b->out->name) == 0;
}

/* --------------------------------------------------------------- slim mode
 * Folded, the bar is a few pixels tall and still has to say something. Each
 * workspace keeps a fixed slot so its position never moves, the clock turns
 * into twelve dots, and anything with a percentage becomes a small gauge. */

static Ws *ws_by_num(Bar *b, int num)
{
    for (int i = 0; i < ws_count; ++i)
        if (ws_list[i].num == num && ws_on_bar(b, &ws_list[i])) return &ws_list[i];
    return NULL;
}

static int slim_mode(Cell *e)
{
    if (e->slim != SLIM_AUTO) return e->slim;
    if (!*e->out) return SLIM_TICK;              /* a placeholder is a button */
    if (strchr(e->out, '%')) return SLIM_BAR;
    float v;
    if ((e->has_warn || e->has_crit) && cell_number(e->out, &v)) return SLIM_BAR;
    return SLIM_TICK;
}

static float slim_width_of(Cell *e, int mode, float s)
{
    if (e && e->slim_w > 0) return (float)e->slim_w * s;
    switch (mode) {
    case SLIM_CLOCK: return 12.0f * 3.0f * s + 11.0f * 2.0f * s;
    case SLIM_BAR:   return 40.0f * s;
    case SLIM_PRESENCE: return 10.0f * s;
    case SLIM_OFF:   return 0.0f;
    default:         return 12.0f * s;
    }
}


static void slim_draw_one(Canvas *cv, Cell *e, int mode, float x, float y,
                          float w, float h, float s)
{
    Col c = e->has_slim_color ? e->slim_color : cell_color(e);
    if (mode == SLIM_PRESENCE) {
        /* the cell only exists while its program does, so a block here means
         * "that one is up" — its own colour, not shared with any gauge */
        if (!e->has_slim_color) c = cfg.running;
        fill_rect(cv, x, y, w, h, c);
        return;
    }
    if (mode == SLIM_CLOCK) {
        time_t t = time(NULL);
        struct tm tmv;
        localtime_r(&t, &tmv);
        int hour = tmv.tm_hour % 12;
        float dot = 3.0f * s, gap = 2.0f * s;
        for (int i = 0; i < 12; ++i) {
            Col dc = cfg.dim;
            float dw = dot;
            if (i < hour) dc = cfg.accent;
            else if (i == hour) { dc = cfg.hl; dw = dot * (0.35f + 0.65f * (float)tmv.tm_min / 59.0f); }
            fill_rect(cv, x + (float)i * (dot + gap), y, dw, h, dc);
        }
        return;
    }
    if (mode == SLIM_BAR) {
        float v = 0;
        bool have = cell_number(e->out, &v);
        float lo = e->slim_min, hi = e->slim_max;
        float f = (hi > lo) ? (v - lo) / (hi - lo) : 0.0f;
        f = clampf(f, 0.0f, 1.0f);
        Col track = cfg.dim;                       /* visible, or a part filled
                                                    * gauge just looks short */
        track.a = 110;
        fill_rect(cv, x, y, w, h, track);
        if (have) fill_rect(cv, x, y, w * f, h, c);
        return;
    }
    fill_rect(cv, x, y, w, h, c);
}

static void draw_signals(Canvas *cv, Bar *b, float y, float h, float s, Runs *rs)
{
    if (!cfg.signals) return;
    float gap = 3.0f * s;
    float x = 4.0f * s;

    if (msg_active() && cfg.msg_flash) {          /* an error is worth the whole strip */
        fill_rect(cv, x, y, (float)cv->w - 8.0f * s, h, msg_color());
        return;
    }

    /* one fixed slot per workspace number, so slot 3 is always workspace 3 */
    int slots = 0;
    for (int i = 0; i < ws_count; ++i)
        if (ws_on_bar(b, &ws_list[i]) && ws_list[i].num > slots) slots = ws_list[i].num;
    if (slots <= 0)
        for (int i = 0; i < ws_count; ++i) if (ws_on_bar(b, &ws_list[i])) slots++;
    if (cfg.slim_ws_slots > 0 && slots > cfg.slim_ws_slots) slots = cfg.slim_ws_slots;

    float tick = 14.0f * s;
    b->hits = 0;
    b->chits = 0;
    for (int i = 1; i <= slots; ++i) {
        Ws *w = ws_by_num(b, i);
        Col c = cfg.dim;
        c.a = 60;                                  /* empty slot: a faint place holder */
        if (w) {
            if (w->urgent) c = cfg.urgent;
            else if (w->focused) c = cfg.hl;
            else if (w->visible) c = cfg.accent;
            else { c = cfg.dim; c.a = 170; }
        }
        fill_rect(cv, x, y, tick, h, c);
        if (w && b->hits < MAX_WORKSPACES) {      /* still switchable when folded */
            b->hit[b->hits].x0 = x / s;
            b->hit[b->hits].x1 = (x + tick) / s;
            str_set(b->hit[b->hits].name, sizeof(b->hit[b->hits].name), w->name);
            b->hits++;
        }
        x += tick + gap;
    }

    if (!cfg.cell_count) {                         /* status_command fallback */
        float rx = (float)cv->w - 4.0f * s;
        for (int i = rs->n - 1; i >= 0 && rx > x; --i) {
            if (!rs->v[i].has_fg) continue;
            float w = clampf((float)strlen(rs->v[i].text) * 2.0f * s, 6.0f * s, 40.0f * s);
            rx -= w;
            fill_rect(cv, rx, y, w, h, rs->v[i].fg);
            rx -= gap;
        }
        return;
    }

    /* the right hand group, in layout order */
    int idx[MAX_CELLS], mode[MAX_CELLS], n = 0;
    bool join[MAX_CELLS];
    float total = 0;
    for (int i = 0; i < layout_n[G_RIGHT] && n < MAX_CELLS; ++i) {
        Item *it = &layout_item[G_RIGHT][i];
        if (it->kind != IT_CELL) continue;
        Cell *e = &cfg.cell[it->cell];
        if (!cell_visible(e)) continue;
        int m = slim_mode(e);
        if (m == SLIM_OFF) continue;
        idx[n] = it->cell;
        mode[n] = m;
        join[n] = (n > 0 && !layout_item[G_RIGHT][i - 1].sep);
        total += slim_width_of(e, m, s) + (n ? (join[n] ? 1.0f * s : gap) : 0);
        n++;
    }
    float rx = (float)cv->w - 4.0f * s - total;
    if (rx < x + gap) rx = x + gap;
    for (int k = 0; k < n; ++k) {
        float w = slim_width_of(&cfg.cell[idx[k]], mode[k], s);
        if (k) rx += join[k] ? 1.0f * s : gap;
        if (rx + w > (float)cv->w - 3.0f * s) break;
        slim_draw_one(cv, &cfg.cell[idx[k]], mode[k], rx, y, w, h, s);
        if (b->chits < MAX_CELLS) {               /* buttons keep working */
            b->chit[b->chits].x0 = rx / s;
            b->chit[b->chits].x1 = (rx + w) / s;
            b->chit[b->chits].cell = idx[k];
            b->chits++;
        }
        rx += w;
    }
}
/* --------------------------------------------------------------- workspaces
 * The buttons are a placeable item like any cell, so they are measured and
 * drawn through the same two-pass code. With ws_inset > 0 they float inside
 * the bar as their own pills instead of being as tall as the bar itself. */

static float ws_button_w(Font *f, Ws *w, float s)
{
    return fmaxf(text_width(f, w->label) + 2.0f * (float)cfg.ws_pad * s,
                 (float)cfg.ws_min_w * s);
}

static float ws_block_w(Font *f, Bar *b, float s)
{
    float w = 0;
    int n = 0;
    for (int i = 0; i < ws_count; ++i) {
        if (!ws_on_bar(b, &ws_list[i])) continue;
        w += ws_button_w(f, &ws_list[i], s);
        n++;
    }
    if (n > 1) w += (float)(n - 1) * (float)cfg.ws_gap * s;
    return w;
}

/* one pill, square on the screen edge only when it is flush with it */
static void pill(Canvas *cv, float x, float y0, float vis, float w, float s,
                 float rad, Col c)
{
    float in = (float)cfg.ws_inset * s;
    bool top = strcmp(cfg.position, "bottom") != 0;
    float y = y0 + in, h = vis - 2.0f * in;
    if (in <= 0) {
        if (top) fill_round(cv, x, y0, w, vis, 0, 0, rad, rad, c);
        else     fill_round(cv, x, y0, w, vis, rad, rad, 0, 0, c);
        return;
    }
    fill_round(cv, x, y, w, h, rad, rad, rad, rad, c);
}

static float ws_radius_px(float vis, float s)
{
    if (cfg.ws_radius >= 0) return cfg.ws_radius * s;
    if (cfg.ws_inset > 0) return vis;            /* capped to a full pill */
    return cfg.radius * s;
}

static void ws_block_draw(Canvas *cv, Bar *b, Font *f, float x, float y0,
                          float vis, float s, float fade)
{
    float rad = ws_radius_px(vis, s);
    float base = baseline_for(f, y0, vis, s);
    for (int i = 0; i < ws_count; ++i) {
        Ws *w = &ws_list[i];
        if (!ws_on_bar(b, w)) continue;
        float bwid = ws_button_w(f, w, s);
        float tw = text_width(f, w->label);

        Col bg = cfg.ws_bg, fg = cfg.ws_fg;
        if (w->urgent)       { bg = cfg.ws_urgent_bg;  fg = cfg.ws_urgent_fg;  }
        else if (w->focused) { bg = cfg.ws_focused_bg; fg = cfg.ws_focused_fg; }
        else if (w->visible) { bg = cfg.ws_visible_bg; fg = cfg.ws_visible_fg; }

        pill(cv, x, y0, vis, bwid, s, rad, col_scale_alpha(bg, fade));
        text_draw(cv, f, x + (bwid - tw) * 0.5f, base, col_scale_alpha(fg, fade), w->label);

        if (b->hits < MAX_WORKSPACES) {
            b->hit[b->hits].x0 = x / s;
            b->hit[b->hits].x1 = (x + bwid) / s;
            str_set(b->hit[b->hits].name, sizeof(b->hit[b->hits].name), w->name);
            b->hits++;
        }
        x += bwid + (float)cfg.ws_gap * s;
    }
}

static bool mode_shown(void) { return cfg.mode_show && strcmp(sway_mode, "default"); }

static float mode_w(Font *f, float s)
{
    if (!mode_shown()) return 0;
    return text_width(f, sway_mode) + 2.0f * (float)cfg.ws_pad * s;
}

static void mode_draw(Canvas *cv, Font *f, float x, float y0, float vis, float s, float fade)
{
    if (!mode_shown()) return;
    pill(cv, x, y0, vis, mode_w(f, s), s, ws_radius_px(vis, s),
         col_scale_alpha(cfg.mode_bg, fade));
    text_draw(cv, f, x + (float)cfg.ws_pad * s, baseline_for(f, y0, vis, s),
              col_scale_alpha(cfg.mode_fg, fade), sway_mode);
}

/* ------------------------------------------------------------- text cells */
/* A cell with scroll=1 never pushes the layout around: too long and it ends
 * in "..", and it scrolls through the full text while the pointer is on it. */

static bool scroll_running = false;

static void runs_truncate(Font *f, Runs *rs, float maxw, float ellw)
{
    float w = 0;
    for (int i = 0; i < rs->n; ++i) {
        char *txt = rs->v[i].text;
        const char *p = txt;
        while (*p) {
            const char *prev = p;
            unsigned cp = utf8_next(&p);
            float aw = glyph_get(f, cp)->adv;
            if (w + aw > maxw - ellw) {
                size_t keep = (size_t)(prev - txt);
                txt[keep] = 0;
                if (keep + 3 < sizeof(rs->v[i].text)) strcat(txt, "..");
                rs->n = i + 1;
                return;
            }
            w += aw;
        }
    }
}

/* how far a hovered cell has scrolled: pause, glide, pause, jump back */
static float scroll_offset(Cell *e, float over, float s)
{
    uint32_t t = now_ms();
    if (!e->scroll_t) e->scroll_t = t;
    float speed = fmaxf((float)cfg.scroll_speed, 1.0f) * s;
    float pause = fmaxf((float)cfg.scroll_pause, 0.0f) / 1000.0f;
    float dur = over / speed;
    float cycle = pause + dur + pause;
    float el = (float)(t - e->scroll_t) / 1000.0f;
    float ph = cycle > 0 ? fmodf(el, cycle) : 0;
    scroll_running = true;
    if (ph < pause) return 0;
    if (ph < pause + dur) return (ph - pause) / dur * over;
    return over;
}

/* ------------------------------------------------------------------ groups */

/* space after an item: the cell may override the global cell_gap, which is
 * how two cells are pushed together into one visible box */
static float item_gap(Item *it, float s)
{
    int g = cfg.cell_gap;
    if (it->kind == IT_CELL && cfg.cell[it->cell].gap >= 0) g = cfg.cell[it->cell].gap;
    return (float)g * s;
}

static float item_width(Canvas *cv, Bar *b, Font *f, Item *it, float s,
                        char *txt, size_t cap, Col *col, float *tw)
{
    (void)cv;
    if (it->kind == IT_WS)   { *tw = 0; return ws_block_w(f, b, s); }
    if (it->kind == IT_MODE) { *tw = 0; return mode_w(f, s); }

    Cell *e = &cfg.cell[it->cell];
    cell_display(e, txt, cap, col);
    Runs rs;
    int saved = cfg.markup;
    cfg.markup = e->markup;
    markup_parse(txt, &rs, *col);
    cfg.markup = saved;
    float w = 0;
    for (int k = 0; k < rs.n; ++k) w += text_width(f, rs.v[k].text);
    *tw = w;
    float pad = 2.0f * (float)e->pad * s;
    float box = fmaxf(w + pad, (float)e->min_w * s);
    if (e->max_w > 0) box = fminf(box, (float)e->max_w * s);
    if (e->scroll && e->min_w > 0) box = (float)e->min_w * s;   /* fixed slot */
    return box;
}

/* Draws one group. Returns the left edge it occupies, so the next group in
 * from the right knows where it must stop. With measure = true nothing is
 * painted and the group's total width comes back instead — that is how the
 * left group reserves its space before the right one is placed. */
static float draw_group_(Canvas *cv, Bar *b, Font *f, int g, float y0, float vis,
                         float s, float fade, float left_limit, float right,
                         bool measure)
{
    static char txt[MAX_CELLS + 4][1024];
    float bw[MAX_CELLS + 4], tw[MAX_CELLS + 4];
    Col col[MAX_CELLS + 4];
    int idx[MAX_CELLS + 4], n = 0;

    float sep_w = *cfg.separator ? text_width(f, cfg.separator) : 0.0f;
    float total = 0;

    for (int i = 0; i < layout_n[g]; ++i) {
        Item *it = &layout_item[g][i];
        if (it->kind == IT_CELL && !cell_visible(&cfg.cell[it->cell])) continue;
        float w = item_width(cv, b, f, it, s, txt[n], sizeof(txt[n]), &col[n], &tw[n]);
        if (w <= 0) continue;
        bw[n] = w;
        idx[n] = i;
        total += w;
        n++;
    }
    if (!n) return measure ? 0.0f : right;
    for (int k = 0; k + 1 < n; ++k) {
        Item *it = &layout_item[g][idx[k]];
        float ig = item_gap(it, s);
        total += ig + (it->sep && sep_w > 0 ? ig + sep_w : 0.0f);
    }

    /* too wide: text cells give up their width first, that is what they are
     * for. Everything else keeps its size and the group clips at the edge. */
    float avail = right - left_limit;
    if (total > avail) {
        float excess = total - avail;
        float floor_w = 60.0f * s;
        for (int k = 0; k < n && excess > 0.5f; ++k) {
            Item *it = &layout_item[g][idx[k]];
            if (it->kind != IT_CELL || !cfg.cell[it->cell].scroll) continue;
            float can = bw[k] - floor_w;
            if (can <= 0) continue;
            float take = fminf(can, excess);
            bw[k] -= take;
            total -= take;
            excess -= take;
        }
    }
    if (measure) return total;

    float x;
    if (g == G_LEFT)        x = left_limit;
    else if (g == G_CENTER) x = ((float)cv->w - total) * 0.5f;
    else                    x = right - total;
    if (x + total > right) x = right - total;
    if (x < left_limit) x = left_limit;
    float origin = x;
    float base = baseline_for(f, y0, vis, s);

    /* where every item lands, so joined backgrounds can be merged into one
     * box instead of two rounded ones meeting in a notch */
    float xs[MAX_CELLS + 4];
    {
        float cx = x;
        for (int k = 0; k < n; ++k) {
            Item *it = &layout_item[g][idx[k]];
            xs[k] = cx;
            cx += bw[k];
            if (k + 1 < n) {
                float ig = item_gap(it, s);
                cx += ig + (it->sep && sep_w > 0 ? sep_w + ig : 0.0f);
            }
        }
    }
    for (int k = 0; k < n; ) {
        Item *it = &layout_item[g][idx[k]];
        if (it->kind != IT_CELL || !cfg.cell[it->cell].has_bg ||
            cell_is_msg(&cfg.cell[it->cell])) { k++; continue; }
        Col bgc = cfg.cell[it->cell].bg;
        int e2 = k;
        while (e2 + 1 < n) {                       /* extend over glued cells */
            Item *cur = &layout_item[g][idx[e2]];
            Item *nx = &layout_item[g][idx[e2 + 1]];
            if (cur->sep || item_gap(cur, s) > 0.5f) break;
            if (nx->kind != IT_CELL || !cfg.cell[nx->cell].has_bg) break;
            Col nc = cfg.cell[nx->cell].bg;
            if (nc.r != bgc.r || nc.g != bgc.g || nc.b != bgc.b || nc.a != bgc.a) break;
            e2++;
        }
        float in = (float)cfg.cell_inset * s;
        float r = cfg.cell_radius * s;
        fill_round(cv, xs[k], y0 + in, xs[e2] + bw[e2] - xs[k], vis - 2.0f * in,
                   r, r, r, r, col_scale_alpha(bgc, fade));
        k = e2 + 1;
    }

    for (int k = 0; k < n; ++k) {
        Item *it = &layout_item[g][idx[k]];
        x = xs[k];

        if (it->kind == IT_WS) {
            ws_block_draw(cv, b, f, x, y0, vis, s, fade);
        } else if (it->kind == IT_MODE) {
            mode_draw(cv, f, x, y0, vis, s, fade);
        } else {
            Cell *e = &cfg.cell[it->cell];
            bool hover = b->hovered && b->px * s >= x && b->px * s < x + bw[k];

            Runs rs;
            int saved = cfg.markup;
            cfg.markup = e->markup;
            markup_parse(txt[k], &rs, col[k]);
            cfg.markup = saved;

            float pad = (float)e->pad * s;
            float inner = bw[k] - 2.0f * pad;
            float tx = x + pad, over = tw[k] - inner;
            if (over > 0 && e->scroll) {
                if (hover) tx -= scroll_offset(e, over, s);
                else { e->scroll_t = 0; runs_truncate(f, &rs, inner, text_width(f, "..")); }
            } else if (over <= 0) {
                if (!strcmp(e->align, "right")) tx = x + bw[k] - pad - tw[k];
                else if (!strcmp(e->align, "center")) tx = x + (bw[k] - tw[k]) * 0.5f;
            }

            int ox0 = cv->cx0, ox1 = cv->cx1;
            canvas_clip(cv, (int)(x + pad), cv->cy0, (int)ceilf(x + bw[k] - pad), cv->cy1);
            for (int r = 0; r < rs.n; ++r) {
                float rw = text_width(f, rs.v[r].text);
                if (rs.v[r].has_bg)
                    fill_rect(cv, tx, y0 + 2.0f * s, rw, vis - 4.0f * s,
                              col_scale_alpha(rs.v[r].bg, fade));
                text_draw(cv, f, tx, base, col_scale_alpha(rs.v[r].fg, fade), rs.v[r].text);
                tx += rw;
            }
            canvas_clip(cv, ox0, cv->cy0, ox1, cv->cy1);

            if (b->chits < MAX_CELLS) {
                b->chit[b->chits].x0 = x / s;
                b->chit[b->chits].x1 = (x + bw[k]) / s;
                b->chit[b->chits].cell = it->cell;
                b->chits++;
            }
        }

        x += bw[k];
        if (k + 1 < n) {
            float ig = item_gap(it, s);
            x += ig;
            if (it->sep && sep_w > 0) {
                text_draw(cv, f, x, base, col_scale_alpha(cfg.separator_color, fade),
                          cfg.separator);
                x += sep_w + ig;
            }
        }
    }
    return origin;
}

/* A message with no cell to live in is drawn centred on its own. */
static float draw_message(Canvas *cv, Bar *b, Font *f, float y0, float vis,
                          float s, float fade, float left_limit, float right)
{
    if (!msg_active()) return right;
    for (int i = 0; i < cfg.cell_count; ++i)      /* a cell already shows it */
        if (cell_is_msg(&cfg.cell[i])) return right;

    Runs rs;
    markup_parse(msg_text, &rs, msg_color());
    float w = 0;
    for (int i = 0; i < rs.n; ++i) w += text_width(f, rs.v[i].text);
    float x = ((float)cv->w - w) * 0.5f;
    if (x + w > right) x = right - w;
    if (x < left_limit) x = left_limit;
    float origin = x;
    float base = baseline_for(f, y0, vis, s);
    int ox0 = cv->cx0, ox1 = cv->cx1;
    canvas_clip(cv, (int)x, cv->cy0, (int)ceilf(x + w), cv->cy1);
    for (int i = 0; i < rs.n; ++i) {
        float rw = text_width(f, rs.v[i].text);
        text_draw(cv, f, x, base, col_scale_alpha(rs.v[i].fg, fade), rs.v[i].text);
        x += rw;
    }
    canvas_clip(cv, ox0, cv->cy0, ox1, cv->cy1);
    (void)b;
    return origin;
}

static float draw_group(Canvas *cv, Bar *b, Font *f, int g, float y0, float vis,
                        float s, float fade, float left_limit, float right)
{
    return draw_group_(cv, b, f, g, y0, vis, s, fade, left_limit, right, false);
}

static float group_width(Canvas *cv, Bar *b, Font *f, int g, float vis, float s)
{
    return draw_group_(cv, b, f, g, 0, vis, s, 1.0f, 0, (float)cv->w, true);
}

/* what the group wants, with nothing squeezed */
static float group_natural(Canvas *cv, Bar *b, Font *f, int g, float vis, float s)
{
    return draw_group_(cv, b, f, g, 0, vis, s, 1.0f, 0, 1.0e6f, true);
}

/* The bar sizes itself: as wide as its content needs, never under min_width,
 * never wider than the output. Cells come and go (charging, docker, a longer
 * clock), so this is checked on every paint and the layer surface is only
 * resized when it actually changed. */
static int bar_natural_width(Bar *b, Canvas *cv, Font *f, float vis, float s)
{
    float lw = group_natural(cv, b, f, G_LEFT, vis, s);
    float cw = group_natural(cv, b, f, G_CENTER, vis, s);
    float rw = group_natural(cv, b, f, G_RIGHT, vis, s);
    float gap = (float)cfg.cell_gap * s;
    int groups = (lw > 0) + (cw > 0) + (rw > 0);
    float total = lw + cw + rw;
    if (groups > 1) total += (float)(groups - 1) * gap * 2.0f;
    total += (float)cfg.pad_x * s + (float)(cfg.pad_right >= 0 ? cfg.pad_right : cfg.pad_x) * s;

    int want = (int)ceilf(total / s);
    if (want < cfg.min_width) want = cfg.min_width;

    int limit = 0;
    if (b->out && b->out->mode_w > 0)
        limit = b->out->mode_w / (b->out->scale > 0 ? b->out->scale : 1) - 2 * cfg.side_margin;
    if (limit > 0 && want > limit) want = limit;
    return want;
}

static void bar_fit_width(Bar *b, Canvas *cv, Font *f, float vis, float s)
{
    if (cfg.min_width <= 0 || !b->ls) return;
    int want = bar_natural_width(b, cv, f, vis, s);
    if (want != b->want_w && abs(want - b->w) >= 2) {
        b->want_w = want;
        ls_set_size(b->ls, (uint32_t)want, (uint32_t)b->h);
        wl_surface_commit(b->surf);
    }
}

/* Everything the bar paints, into a plain canvas. Split out of bar_render so
 * it can be exercised without a compositor. */
static void bar_paint(Bar *b, Canvas *cv, float visL, float s)
{
    bool top = strcmp(cfg.position, "bottom") != 0;
    float vis = visL * s;
    float y0 = top ? 0.0f : (float)cv->h - vis;
    float rad = cfg.radius * s;
    float bw = (float)cv->w;

    memset(cv->px, 0, (size_t)cv->w * (size_t)cv->h * 4);
    canvas_clip_all(cv);

    /* flush with the screen edge: the two corners that touch it stay square */
    if (top) fill_round(cv, 0, y0, bw, vis, 0, 0, rad, rad, cfg.bg);
    else     fill_round(cv, 0, y0, bw, vis, rad, rad, 0, 0, cfg.bg);

    Runs rs;
    markup_parse(status_line, &rs, cfg.text);

    float fade = clampf(b->vis * 1.6f - 0.25f, 0.0f, 1.0f);
    b->hits = 0;
    b->chits = 0;
    if (fade <= 0.01f) {
        draw_signals(cv, b, y0, vis, s, &rs);
        return;
    }

    canvas_clip(cv, 0, (int)floorf(y0), (int)bw, (int)ceilf(y0 + vis));

    Font *f = font_get((int)lroundf(cfg.text_px * cfg.ui_scale * s));
    bar_fit_width(b, cv, f, vis, s);
    float gap = (float)cfg.cell_gap * s;
    float left_limit = (float)cfg.pad_x * s;
    float right = bw - (float)(cfg.pad_right >= 0 ? cfg.pad_right : cfg.pad_x) * s;

    if (cfg.cell_count || layout_n[G_LEFT]) {
        float lw = group_width(cv, b, f, G_LEFT, vis, s);
        float after_left = left_limit + (lw > 0 ? lw + gap : 0.0f);
        float rx = draw_group(cv, b, f, G_RIGHT, y0, vis, s, fade, after_left, right);
        float mx = draw_message(cv, b, f, y0, vis, s, fade, after_left, rx - gap);
        float cx = draw_group(cv, b, f, G_CENTER, y0, vis, s, fade, after_left,
                              fminf(rx, mx) - gap);
        draw_group(cv, b, f, G_LEFT, y0, vis, s, fade, left_limit, cx - gap);
    }

    if (!cfg.cell_count && *status_line) {         /* status_command fallback */
        float total = 0;
        for (int i = 0; i < rs.n; ++i) total += text_width(f, rs.v[i].text);
        float sx = right - total;
        float base = baseline_for(f, y0, vis, s);
        for (int i = 0; i < rs.n; ++i) {
            float rw = text_width(f, rs.v[i].text);
            text_draw(cv, f, sx, base, col_scale_alpha(rs.v[i].fg, fade), rs.v[i].text);
            sx += rw;
        }
    }
    canvas_clip_all(cv);
}

static void bar_render(Bar *b)
{
    if (!b->configured || b->closed) return;
    if (!bar_alloc(b)) return;

    int slot = -1;
    for (int i = 0; i < 2; ++i) if (!b->busy[i]) { slot = i; break; }
    if (slot < 0) return;                     /* both in flight: next frame */

    float s = (float)b->scale;
    float visL = bar_visible_logical(b);
    bool top = strcmp(cfg.position, "bottom") != 0;
    Canvas cv = { (uint32_t *)b->bufmem[slot], b->bw, b->bh, 0, 0, b->bw, b->bh };
    bar_paint(b, &cv, visL, s);

    /* hand the buffer over */
    b->busy[slot] = true;
    wl_surface_set_buffer_scale(b->surf, b->scale);
    wl_surface_attach(b->surf, b->buf[slot], 0, 0);
    wl_surface_damage_buffer(b->surf, 0, 0, b->bw, b->bh);

    /* only the visible strip takes clicks — the rest of the surface is
     * transparent and must not swallow them */
    struct wl_region *reg = wl_compositor_create_region(compositor);
    int rv = (int)ceilf(visL);
    wl_region_add(reg, 0, top ? 0 : b->h - rv, b->w, rv);
    wl_surface_set_input_region(b->surf, reg);
    wl_region_destroy(reg);

    if (cfg.exclusive)
        ls_set_exclusive_zone(b->ls, rv + cfg.margin);

    wl_surface_commit(b->surf);
    b->dirty = false;
}
/* ------------------------------------------------------------- animation */

static void bar_set_collapsed(Bar *b, bool c)
{
    b->collapsed = c;
    b->anim_from = b->vis;
    b->anim_to = c ? 0.0f : 1.0f;
    b->anim_start = now_ms();
    if (cfg.anim_ms <= 0) b->vis = b->anim_to;
    b->dirty = true;
}

static bool bar_animating(Bar *b) { return b->vis != b->anim_to; }

static void bar_anim_step(Bar *b, uint32_t t)
{
    if (!bar_animating(b)) return;
    if (cfg.anim_ms <= 0) { b->vis = b->anim_to; b->dirty = true; return; }
    float k = (float)(t - b->anim_start) / (float)cfg.anim_ms;
    if (k >= 1.0f) b->vis = b->anim_to;
    else b->vis = b->anim_from + (b->anim_to - b->anim_from) * (k * k * (3.0f - 2.0f * k));
    b->dirty = true;
}

static void bar_set_grab(Bar *b, bool on)
{
    if (!cfg.hover_keys || b->kbd_grab == on || !b->ls) return;
    b->kbd_grab = on;
    ls_set_keyboard(b->ls, on ? ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE
                              : ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(b->surf);
}

/* ---------------------------------------------------------- layer surface */

static void ls_configure(void *data, struct zwlr_layer_surface_v1 *s,
                         uint32_t serial, uint32_t w, uint32_t h)
{
    Bar *b = (Bar *)data;
    ls_ack_configure(s, serial);
    if (w) b->w = (int)w;
    if (h) b->h = (int)h;
    b->configured = true;
    b->dirty = true;
    bar_render(b);
}

static void ls_closed(void *data, struct zwlr_layer_surface_v1 *s)
{
    (void)s;
    Bar *b = (Bar *)data;
    b->closed = true;
    b->configured = false;
}

static const struct zwlr_layer_surface_v1_listener ls_listener = { ls_configure, ls_closed };

static void bar_create(Output *o)
{
    if (o->bar || !layer_shell || !compositor || !shm) return;
    if (!output_wanted(o->name)) return;

    Bar *b = (Bar *)xmalloc(sizeof(Bar));
    memset(b, 0, sizeof(*b));
    b->out = o;
    b->scale = o->scale > 0 ? o->scale : 1;
    b->w = 0;
    b->h = bar_height_logical();
    b->vis = cfg.start_collapsed ? 0.0f : 1.0f;
    b->anim_to = b->vis;
    b->collapsed = cfg.start_collapsed != 0;

    bool top = strcmp(cfg.position, "bottom") != 0;
    uint32_t layer = ZWLR_LAYER_SHELL_V1_LAYER_TOP;
    if (!strcmp(cfg.layer, "overlay")) layer = ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY;
    else if (!strcmp(cfg.layer, "bottom")) layer = ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM;
    else if (!strcmp(cfg.layer, "background")) layer = ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND;

    b->surf = wl_compositor_create_surface(compositor);
    b->ls = zwlr_layer_shell_v1_get_layer_surface(layer_shell, b->surf, o->wl, layer, APP_ID);
    zwlr_layer_surface_v1_add_listener(b->ls, &ls_listener, b);

    uint32_t anchor = top ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                          : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    if (cfg.min_width > 0) {
        /* a floating pill: anchored to one edge only, so the compositor
         * centres it — unless align_x pins it to a side. The width is not
         * fixed, bar_render grows it to whatever the content needs. */
        if (!strcmp(cfg.align_x, "left"))  anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        if (!strcmp(cfg.align_x, "right")) anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        b->w = cfg.min_width;
        ls_set_anchor(b->ls, anchor);
        ls_set_size(b->ls, (uint32_t)cfg.min_width, (uint32_t)b->h);
    } else {
        anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        ls_set_anchor(b->ls, anchor);
        ls_set_size(b->ls, 0, (uint32_t)b->h);
    }
    ls_set_margin(b->ls, top ? cfg.margin : 0, cfg.side_margin,
                  top ? 0 : cfg.margin, cfg.side_margin);
    ls_set_exclusive_zone(b->ls, cfg.exclusive ? b->h + cfg.margin : 0);
    ls_set_keyboard(b->ls, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    wl_surface_commit(b->surf);
    o->bar = b;
}

static void bar_destroy(Output *o)
{
    Bar *b = o->bar;
    if (!b) return;
    if (pointer_bar == b) pointer_bar = NULL;
    bar_free_buffers(b);
    if (b->ls) ls_destroy(b->ls);
    if (b->surf) wl_surface_destroy(b->surf);
    free(b);
    o->bar = NULL;
}

/* --------------------------------------------------------------- outputs */

static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t sub, const char *make,
                         const char *model, int32_t tr)
{ (void)d;(void)o;(void)x;(void)y;(void)pw;(void)ph;(void)sub;(void)make;(void)model;(void)tr; }

static void out_mode(void *d, struct wl_output *wo, uint32_t f, int32_t w, int32_t h, int32_t r)
{
    (void)wo; (void)r;
    Output *o = (Output *)d;
    if (f & WL_OUTPUT_MODE_CURRENT) { o->mode_w = w; o->mode_h = h; }
}

static void out_scale(void *d, struct wl_output *wo, int32_t factor)
{
    (void)wo;
    Output *o = (Output *)d;
    o->scale = factor > 0 ? factor : 1;
    if (o->bar && o->bar->scale != o->scale) {
        o->bar->scale = o->scale;
        bar_free_buffers(o->bar);
        o->bar->dirty = true;
    }
}

static void out_name(void *d, struct wl_output *wo, const char *name)
{
    (void)wo;
    Output *o = (Output *)d;
    str_set(o->name, sizeof(o->name), name ? name : "");
}

static void out_description(void *d, struct wl_output *o, const char *desc)
{ (void)d;(void)o;(void)desc; }

static void out_done(void *d, struct wl_output *wo)
{
    (void)wo;
    Output *o = (Output *)d;
    if (!o->bar) bar_create(o);
    else o->bar->dirty = true;
}

static const struct wl_output_listener output_listener = {
    out_geometry, out_mode, out_done, out_scale, out_name, out_description
};

/* --------------------------------------------------------------- pointer */

static Bar *bar_of_surface(struct wl_surface *s)
{
    for (int i = 0; i < output_count; ++i)
        if (outputs[i].bar && outputs[i].bar->surf == s) return outputs[i].bar;
    return NULL;
}

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *surf, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)d; (void)p; (void)serial;
    Bar *b = bar_of_surface(surf);
    pointer_bar = b;
    if (!b) return;
    b->hovered = true;
    b->px = wl_fixed_to_double(sx);
    b->py = wl_fixed_to_double(sy);
    bar_set_grab(b, true);
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *surf)
{
    (void)d; (void)p; (void)serial;
    Bar *b = bar_of_surface(surf);
    if (!b) return;
    b->hovered = false;
    b->dirty = true;
    bar_set_grab(b, false);
    if (pointer_bar == b) pointer_bar = NULL;
}

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t sx, wl_fixed_t sy)
{
    (void)d; (void)p; (void)t;
    if (!pointer_bar) return;
    pointer_bar->px = wl_fixed_to_double(sx);
    pointer_bar->py = wl_fixed_to_double(sy);
    pointer_bar->dirty = true;
}

static void ws_switch(const char *name)
{
    char esc[256];
    size_t j = 0;
    for (const char *q = name; *q && j < sizeof(esc) - 2; ++q) {
        if (*q == '"' || *q == '\\') esc[j++] = '\\';
        esc[j++] = *q;
    }
    esc[j] = 0;
    sway_cmd("workspace --no-auto-back-and-forth \"%s\"", esc);
}

static void bar_click(Bar *b, int button)
{
    for (int i = 0; i < b->chits; ++i)
        if (b->px >= b->chit[i].x0 && b->px < b->chit[i].x1) {
            Cell *e = &cfg.cell[b->chit[i].cell];
            if (cell_is_msg(e)) { msg_text[0] = 0; msg_until = 0; damage_all(); return; }
            if (button >= 1 && button < MAX_BINDS && *e->bind[button]) {
                run_command(e->bind[button]);
                return;
            }
            break;
        }
    if (button == 1 && cfg.ws_click) {
        for (int i = 0; i < b->hits; ++i)
            if (b->px >= b->hit[i].x0 && b->px < b->hit[i].x1) {
                ws_switch(b->hit[i].name);
                return;
            }
    }
    if ((button == 4 || button == 5) && cfg.scroll_workspace) {
        sway_cmd("workspace %s_on_output", button == 4 ? "prev" : "next");
        return;
    }
    if (button >= 1 && button < MAX_BINDS && *cfg.bind[button])
        run_command(cfg.bind[button]);
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
                       uint32_t button, uint32_t state)
{
    (void)d; (void)p; (void)serial; (void)t;
    if (state != WL_POINTER_BUTTON_STATE_PRESSED || !pointer_bar) return;
    int n = 0;
    switch (button) {
    case 272: n = 1; break;   /* BTN_LEFT   */
    case 274: n = 2; break;   /* BTN_MIDDLE */
    case 273: n = 3; break;   /* BTN_RIGHT  */
    case 275: n = 8; break;   /* BTN_SIDE   */
    case 276: n = 9; break;   /* BTN_EXTRA  */
    default: return;
    }
    bar_click(pointer_bar, n);
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t value)
{
    (void)d; (void)p; (void)t;
    if (!pointer_bar) return;
    double v = wl_fixed_to_double(value);
    if (v == 0) return;
    int n;
    if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL) n = v < 0 ? 4 : 5;
    else                                         n = v < 0 ? 6 : 7;
    bar_click(pointer_bar, n);
}

static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d;(void)p;(void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a)
{ (void)d;(void)p;(void)t;(void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v)
{ (void)d;(void)p;(void)a;(void)v; }

/* designated, so newer libwayland versions with extra axis events still
   compile against this without a warning — we bind wl_seat at version 5,
   where those events are never sent */
static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
    .button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
    .axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete
};

/* -------------------------------------------------------------- keyboard */
/* No libxkbcommon: the bar only ever looks for one key, so it compares the
 * raw evdev keycode. That makes the binding layout independent — the key in
 * the space bar's place, whatever your layout calls it. */

static struct { const char *name; int code; } keycodes[] = {
    { "space", 57 }, { "escape", 1 }, { "esc", 1 }, { "tab", 15 }, { "enter", 28 },
    { "backspace", 14 }, { "minus", 12 }, { "grave", 41 }, { "backslash", 43 },
    { "q", 16 }, { "w", 17 }, { "e", 18 }, { "r", 19 }, { "t", 20 }, { "y", 21 },
    { "u", 22 }, { "i", 23 }, { "o", 24 }, { "p", 25 }, { "a", 30 }, { "s", 31 },
    { "d", 32 }, { "f", 33 }, { "g", 34 }, { "h", 35 }, { "j", 36 }, { "k", 37 },
    { "l", 38 }, { "z", 44 }, { "x", 45 }, { "c", 46 }, { "v", 47 }, { "b", 48 },
    { "n", 49 }, { "m", 50 },
    { "1", 2 }, { "2", 3 }, { "3", 4 }, { "4", 5 }, { "5", 6 }, { "6", 7 },
    { "7", 8 }, { "8", 9 }, { "9", 10 }, { "0", 11 },
    { "f1", 59 }, { "f2", 60 }, { "f3", 61 }, { "f4", 62 }, { "f5", 63 },
    { "f6", 64 }, { "f7", 65 }, { "f8", 66 }, { "f9", 67 }, { "f10", 68 },
    { "f11", 87 }, { "f12", 88 }, { NULL, 0 }
};

static int hide_keycode(void)
{
    if (cfg.hide_keycode > 0) return cfg.hide_keycode;
    for (int i = 0; keycodes[i].name; ++i)
        if (!strcasecmp(keycodes[i].name, cfg.hide_key)) return keycodes[i].code;
    return 57;
}

static void kb_keymap(void *d, struct wl_keyboard *k, uint32_t fmt, int32_t fd, uint32_t sz)
{ (void)d; (void)k; (void)fmt; (void)sz; if (fd >= 0) close(fd); }
static void kb_enter(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *su,
                     struct wl_array *keys)
{ (void)d;(void)k;(void)s;(void)su;(void)keys; }
static void kb_leave(void *d, struct wl_keyboard *k, uint32_t s, struct wl_surface *su)
{ (void)d;(void)k;(void)s;(void)su; }
static void kb_modifiers(void *d, struct wl_keyboard *k, uint32_t s, uint32_t dep,
                         uint32_t lat, uint32_t lock, uint32_t grp)
{ (void)d;(void)k;(void)s;(void)dep;(void)lat;(void)lock;(void)grp; }
static void kb_repeat(void *d, struct wl_keyboard *k, int32_t rate, int32_t delay)
{ (void)d;(void)k;(void)rate;(void)delay; }

static void kb_key(void *d, struct wl_keyboard *k, uint32_t serial, uint32_t t,
                   uint32_t key, uint32_t state)
{
    (void)d; (void)k; (void)serial; (void)t;
    if (state != WL_KEYBOARD_KEY_STATE_PRESSED) return;
    if ((int)key != hide_keycode()) return;
    Bar *b = pointer_bar;
    if (b) bar_set_collapsed(b, !b->collapsed);
}

static const struct wl_keyboard_listener keyboard_listener = {
    kb_keymap, kb_enter, kb_leave, kb_key, kb_modifiers, kb_repeat
};

static void seat_caps(void *d, struct wl_seat *s, uint32_t caps)
{
    (void)d;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
    }
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !keyboard) {
        keyboard = wl_seat_get_keyboard(s);
        wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *n) { (void)d;(void)s;(void)n; }
static const struct wl_seat_listener seat_listener = { seat_caps, seat_name };

/* -------------------------------------------------------------- registry */

static uint32_t vmin(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void reg_global(void *d, struct wl_registry *r, uint32_t id,
                       const char *iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        compositor = (struct wl_compositor *)wl_registry_bind(r, id, &wl_compositor_interface, vmin(ver, 4));
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        shm = (struct wl_shm *)wl_registry_bind(r, id, &wl_shm_interface, 1);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        if (!seat) {
            seat = (struct wl_seat *)wl_registry_bind(r, id, &wl_seat_interface, vmin(ver, 5));
            wl_seat_add_listener(seat, &seat_listener, NULL);
        }
    } else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
        layer_shell = (struct zwlr_layer_shell_v1 *)
            wl_registry_bind(r, id, &zwlr_layer_shell_v1_interface, vmin(ver, 4));
    } else if (!strcmp(iface, wl_output_interface.name)) {
        Output *o = NULL;
        for (int i = 0; i < output_count; ++i)
            if (!outputs[i].alive) { o = &outputs[i]; break; }
        if (!o) {
            if (output_count >= MAX_OUTPUTS) return;
            o = &outputs[output_count++];
        }
        memset(o, 0, sizeof(*o));
        o->id = id;
        o->scale = 1;
        o->alive = true;
        o->wl = (struct wl_output *)wl_registry_bind(r, id, &wl_output_interface, vmin(ver, 4));
        wl_output_add_listener(o->wl, &output_listener, o);
    }
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t id)
{
    (void)d; (void)r;
    for (int i = 0; i < output_count; ++i) {
        if (!outputs[i].alive || outputs[i].id != id) continue;
        bar_destroy(&outputs[i]);
        if (outputs[i].wl) wl_output_destroy(outputs[i].wl);
        outputs[i].alive = false;
        outputs[i].wl = NULL;
        outputs[i].name[0] = 0;
    }
}

static const struct wl_registry_listener registry_listener = { reg_global, reg_remove };

/* ------------------------------------------------------------------ help */

static void usage(void)
{
    fputs(
"SwBr " SWBR_VERSION " (build " SWBR_BUILD ") — a floating layer-shell bar for Sway\n"
"\n"
"usage: swbr [key=value ...]\n"
"       every config key also works as key=value, --key=value or -s key=value\n"
"\n"
"  --config PATH       read this file instead of the default\n"
"  --dump-config       print a config file with all defaults, then exit\n"
"  --probe             print outputs, sizes, font metrics and cell values\n"
"  --slim              start folded (same as start_collapsed=1)\n"
"  --msg TEXT          send a message to the running bar, then exit\n"
"                      'warn: ..', 'error: ..', 'info: ..', or 'clear'\n"
"  --help, --version\n"
"\n"
"config file: ${XDG_CONFIG_HOME:-~/.config}/swbr/config\n"
"\n"
"placement\n"
"  layer=top           background | bottom | top | overlay\n"
"  position=top        top | bottom\n"
"  height=0            bar height in logical px, 0 = derive from the font\n"
"  exclusive=0         1 = reserve the space (windows tile below the bar)\n"
"                      0 = float above them\n"
"  margin=0            gap to the screen edge\n"
"  side_margin=0       gap left and right\n"
"  min_width=800       0 = span the whole output. Otherwise the bar floats\n"
"                      and sizes itself to its content: never narrower than\n"
"                      this, never wider than the screen. align_x places it\n"
"  radius=14           corner radius; the two corners at the screen edge\n"
"                      stay square\n"
"  outputs=            comma separated output names, empty = every output\n"
"\n"
"workspaces\n"
"  ws_names=0          0 = number only, 1 = the full workspace name\n"
"  ws_pad=11 ws_gap=4 ws_min_w=0\n"
"  ws_inset=0          gap above and below the buttons; >0 makes them pills\n"
"  ws_radius=0         0 = square, the button spans the whole bar height\n"
"                      -1 = a full pill when inset, else follows radius\n"
"  ws_click=1          left click switches workspace\n"
"  scroll_workspace=0  1 = the wheel switches workspaces\n"
"  mode_show=1         show the binding mode (resize, ..)\n"
"\n"
"text\n"
"  font=               .ttf path or a fontconfig family. Default: monospace\n"
"  font_alt=           fallback font for glyphs the first one lacks\n"
"  ui_scale=1.0 text_px=19 ws_px=19 pad_x=12 pad_right=20\n"
"  text_y=-2           nudge every baseline, logical px, negative = up\n"
"  markup=1            parse the pango subset (<span foreground=..>, &amp;)\n"
"\n"
"cells — the right hand side, one command per cell\n"
"  cell=NAME           declare a cell; the order of these lines is the order\n"
"                      the cells are drawn in, left to right\n"
"  NAME.cmd=CMD        the shell command; its output is the cell text\n"
"  NAME.interval=2     seconds between runs. 0 = run once, -1 = keep the\n"
"                      command running and take every line it prints\n"
"  NAME.fmt=%s C       %s is replaced by the output\n"
"  NAME.color=b3c0cd   text colour\n"
"  NAME.bg=            cell background\n"
"  NAME.warn=>70:cecb00   recolour when the first number in the output is at\n"
"  NAME.crit=<12:ff2222   or above (>) / at or below (<) the threshold\n"
"  NAME.min_w=280      keep this width even when the text is shorter\n"
"  NAME.max_w=0        clip beyond this width\n"
"  NAME.align=right    left | center | right, inside min_w\n"
"  NAME.pos=right      which group the cell joins: left | center | right\n"
"  NAME.scroll=0       1 = a text cell: too long is cut with '..' and scrolls\n"
"                      through while the pointer is on it. Pair with min_w\n"
"  scroll_speed=45 scroll_pause=900\n"
"\n"
"layout — one line that places everything\n"
"  bar={workspaces||cmus,volume,(clock,date)}\n"
"    ||   splits groups: 1 = left, 2 = left+right, 3 = left+center+right\n"
"    ,    separates items    (..)  glues items with no separator between\n"
"    names are cell names plus the built-ins 'workspaces' and 'mode'\n"
"  It sets position, order and separators, so keep it last in the config.\n"
"  NAME.sep=1          draw the separator after this cell\n"
"  NAME.hide_empty=1   no output = the cell and its separator disappear\n"
"  NAME.empty=TEXT     drawn instead of hiding, so the area stays clickable\n"
"  NAME.gap=           space after this cell; 0 glues it to the next one and\n"
"                      equal background colours merge into a single box\n"
"  NAME.pad=0          padding inside the cell, left and right\n"
"  NAME.slim=auto      folded strip: auto | tick | bar | clock | presence\n"
"                      | off. presence = a block in the running colour that\n"
"                      is there only while the program is\n"
"  NAME.slim_color=    override this cell's colour on the strip\n"
"  NAME.slim_min=0 NAME.slim_max=100   the range a gauge maps\n"
"  NAME.slim_w=0       width in the folded strip, 0 = the mode's default\n"
"  NAME.markup=1       parse pango markup in this cell's output\n"
"  NAME.button1=CMD    click this cell\n"
"  cell_gap=14 cell_inset=3 cell_radius=5\n"
"  separator=  separator_color=5a6b7aff\n"
"\n"
"status (used only when no cells are configured)\n"
"  status_command=     shell command; every line it prints becomes the status\n"
"  interval=1          seconds before restarting a command that exited\n"
"\n"
"messages\n"
"  a fifo other programs can shout into, or `swbr --msg TEXT`\n"
"  msg_fifo=           default $XDG_RUNTIME_DIR/swbr.fifo\n"
"  msg_target=         name of the cell a message takes over while it lasts\n"
"  msg_timeout=8       seconds, 0 = until cleared. Click it to dismiss\n"
"  msg_flash=1         paint the whole folded strip in the message colour\n"
"  msg_info= msg_warn= msg_error=\n"
"\n"
"folding\n"
"  hide_key=space      hover the bar and press this to fold it into a strip\n"
"  hide_keycode=0      raw evdev code, overrides hide_key\n"
"  hover_keys=1        grab the keyboard while the pointer is over the bar\n"
"  collapsed_px=6      height of the folded strip, logical px\n"
"  anim_ms=120         fold animation, 0 = instant\n"
"  signals=1           the folded strip keeps saying something: a fixed slot\n"
"                      per workspace, twelve dots for a clock cell, a gauge\n"
"                      for anything with a percentage\n"
"  slim_ws_slots=10    at most this many workspace slots\n"
"  start_collapsed=0\n"
"  SIGUSR1 folds or unfolds every bar:  pkill -USR1 swbr\n"
"\n"
"mouse\n"
"  button1..button9=CMD   shell command per button (1 left, 2 middle,\n"
"                         3 right, 4/5 wheel, 6/7 tilt, 8/9 side)\n"
"\n"
"colors (#rrggbb or #rrggbbaa)\n"
"  bg text dim accent hl urgent outline\n"
"  ws_bg ws_fg ws_focused_bg ws_focused_fg ws_visible_bg ws_visible_fg\n"
"  ws_urgent_bg ws_urgent_fg mode_bg mode_fg\n"
"  running             the folded strip's 'this program is up' colour\n", stdout);
}

static void print_color(const char *k, Col c)
{
    printf("%s=%02x%02x%02x%02x\n", k, c.r, c.g, c.b, c.a);
}

static void dump_config(void)
{
    Config c;
    config_defaults(&c);
    printf("# SwBr config  —  ~/.config/swbr/config\n"
           "# One key=value per line. '#' starts a comment.\n"
           "# Every key also works on the command line; command line wins.\n\n");
    printf("# --- placement ---\n");
    printf("layer=%s\nposition=%s\nheight=%d\nexclusive=%d\nmargin=%d\nside_margin=%d\n"
           "min_width=%d\nalign_x=%s\nradius=%g\noutputs=%s\n",
           c.layer, c.position, c.height, c.exclusive, c.margin, c.side_margin,
           c.min_width, c.align_x, (double)c.radius, c.outputs);
    printf("\n# --- workspaces ---\n");
    printf("ws_names=%d\nws_pad=%d\nws_gap=%d\nws_min_w=%d\nws_inset=%d\nws_radius=%g\n"
           "ws_click=%d\nscroll_workspace=%d\nmode_show=%d\n",
           c.ws_names, c.ws_pad, c.ws_gap, c.ws_min_w, c.ws_inset, (double)c.ws_radius,
           c.ws_click, c.scroll_workspace, c.mode_show);
    printf("\n# --- text ---\n");
    printf("# font=\n# font_alt=\nui_scale=%g\ntext_px=%g\nws_px=%g\ntext_y=%g\n"
           "pad_x=%d\npad_right=%d\nmarkup=%d\nscroll_speed=%d\nscroll_pause=%d\n",
           (double)c.ui_scale, (double)c.text_px, (double)c.ws_px, (double)c.text_y,
           c.pad_x, c.pad_right, c.markup, c.scroll_speed, c.scroll_pause);
    printf("\n# --- cells (the right hand side) ---\n");
    printf("cell_gap=%d\ncell_inset=%d\ncell_radius=%g\nseparator=%s\n",
           c.cell_gap, c.cell_inset, (double)c.cell_radius, c.separator);
    print_color("separator_color", c.separator_color);
    printf("# cell=clock\n# clock.cmd=date '+%%H:%%M'\n# clock.interval=10\n");
    printf("\n# --- messages ---\n");
    printf("# msg_fifo=\n# msg_target=cmus\nmsg_timeout=%d\nmsg_flash=%d\n",
           c.msg_timeout, c.msg_flash);
    print_color("msg_info", c.msg_info);
    print_color("msg_warn", c.msg_warn);
    print_color("msg_error", c.msg_error);
    printf("\n# --- layout: keep this last, it places everything ---\n");
    printf("# bar={workspaces||cmus,volume,(clock,date)}\n");
    printf("\n# --- status (only when no cells are configured) ---\n");
    printf("# status_command=~/bin/sway_bar_status.sh\ninterval=%d\n", c.interval);
    printf("\n# --- folding ---\n");
    printf("hide_key=%s\nhover_keys=%d\ncollapsed_px=%d\nanim_ms=%d\nsignals=%d\n"
           "slim_ws_slots=%d\n"
           "start_collapsed=%d\n",
           c.hide_key, c.hover_keys, c.collapsed_px, c.anim_ms, c.signals,
           c.slim_ws_slots, c.start_collapsed);
    printf("\n# --- mouse ---\n# button2=cmus_control toggle\n");
    printf("\n# --- colors ---\n");
    print_color("bg", c.bg);
    print_color("text", c.text);
    print_color("dim", c.dim);
    print_color("accent", c.accent);
    print_color("hl", c.hl);
    print_color("urgent", c.urgent);
    print_color("running", c.running);
    print_color("outline", c.outline);
    print_color("ws_bg", c.ws_bg);
    print_color("ws_fg", c.ws_fg);
    print_color("ws_focused_bg", c.ws_focused_bg);
    print_color("ws_focused_fg", c.ws_focused_fg);
    print_color("ws_visible_bg", c.ws_visible_bg);
    print_color("ws_visible_fg", c.ws_visible_fg);
    print_color("ws_urgent_bg", c.ws_urgent_bg);
    print_color("ws_urgent_fg", c.ws_urgent_fg);
    print_color("mode_bg", c.mode_bg);
    print_color("mode_fg", c.mode_fg);
}

/* ------------------------------------------------------------------ main */

static int  probe = 0;
static int  cfg_start_collapsed_arg = 0;

static void probe_report(void)
{
    printf("SwBr %s (build %s)\n", SWBR_VERSION, SWBR_BUILD);
    printf("font          %s\n", *font_path_used ? font_path_used : "(none found)");
    printf("font_alt      %s\n", *font_alt_used ? font_alt_used : "(none)");
    printf("bar height    %d logical px  (height=%d text_px=%g ui_scale=%g)\n",
           bar_height_logical(), cfg.height, (double)cfg.text_px, (double)cfg.ui_scale);
    printf("min_width     %d  (0 = full output width)\n", cfg.min_width);
    printf("outputs=      '%s'%s\n", cfg.outputs,
           *cfg.outputs ? "  (only these get a bar)" : "  (every output)");
    printf("position=%s layer=%s exclusive=%d cells=%d\n",
           cfg.position, cfg.layer, cfg.exclusive, cfg.cell_count);
    for (int i = 0; i < output_count; ++i) {
        Output *o = &outputs[i];
        if (!o->alive) continue;
        printf("output %-10s scale %d  wanted %d  bar %s\n",
               *o->name ? o->name : "(unnamed)", o->scale,
               (int)output_wanted(o->name), o->bar ? "yes" : "NO");
    }
    for (int i = 0; i < output_count; ++i) {
        Output *o = &outputs[i];
        if (!o->alive || !o->bar) continue;
        Bar *b = o->bar;
        Font *f = font_get((int)lroundf(cfg.text_px * cfg.ui_scale * b->scale));
        printf("bar %-10s surface %dx%d logical, %dx%d px, scale %d\n",
               o->name, b->w, b->h, b->bw, b->bh, b->scale);
        if (cfg.min_width > 0 && b->bw > 0) {
            Canvas probe_cv = { NULL, b->bw, b->bh, 0, 0, b->bw, b->bh };
            printf("            content wants %d logical px (min_width %d)\n",
                   bar_natural_width(b, &probe_cv, f,
                                     (float)bar_height_logical() * b->scale,
                                     (float)b->scale), cfg.min_width);
        }
        printf("            font px %d ascent %.1f lineh %.1f baseline %.1f of %d\n",
               f->px, (double)f->ascent, (double)f->lineh,
               (double)baseline_for(f, 0, (float)b->bh, (float)b->scale), b->bh);
    }
    for (int i = 0; i < cfg.cell_count; ++i) {
        Cell *e = &cfg.cell[i];
        printf("cell %-10s iv=%-3d min_w=%-4d sep=%d  '%s'\n",
               e->name, e->interval, e->min_w, e->sep, e->out);
    }
    printf("folded strip (%d px), right side left to right:\n", cfg.collapsed_px);
    for (int i = 0; i < layout_n[G_RIGHT]; ++i) {
        Item *it = &layout_item[G_RIGHT][i];
        if (it->kind != IT_CELL) continue;
        Cell *e = &cfg.cell[it->cell];
        if (!cell_visible(e) || !*e->out) { printf("  %-10s hidden\n", e->name); continue; }
        int m = slim_mode(e);
        Col c = cell_color(e);
        float v = 0;
        bool have = cell_number(e->out, &v);
        const char *mn = m == SLIM_BAR ? "gauge" : m == SLIM_CLOCK ? "clock"
                       : m == SLIM_OFF ? "off"
                       : m == SLIM_PRESENCE ? "up" : "tick";
        printf("  %-10s %-5s %3.0fpx %02x%02x%02x", e->name, mn,
               (double)slim_width_of(e, m, 1.0f), c.r, c.g, c.b);
        if (m == SLIM_BAR && have)
            printf("  %.1f of %g..%g = %.0f%% full",
                   (double)v, (double)e->slim_min, (double)e->slim_max,
                   (double)(clampf((v - e->slim_min) / (e->slim_max - e->slim_min),
                                   0.0f, 1.0f) * 100.0f));
        printf("  '%s'\n", e->out);
    }
}

/* one redraw when a timed message runs out */
static bool msg_expiry_due(void)
{
    static bool was = false;
    bool now_on = msg_active();
    bool changed = was != now_on;
    was = now_on;
    return changed;
}

static volatile sig_atomic_t sig_toggle = 0, sig_quit = 0;

static void on_signal(int s)
{
    if (s == SIGUSR1) sig_toggle = 1;
    else sig_quit = 1;
}

int main(int argc, char **argv)
{
    char cfgpath[PATH_MAX];
    const char *xdg = getenv("XDG_CONFIG_HOME"), *home = getenv("HOME");
    if (xdg && *xdg) snprintf(cfgpath, sizeof(cfgpath), "%s/swbr/config", xdg);
    else snprintf(cfgpath, sizeof(cfgpath), "%s/.config/swbr/config", home ? home : ".");

    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--config") && i + 1 < argc)
            expand_tilde(argv[++i], cfgpath, sizeof(cfgpath));
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) { usage(); return 0; }
        else if (!strcmp(argv[i], "--version") || !strcmp(argv[i], "-v")) {
            printf("SwBr %s (build %s)\n", SWBR_VERSION, SWBR_BUILD); return 0;
        } else if (!strcmp(argv[i], "--dump-config")) { dump_config(); return 0; }
        else if (!strcmp(argv[i], "--probe")) probe = 1;
        else if (!strcmp(argv[i], "--slim") || !strcmp(argv[i], "--collapsed"))
            cfg_start_collapsed_arg = 1;
    }

    config_defaults(&cfg);
    sw_shared_apply("swbr", config_set_shared, &cfg);   /* shared first */
    config_load(&cfg, cfgpath);                         /* our own wins  */

    for (int i = 1; i < argc; ++i)                  /* talk to a running bar */
        if (!strcmp(argv[i], "--msg") || !strcmp(argv[i], "-m")) {
            if (i + 1 >= argc) die("--msg needs a text ('warn: ..', 'clear')");
            return msg_send(argv[i + 1]);
        }

    for (int i = 1; i < argc; ++i) {                 /* command line wins */
        const char *a = argv[i];
        if (!strcmp(a, "--config")) { i++; continue; }
        if (!strcmp(a, "--probe") || !strcmp(a, "--slim") ||
            !strcmp(a, "--collapsed")) continue;
        if (!strcmp(a, "-s") && i + 1 < argc) a = argv[++i];
        else if (!strncmp(a, "--", 2)) a += 2;
        char buf[4096];
        str_set(buf, sizeof(buf), a);
        char *eq = strchr(buf, '=');
        if (!eq) continue;
        *eq = 0;
        config_set(&cfg, trim(buf), trim(eq + 1));
    }

    signal(SIGUSR1, on_signal);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);

    sway_fd = sway_connect();
    if (sway_fd < 0) die("cannot reach sway (is SWAYSOCK set?)");
    sway_subscribe_events();
    ws_reload(cfg.ws_names != 0);

    if (cfg_start_collapsed_arg) cfg.start_collapsed = 1;
    layout_parse(cfg.layout);
    fonts_init();

    dpy = wl_display_connect(NULL);
    if (!dpy) die("cannot connect to the wayland display");
    registry = wl_display_get_registry(dpy);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(dpy);                       /* globals */
    wl_display_roundtrip(dpy);                       /* output names, seats */

    if (!compositor || !shm) die("compositor is missing wl_compositor/wl_shm");
    if (!layer_shell) die("compositor does not support wlr-layer-shell");

    for (int i = 0; i < output_count; ++i)
        if (outputs[i].alive && !outputs[i].bar) bar_create(&outputs[i]);

    if (*cfg.status_command && !cfg.cell_count) status_spawn();
    cells_tick();
    msg_open();

    if (probe) {
        for (int i = 0; i < 60; ++i) {          /* give the cells a moment */
            cells_read();
            usleep(20000);
        }
        wl_display_roundtrip(dpy);
        probe_report();
        cells_stop();
        return 0;
    }

    int wlfd = wl_display_get_fd(dpy);
    while (running && !sig_quit) {
        bool redraw = false;

        if (sig_toggle) {
            sig_toggle = 0;
            bool any_open = false;
            for (int i = 0; i < output_count; ++i)
                if (outputs[i].bar && !outputs[i].bar->collapsed) any_open = true;
            for (int i = 0; i < output_count; ++i)
                if (outputs[i].bar) bar_set_collapsed(outputs[i].bar, any_open);
            redraw = true;
        }

        if (sway_events_pending()) { ws_reload(cfg.ws_names != 0); redraw = true; }
        if (status_read()) redraw = true;
        cells_tick();
        if (cells_read()) redraw = true;
        if (msg_read()) redraw = true;
        if (msg_expiry_due()) redraw = true;

        if (status_fd < 0 && *cfg.status_command && status_next_spawn &&
            (int32_t)(now_ms() - status_next_spawn) >= 0) {
            status_next_spawn = 0;
            status_spawn();
        }

        uint32_t t = now_ms();
        bool animating = false;
        for (int i = 0; i < output_count; ++i) {
            Bar *b = outputs[i].bar;
            if (!b) continue;
            bar_anim_step(b, t);
            if (bar_animating(b)) animating = true;
        }
        if (redraw) damage_all();

        scroll_running = false;
        for (int i = 0; i < output_count; ++i) {
            Bar *b = outputs[i].bar;
            if (b && b->dirty) bar_render(b);
        }
        if (scroll_running) damage_all();

        while (wl_display_prepare_read(dpy) != 0) wl_display_dispatch_pending(dpy);
        wl_display_flush(dpy);

        struct pollfd pfd[4 + MAX_CELLS];
        int n = 0;
        pfd[n].fd = wlfd;        pfd[n].events = POLLIN; n++;
        pfd[n].fd = sway_evt_fd; pfd[n].events = POLLIN; n++;
        if (status_fd >= 0) { pfd[n].fd = status_fd; pfd[n].events = POLLIN; n++; }
        if (msg_fd >= 0) { pfd[n].fd = msg_fd; pfd[n].events = POLLIN; n++; }
        for (int i = 0; i < cfg.cell_count; ++i)
            if (cfg.cell[i].fd >= 0) { pfd[n].fd = cfg.cell[i].fd; pfd[n].events = POLLIN; n++; }

        int timeout = -1;
        if (animating || scroll_running) timeout = 16;
        else if (cfg.cell_count || msg_until) timeout = 100;
        else if (status_fd < 0 && *cfg.status_command) timeout = 100;
        else {
            bool any_dirty = false;
            for (int i = 0; i < output_count; ++i)
                if (outputs[i].bar && outputs[i].bar->dirty) any_dirty = true;
            if (any_dirty) timeout = 16;
        }

        int pr = poll(pfd, (nfds_t)n, timeout);
        if (pr < 0 && errno != EINTR) { wl_display_cancel_read(dpy); break; }

        if (pfd[0].revents & POLLIN) {
            if (wl_display_read_events(dpy) < 0) break;
        } else {
            wl_display_cancel_read(dpy);
        }
        if (wl_display_dispatch_pending(dpy) < 0) break;
        if ((pfd[0].revents & (POLLERR | POLLHUP)) != 0) break;
    }

    status_stop();
    cells_stop();
    if (msg_fd >= 0) { close(msg_fd); unlink(msg_path); }
    for (int i = 0; i < output_count; ++i) bar_destroy(&outputs[i]);
    if (dpy) { wl_display_flush(dpy); wl_display_disconnect(dpy); }
    if (sway_fd >= 0) close(sway_fd);
    if (sway_evt_fd >= 0) close(sway_evt_fd);
    return 0;
}
