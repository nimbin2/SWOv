/* swov — a fast window/workspace overview for Sway (SDL3)
 *
 * Build:
 *   cc -std=c11 -O2 -Wall -Wextra -o swov swov.c \
 *      $(pkg-config --cflags --libs sdl3 sdl3-image sdl3-ttf)
 *
 * Runtime deps: a running Sway session ($SWAYSOCK). No jq, no shell-outs:
 * the program talks to the Sway IPC socket directly. fontconfig (fc-match)
 * is used if present to pick the desktop font, otherwise a few well known
 * font paths are tried.
 *
 * See README.md for configuration and key bindings.
 */

#define _POSIX_C_SOURCE 200809L

#include <SDL3/SDL.h>
#include <SDL3_image/SDL_image.h>
#include <SDL3_ttf/SDL_ttf.h>

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <limits.h>
#include <unistd.h>

#define APP_ID          "swov"
#define SWOV_VERSION    "1.0"
#define MAX_WINDOWS     512
#define MAX_WORKSPACES  64
#define MAX_DESKTOPS    4096

/* ------------------------------------------------------------------ util */

static void die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("swov: ", stderr);
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

static char *xstrdup(const char *s)
{
    size_t n = strlen(s ? s : "") + 1;
    char *p = (char *)xmalloc(n);
    memcpy(p, s ? s : "", n);
    return p;
}

static char *fmt_alloc(const char *fmt, ...)
{
    if (!fmt) return xstrdup("");
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) return xstrdup("");
    char *buf = (char *)xmalloc((size_t)n + 1);
    va_start(ap, fmt);
    vsnprintf(buf, (size_t)n + 1, fmt, ap);
    va_end(ap);
    return buf;
}

static bool file_readable(const char *p) { return p && access(p, R_OK) == 0; }

static bool is_dir(const char *p)
{
    struct stat st;
    return p && stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *str_trim(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n-1]==' '||s[n-1]=='\t'||s[n-1]=='\r'||s[n-1]=='\n')) s[--n] = 0;
    return s;
}

static int ci_cmp(const char *a, const char *b)
{
    while (*a && *b) {
        int ca = tolower((unsigned char)*a++);
        int cb = tolower((unsigned char)*b++);
        if (ca != cb) return ca - cb;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

/* case-insensitive substring search (used by the filter) */
static bool ci_contains(const char *hay, const char *needle)
{
    if (!needle || !*needle) return true;
    if (!hay) return false;
    size_t nl = strlen(needle);
    for (const char *p = hay; *p; ++p) {
        size_t i = 0;
        while (i < nl && p[i] &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) i++;
        if (i == nl) return true;
    }
    return false;
}

static bool str_all_digits(const char *s)
{
    if (!s || !*s) return false;
    for (const char *p = s; *p; ++p) if (!isdigit((unsigned char)*p)) return false;
    return true;
}

/* ---------------------------------------------------------------- colors */

static SDL_FColor rgba(uint32_t v)
{
    return (SDL_FColor){ ((v >> 24) & 0xff) / 255.0f,
                         ((v >> 16) & 0xff) / 255.0f,
                         ((v >>  8) & 0xff) / 255.0f,
                         ((v      ) & 0xff) / 255.0f };
}

static bool parse_color(const char *s, SDL_FColor *out)
{
    if (!s) return false;
    while (*s == '#' || *s == ' ') s++;
    size_t n = strlen(s);
    if (n != 6 && n != 8) return false;
    char buf[9];
    memcpy(buf, s, n);
    buf[n] = 0;
    for (size_t i = 0; i < n; ++i) if (!isxdigit((unsigned char)buf[i])) return false;

    unsigned long v = strtoul(buf, NULL, 16);
    if (n == 6) v = (v << 8) | 0xff;
    *out = rgba((uint32_t)v);
    return true;
}

/* c blended towards `to` by t (0..1); alpha kept from c */
static SDL_FColor mix(SDL_FColor c, SDL_FColor to, float t)
{
    return (SDL_FColor){ c.r + (to.r - c.r) * t,
                         c.g + (to.g - c.g) * t,
                         c.b + (to.b - c.b) * t,
                         c.a + (to.a - c.a) * t };
}

static SDL_FColor with_alpha(SDL_FColor c, float a) { c.a = a; return c; }

/* ---------------------------------------------------------------- config */

typedef struct {
    /* rendering */
    int   ssaa;              /* 1..4 full scene supersampling */
    int   icons;             /* show .desktop icons */
    int   icon_px;           /* icon edge length (logical px, before ui_scale) */
    int   shadow;
    int   vsync;

    /* text */
    float ui_scale;
    int   ws_px;             /* workspace number badge */
    int   label_px;          /* app name on a window card */
    int   title_px;          /* window title (the long text) */
    int   hint_px;           /* footer / header text */
    char  font[512];
    char  font_bold[512];

    /* layout */
    int   cols, rows;        /* 0 = automatic */
    float margin, gap, pad, radius, border;
    float win_gap;           /* space between two window cards            */
    float badge_top;         /* space above the workspace number          */
    float float_alpha;       /* opacity of floating / fullscreen cards    */
    float screen_pad;        /* border between mini screen and the cards  */
    int   shadow_layers;
    int   usage_dots;        /* dot scale down the left edge of a tile     */
    int   track;             /* record how long each workspace is used     */
    int   dot_count;         /* how many dots the scale has                */
    float dot_px;            /* dot diameter                               */
    float anim_ms;           /* tile glide duration, 0 disables            */
    int   start_selection;   /* 0 none, 1 workspace, 2 first window       */
    char  header_pos[16];    /* none|top-left|top-center|...|bottom-right */
    char  hints_pos[16];
    int   show_empty;
    int   all_outputs;
    int   show_header;
    int   show_hints;
    int   quit_on_focus_loss;
    int   quit_after_action;

    /* colors */
    SDL_FColor bg, tile, tile_sel, tile_hover, mini_bg;
    SDL_FColor card, card_hover, card_focus;
    SDL_FColor hl, text, subtext, dim, accent, hltext, shadow_col, outline, urgent, hint;
    SDL_FColor current;      /* what sway is showing right now             */
    SDL_FColor match;        /* windows the search found                   */
} Cfg;

static Cfg cfg_defaults(void)
{
    Cfg c;
    memset(&c, 0, sizeof(c));

    c.ssaa       = 2;
    c.icons      = 1;
    c.icon_px    = 40;
    c.shadow     = 1;
    c.vsync      = 1;

    c.ui_scale   = 1.0f;
    c.ws_px      = 26;
    c.label_px   = 16;
    c.title_px   = 13;
    c.hint_px    = 14;

    c.cols = c.rows = 0;
    c.margin  = 26.0f;
    c.win_gap    = 5.0f;
    c.badge_top  = 7.0f;
    c.float_alpha = 0.62f;
    c.screen_pad = 6.0f;
    c.shadow_layers = 3;
    c.usage_dots = 1;
    c.track      = 1;
    c.dot_count  = 14;
    c.dot_px     = 5.0f;
    c.anim_ms = 160.0f;
    c.start_selection = 1;
    snprintf(c.header_pos, sizeof(c.header_pos), "%s", "top-right");
    snprintf(c.hints_pos,  sizeof(c.hints_pos),  "%s", "bottom-center");
    c.gap     = 14.0f;
    c.pad     = 10.0f;
    c.radius  = 14.0f;
    c.border  = 3.0f;

    c.show_empty         = 1;
    c.all_outputs        = 0;
    c.show_header        = 1;
    c.show_hints         = 1;
    c.quit_on_focus_loss = 1;
    c.quit_after_action  = 0;

    /* palette: same family as the appwheel config */
    c.bg         = rgba(0x0d1117cc);  /* dims whatever is behind the overlay*/
    c.tile       = rgba(0x1e2733f2);  /* workspace tile                     */
    c.tile_sel   = rgba(0x26313ff2);  /* selected workspace tile            */
    c.tile_hover = rgba(0x2b3644f2);  /* hovered workspace tile             */
    c.mini_bg    = rgba(0x11171f9e);  /* inset "screen" inside a tile       */
    c.card       = rgba(0x33404ff7);  /* window card                        */
    c.card_hover = rgba(0x46566af7);  /* hovered window card                */
    c.card_focus = rgba(0x3b4a5bf7);  /* card of the window sway focuses    */
    c.hl         = rgba(0xcb9b00ff);  /* the orange                         */
    c.text       = rgba(0xe8e8e8ff);
    c.subtext    = rgba(0xb3c0cdff);
    c.dim        = rgba(0x5a6b7aff);
    c.accent     = rgba(0x89afc4ff);
    c.hltext     = rgba(0x141414ff);
    c.shadow_col = rgba(0x00000073);
    c.outline    = rgba(0x0a0e1499);
    c.urgent     = rgba(0xe0533cff);
    c.hint       = rgba(0xa7b5c4ff);  /* header line and the key hints       */
    c.current    = rgba(0x4fb3a5ff);  /* the live workspace / focused window */
    c.match      = rgba(0xb58ae0ff);  /* search hits                         */
    return c;
}

static bool key_is(const char *k, const char *a) { return ci_cmp(k, a) == 0; }

static void cfg_set(Cfg *c, const char *k, const char *v)
{
    /* rendering */
    if (key_is(k,"ssaa") || key_is(k,"aa")) {
        c->ssaa = atoi(v); if (c->ssaa < 1) c->ssaa = 1; if (c->ssaa > 4) c->ssaa = 4;
    }
    else if (key_is(k,"icons"))        c->icons = atoi(v) != 0;
    else if (key_is(k,"icon_px"))      c->icon_px = atoi(v);
    else if (key_is(k,"shadow"))       c->shadow = atoi(v) != 0;
    else if (key_is(k,"vsync"))        c->vsync = atoi(v) != 0;

    /* text */
    else if (key_is(k,"ui_scale") || key_is(k,"font_scale") || key_is(k,"text_scale"))
        c->ui_scale = (float)atof(v);
    else if (key_is(k,"ws_px") || key_is(k,"workspace_px")) c->ws_px = atoi(v);
    else if (key_is(k,"label_px") || key_is(k,"app_px"))    c->label_px = atoi(v);
    else if (key_is(k,"title_px"))                          c->title_px = atoi(v);
    else if (key_is(k,"hint_px") || key_is(k,"count_px"))   c->hint_px = atoi(v);
    else if (key_is(k,"font"))       snprintf(c->font, sizeof(c->font), "%s", v);
    else if (key_is(k,"font_bold"))  snprintf(c->font_bold, sizeof(c->font_bold), "%s", v);

    /* layout */
    else if (key_is(k,"cols"))          c->cols = atoi(v);
    else if (key_is(k,"rows"))          c->rows = atoi(v);
    else if (key_is(k,"margin"))        c->margin = (float)atof(v);
    else if (key_is(k,"gap"))           c->gap = (float)atof(v);
    else if (key_is(k,"pad"))           c->pad = (float)atof(v);
    else if (key_is(k,"radius") || key_is(k,"corner")) c->radius = (float)atof(v);
    else if (key_is(k,"border"))        c->border = (float)atof(v);
    else if (key_is(k,"win_gap") || key_is(k,"window_gap")) c->win_gap = (float)atof(v);
    else if (key_is(k,"screen_pad"))    c->screen_pad = (float)atof(v);
    else if (key_is(k,"badge_top") || key_is(k,"badge_scale")) c->badge_top = (float)atof(v);
    else if (key_is(k,"float_alpha") || key_is(k,"floating_alpha"))
        c->float_alpha = (float)atof(v);
    else if (key_is(k,"shadow_layers")) c->shadow_layers = atoi(v);
    else if (key_is(k,"usage_dots") || key_is(k,"usage_bar"))
        c->usage_dots = atoi(v) != 0;
    else if (key_is(k,"dot_count"))     c->dot_count = atoi(v);
    else if (key_is(k,"dot_px"))        c->dot_px = (float)atof(v);
    else if (key_is(k,"track"))         c->track = atoi(v) != 0;
    else if (key_is(k,"anim_ms") || key_is(k,"animation")) c->anim_ms = (float)atof(v);
    else if (key_is(k,"start_selection")) {
        c->start_selection = key_is(v,"none") ? 0 : key_is(v,"window") ? 2 : 1;
    }
    else if (key_is(k,"header_pos")) snprintf(c->header_pos, sizeof(c->header_pos), "%s", v);
    else if (key_is(k,"hints_pos"))  snprintf(c->hints_pos,  sizeof(c->hints_pos),  "%s", v);
    else if (key_is(k,"show_empty"))    c->show_empty = atoi(v) != 0;
    else if (key_is(k,"all_outputs"))   c->all_outputs = atoi(v) != 0;
    else if (key_is(k,"show_header"))   c->show_header = atoi(v) != 0;
    else if (key_is(k,"show_hints"))    c->show_hints = atoi(v) != 0;
    else if (key_is(k,"quit_on_focus_loss")) c->quit_on_focus_loss = atoi(v) != 0;
    else if (key_is(k,"quit_after_action"))  c->quit_after_action = atoi(v) != 0;

    /* colors */
    else if (key_is(k,"bg"))          parse_color(v, &c->bg);
    else if (key_is(k,"tile") || key_is(k,"ring"))       parse_color(v, &c->tile);
    else if (key_is(k,"tile_sel") || key_is(k,"ring2"))  parse_color(v, &c->tile_sel);
    else if (key_is(k,"tile_hover"))  parse_color(v, &c->tile_hover);
    else if (key_is(k,"mini_bg") || key_is(k,"center"))  parse_color(v, &c->mini_bg);
    else if (key_is(k,"card"))        parse_color(v, &c->card);
    else if (key_is(k,"card_hover") || key_is(k,"hover")) parse_color(v, &c->card_hover);
    else if (key_is(k,"card_focus"))  parse_color(v, &c->card_focus);
    else if (key_is(k,"hl"))          parse_color(v, &c->hl);
    else if (key_is(k,"text"))        parse_color(v, &c->text);
    else if (key_is(k,"subtext"))     parse_color(v, &c->subtext);
    else if (key_is(k,"dim"))         parse_color(v, &c->dim);
    else if (key_is(k,"accent"))      parse_color(v, &c->accent);
    else if (key_is(k,"hltext"))      parse_color(v, &c->hltext);
    else if (key_is(k,"shadow_color")) parse_color(v, &c->shadow_col);
    else if (key_is(k,"outline"))     parse_color(v, &c->outline);
    else if (key_is(k,"urgent"))      parse_color(v, &c->urgent);
    else if (key_is(k,"hint"))        parse_color(v, &c->hint);
    else if (key_is(k,"current"))     parse_color(v, &c->current);
    else if (key_is(k,"match"))       parse_color(v, &c->match);
    else fprintf(stderr, "swov: unknown config key '%s' (ignored)\n", k);
}

static char *expand_tilde(const char *p)
{
    if (p && p[0] == '~' && (p[1] == '/' || p[1] == 0)) {
        const char *home = getenv("HOME");
        if (home) return fmt_alloc("%s%s", home, p + 1);
    }
    return xstrdup(p ? p : "");
}

static bool cfg_load_file(Cfg *c, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *s = str_trim(line);
        if (!*s || *s == '#' || *s == ';') continue;
        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = 0;
        char *k = str_trim(s);
        char *v = str_trim(eq + 1);
        char *hash = strchr(v, '#');           /* trailing comment */
        if (hash && hash > v && hash[-1] == ' ') { *hash = 0; v = str_trim(v); }
        if (*k) cfg_set(c, k, v);
    }
    fclose(f);
    return true;
}

static char  CFG_PATH[512];
static bool  CFG_LOADED;

static char *default_config_path(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return fmt_alloc("%s/swov/config", xdg);
    const char *home = getenv("HOME");
    if (home) return fmt_alloc("%s/.config/swov/config", home);
    return NULL;
}

/* ----------------------------------------------------------- json parser
 * Just enough JSON to read sway's IPC replies: no streaming, no comments,
 * numbers as double, strings unescaped to UTF-8.
 */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JV JV;
struct JV {
    JType type;
    bool   b;
    double num;
    char  *str;              /* J_STR */
    JV   **items;            /* J_ARR / J_OBJ values */
    char **keys;             /* J_OBJ keys */
    int    count, cap;
};

static void jfree(JV *v)
{
    if (!v) return;
    switch (v->type) {
    case J_STR: free(v->str); break;
    case J_ARR:
        for (int i = 0; i < v->count; ++i) jfree(v->items[i]);
        free(v->items);
        break;
    case J_OBJ:
        for (int i = 0; i < v->count; ++i) { free(v->keys[i]); jfree(v->items[i]); }
        free(v->items);
        free(v->keys);
        break;
    default: break;
    }
    free(v);
}

static JV *jnew(JType t)
{
    JV *v = (JV *)xmalloc(sizeof(JV));
    memset(v, 0, sizeof(*v));
    v->type = t;
    return v;
}

static void jpush(JV *v, char *key, JV *val)
{
    if (v->count == v->cap) {
        v->cap = v->cap ? v->cap * 2 : 8;
        v->items = (JV **)xrealloc(v->items, (size_t)v->cap * sizeof(JV *));
        if (v->type == J_OBJ)
            v->keys = (char **)xrealloc(v->keys, (size_t)v->cap * sizeof(char *));
    }
    if (v->type == J_OBJ) v->keys[v->count] = key;
    v->items[v->count++] = val;
}

static void utf8_append(char **dst, size_t *len, size_t *cap, uint32_t cp)
{
    char tmp[4];
    int n = 0;
    if (cp < 0x80) { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800) {
        tmp[0] = (char)(0xc0 | (cp >> 6));
        tmp[1] = (char)(0x80 | (cp & 0x3f));
        n = 2;
    } else if (cp < 0x10000) {
        tmp[0] = (char)(0xe0 | (cp >> 12));
        tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        tmp[2] = (char)(0x80 | (cp & 0x3f));
        n = 3;
    } else {
        tmp[0] = (char)(0xf0 | (cp >> 18));
        tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
        tmp[3] = (char)(0x80 | (cp & 0x3f));
        n = 4;
    }
    if (*len + (size_t)n + 1 > *cap) {
        *cap = (*cap ? *cap * 2 : 32) + (size_t)n;
        *dst = (char *)xrealloc(*dst, *cap);
    }
    memcpy(*dst + *len, tmp, (size_t)n);
    *len += (size_t)n;
    (*dst)[*len] = 0;
}

static const char *jskip_ws(const char *p)
{
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
    return p;
}

static const char *jparse_string(const char *p, char **out)
{
    /* p points at the opening quote */
    size_t len = 0, cap = 32;
    char *s = (char *)xmalloc(cap);
    s[0] = 0;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\') {
            p++;
            uint32_t cp = 0;
            switch (*p) {
            case 'n': cp = '\n'; p++; break;
            case 't': cp = '\t'; p++; break;
            case 'r': cp = '\r'; p++; break;
            case 'b': cp = '\b'; p++; break;
            case 'f': cp = '\f'; p++; break;
            case '/': cp = '/';  p++; break;
            case '"': cp = '"';  p++; break;
            case '\\': cp = '\\'; p++; break;
            case 'u': {
                p++;
                char hex[5] = {0};
                for (int i = 0; i < 4 && p[i]; ++i) hex[i] = p[i];
                cp = (uint32_t)strtoul(hex, NULL, 16);
                p += 4;
                if (cp >= 0xd800 && cp <= 0xdbff && p[0] == '\\' && p[1] == 'u') {
                    char hex2[5] = {0};
                    for (int i = 0; i < 4 && p[2 + i]; ++i) hex2[i] = p[2 + i];
                    uint32_t lo = (uint32_t)strtoul(hex2, NULL, 16);
                    if (lo >= 0xdc00 && lo <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                        p += 6;
                    }
                }
                break;
            }
            default:
                if (!*p) { free(s); *out = NULL; return NULL; }
                cp = (unsigned char)*p++;
                break;
            }
            utf8_append(&s, &len, &cap, cp);
        } else {
            if (len + 2 > cap) { cap *= 2; s = (char *)xrealloc(s, cap); }
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
    IPC_GET_TREE       = 4,
    IPC_SUBSCRIBE      = 2
};

static int sway_fd = -1;      /* request socket  */
static int sway_evt_fd = -1;  /* event socket    */

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
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
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
    if (!fmt) return false;
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
    if (v && v->type == J_ARR) {
        for (int i = 0; i < v->count; ++i)
            if (!jbool(v->items[i], "success", true)) ok = false;
    }
    jfree(v);
    free(reply);
    return ok;
}

/* Sway answers a command as soon as it is queued, but the tree only carries
 * the new geometry once the transaction has committed. Rather than sleeping
 * and hoping, we subscribe to window and workspace events on a second socket
 * and reload when sway tells us it is done. That also keeps the overview
 * correct while it is open. */
static bool sway_subscribe_events(void)
{
    sway_evt_fd = sway_connect();
    if (sway_evt_fd < 0) return false;

    const char *payload = "[\"window\",\"workspace\"]";
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
    if (!read_all(sway_evt_fd, rhdr, sizeof(rhdr))) { close(sway_evt_fd); sway_evt_fd = -1; return false; }
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

/* true when sway reported at least one change since the last call */
static bool sway_events_pending(void)
{
    if (sway_evt_fd < 0) return false;
    bool any = false;

    for (;;) {
        char hdr[14];
        ssize_t r = recv(sway_evt_fd, hdr, sizeof(hdr), MSG_DONTWAIT);
        if (r <= 0) break;
        if ((size_t)r < sizeof(hdr)) {              /* rest of the header */
            if (!read_all(sway_evt_fd, hdr + r, sizeof(hdr) - (size_t)r)) break;
        }
        uint32_t rlen = 0;
        memcpy(&rlen, hdr + 6, 4);
        if (rlen > (16u << 20)) break;
        if (rlen) {
            char *body = (char *)xmalloc(rlen + 1);
            bool ok = read_all(sway_evt_fd, body, rlen);
            free(body);
            if (!ok) break;
        }
        any = true;
    }
    return any;
}

/* ------------------------------------------------------------ usage store
 * How long the user has had each workspace in front of them. Every workspace
 * switch goes through swov, so no daemon is needed: on each switch we credit
 * the workspace we are leaving with the time since the last switch, and write
 * down where we are going and when. A workspace that falls empty loses its
 * count — the number describes the workspace as it is now.
 *
 * File: one "> <epoch> <name>" line saying where we are, then "<seconds>
 * <name>" per workspace.
 */

typedef struct { char name[64]; double secs; } Usage;

static Usage  USAGE[128];
static int    NUSAGE;
static double USAGE_MAX = 0.0;    /* the busiest workspace, for the scale */
static char   USAGE_CUR[64];      /* the workspace we are on              */
static double USAGE_SINCE;        /* ... since this moment (epoch seconds) */

static double now_secs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static char *usage_path(void)
{
    const char *xdg = getenv("XDG_CACHE_HOME");
    if (xdg && *xdg) return fmt_alloc("%s/swov/usage", xdg);
    const char *home = getenv("HOME");
    return home ? fmt_alloc("%s/.cache/swov/usage", home) : NULL;
}

static Usage *usage_find(const char *name, bool create)
{
    for (int i = 0; i < NUSAGE; ++i)
        if (strcmp(USAGE[i].name, name) == 0) return &USAGE[i];
    if (!create || NUSAGE >= (int)SDL_arraysize(USAGE)) return NULL;

    Usage *u = &USAGE[NUSAGE++];
    snprintf(u->name, sizeof(u->name), "%s", name);
    u->secs = 0.0;
    return u;
}

static void usage_load(void)
{
    NUSAGE = 0;
    char *path = usage_path();
    if (!path) return;

    FILE *f = fopen(path, "r");
    free(path);
    if (!f) return;

    USAGE_CUR[0] = 0;
    USAGE_SINCE = 0.0;

    char line[256];
    while (fgets(line, sizeof(line), f) && NUSAGE < (int)SDL_arraysize(USAGE)) {
        double v = 0.0;
        char name[64] = {0};
        if (line[0] == '>') {
            if (sscanf(line + 1, "%lf %63[^\n]", &v, name) == 2) {
                USAGE_SINCE = v;
                snprintf(USAGE_CUR, sizeof(USAGE_CUR), "%s", name);
            }
        } else if (sscanf(line, "%lf %63[^\n]", &v, name) == 2 && name[0]) {
            Usage *u = usage_find(name, true);
            if (u) u->secs = v;
        }
    }
    fclose(f);
}

/* time spent on the workspace we are on but not yet written down */
static double usage_pending(void)
{
    if (!USAGE_CUR[0] || USAGE_SINCE <= 0.0) return 0.0;
    double d = now_secs() - USAGE_SINCE;
    if (d < 0.0 || d > 12.0 * 3600.0) return 0.0;   /* clock jump, or a nap */
    return d;
}

static void usage_save(void)
{
    char *path = usage_path();
    if (!path) return;

    char *slash = strrchr(path, '/');
    if (slash) {                       /* mkdir -p on the parent, one level */
        *slash = 0;
        char *up = strrchr(path, '/');
        if (up) { *up = 0; mkdir(path, 0755); *up = '/'; }
        mkdir(path, 0755);
        *slash = '/';
    }

    char *tmp = fmt_alloc("%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (f) {
        if (USAGE_CUR[0]) fprintf(f, "> %.0f %s\n", USAGE_SINCE, USAGE_CUR);
        for (int i = 0; i < NUSAGE; ++i)
            if (USAGE[i].secs > 0.0) fprintf(f, "%.0f %s\n", USAGE[i].secs, USAGE[i].name);
        fclose(f);
        rename(tmp, path);
    }
    free(tmp);
    free(path);
}

/* Credit the workspace we are leaving, then note where we are going. This is
 * the whole of the time tracking: swov performs every switch itself. */
static void usage_switch(const char *to)
{
    if (!to || !*to) return;
    usage_load();

    double pending = usage_pending();
    if (pending > 0.0 && USAGE_CUR[0]) {
        Usage *u = usage_find(USAGE_CUR, true);
        if (u) u->secs += pending;
    }
    snprintf(USAGE_CUR, sizeof(USAGE_CUR), "%s", to);
    USAGE_SINCE = now_secs();
    usage_save();
}

/* the workspace sway is showing at this moment */
static void focused_workspace(char *out, size_t cap)
{
    out[0] = 0;
    JV *r = sway_query(IPC_GET_WORKSPACES);
    if (r && r->type == J_ARR)
        for (int i = 0; i < r->count; ++i)
            if (jbool(r->items[i], "focused", false))
                snprintf(out, cap, "%s", jstr(r->items[i], "name", ""));
    jfree(r);
}

/* --------------------------------------------------------------- globals */

static Cfg           C;
static SDL_Renderer *REN;
static TTF_Font     *F_BADGE;   /* workspace number            */
static TTF_Font     *F_LABEL;   /* app name on a window card   */
static TTF_Font     *F_TITLE;   /* window title (small)        */
static TTF_Font     *F_HINT;    /* header / footer             */
static float         SC = 1.0f; /* supersampling scale factor  */

static int px(float logical) { return (int)(logical * SC + 0.5f); }

/* ----------------------------------------------------------------- text */

typedef struct { SDL_Texture *t; int w, h; } Tex;

static void tex_free(Tex *x)
{
    if (x->t) SDL_DestroyTexture(x->t);
    x->t = NULL;
    x->w = x->h = 0;
}

/* Text is always rendered white and tinted at draw time, so one texture can
 * be reused for normal / hovered / selected states. */
static Tex text_make(TTF_Font *f, const char *s)
{
    Tex out = {0};
    if (!f || !s || !*s) return out;
    SDL_Color white = { 255, 255, 255, 255 };
    SDL_Surface *surf = TTF_RenderText_Blended(f, s, strlen(s), white);
    if (!surf) return out;
    out.t = SDL_CreateTextureFromSurface(REN, surf);
    out.w = surf->w;
    out.h = surf->h;
    SDL_DestroySurface(surf);
    if (out.t) SDL_SetTextureScaleMode(out.t, SDL_SCALEMODE_LINEAR);
    return out;
}

/* Render `s`, shortened with an ellipsis so that it fits into max_w pixels. */
static Tex text_make_fit(TTF_Font *f, const char *s, int max_w)
{
    Tex out = {0};
    if (!f || !s || !*s || max_w <= 0) return out;

    int w = 0, h = 0;
    if (TTF_GetStringSize(f, s, strlen(s), &w, &h) && w <= max_w)
        return text_make(f, s);

    const char *ell = "\xe2\x80\xa6";           /* … */
    int ew = 0, eh = 0;
    TTF_GetStringSize(f, ell, strlen(ell), &ew, &eh);

    int avail = max_w - ew;
    if (avail <= 0) return out;

    int mw = 0;
    size_t fit = 0;
    if (!TTF_MeasureString(f, s, strlen(s), avail, &mw, &fit) || fit == 0)
        return out;

    char *buf = (char *)xmalloc(fit + strlen(ell) + 1);
    memcpy(buf, s, fit);
    memcpy(buf + fit, ell, strlen(ell) + 1);
    out = text_make(f, buf);
    free(buf);
    return out;
}

static void tex_draw(Tex x, float dx, float dy, SDL_FColor col)
{
    if (!x.t) return;
    SDL_SetTextureColorModFloat(x.t, col.r, col.g, col.b);
    SDL_SetTextureAlphaModFloat(x.t, col.a);
    SDL_FRect dst = { dx, dy, (float)x.w, (float)x.h };
    SDL_RenderTexture(REN, x.t, NULL, &dst);
}

static void tex_draw_center(Tex x, float cx, float dy, SDL_FColor col)
{
    tex_draw(x, cx - x.w * 0.5f, dy, col);
}

/* Centre a line inside a box. A text texture is a full line box: it reserves
 * room for descenders even when the string has none, so plain centring makes
 * digits and capitals look too high. Half the descent puts them right. */
static void tex_draw_in_box(Tex x, SDL_FRect box, TTF_Font *f, SDL_FColor col)
{
    if (!x.t) return;
    float desc = f ? (float)-TTF_GetFontDescent(f) : 0.0f;
    float y = box.y + (box.h - (float)x.h) * 0.5f - desc * 0.5f;
    tex_draw(x, box.x + (box.w - (float)x.w) * 0.5f, y, col);
}

/* ------------------------------------------------------------- primitives */

static void set_col(SDL_FColor c)
{
    SDL_SetRenderDrawColorFloat(REN, c.r, c.g, c.b, c.a);
}

static void fill_round_rect(SDL_FRect r, float rad, SDL_FColor c)
{
    if (r.w <= 0.0f || r.h <= 0.0f) return;
    rad = SDL_min(rad, SDL_min(r.w, r.h) * 0.5f);
    set_col(c);
    if (rad <= 0.5f) { SDL_RenderFillRect(REN, &r); return; }

    SDL_FRect mid = { r.x, r.y + rad, r.w, r.h - 2.0f * rad };
    SDL_RenderFillRect(REN, &mid);

    int steps = (int)SDL_ceilf(rad);
    for (int i = 0; i < steps; ++i) {
        float dy = rad - (float)i - 0.5f;
        float dx = SDL_sqrtf(SDL_max(0.0f, rad * rad - dy * dy));
        float x0 = r.x + rad - dx;
        float w  = r.w - 2.0f * rad + 2.0f * dx;
        SDL_FRect top = { x0, r.y + (float)i, w, 1.0f };
        SDL_FRect bot = { x0, r.y + r.h - (float)i - 1.0f, w, 1.0f };
        SDL_RenderFillRect(REN, &top);
        SDL_RenderFillRect(REN, &bot);
    }
}

/* rounded rectangle outline of thickness t, drawn inside r */
static void stroke_round_rect(SDL_FRect r, float rad, float t, SDL_FColor c)
{
    if (t <= 0.0f || r.w <= 0.0f || r.h <= 0.0f) return;
    rad = SDL_min(rad, SDL_min(r.w, r.h) * 0.5f);
    t   = SDL_min(t, SDL_min(r.w, r.h) * 0.5f);
    set_col(c);

    if (rad <= 0.5f) {
        SDL_FRect e[4] = {
            { r.x, r.y, r.w, t },
            { r.x, r.y + r.h - t, r.w, t },
            { r.x, r.y + t, t, r.h - 2.0f * t },
            { r.x + r.w - t, r.y + t, t, r.h - 2.0f * t }
        };
        SDL_RenderFillRects(REN, e, 4);
        return;
    }

    SDL_FRect edges[4] = {
        { r.x + rad, r.y, r.w - 2.0f * rad, t },                        /* top    */
        { r.x + rad, r.y + r.h - t, r.w - 2.0f * rad, t },              /* bottom */
        { r.x, r.y + rad, t, r.h - 2.0f * rad },                        /* left   */
        { r.x + r.w - t, r.y + rad, t, r.h - 2.0f * rad }               /* right  */
    };
    SDL_RenderFillRects(REN, edges, 4);

    float ri = rad - t;
    int steps = (int)SDL_ceilf(rad);
    for (int i = 0; i < steps; ++i) {
        float dy  = rad - (float)i - 0.5f;
        float dxo = SDL_sqrtf(SDL_max(0.0f, rad * rad - dy * dy));
        float dxi = (ri > 0.0f && SDL_fabsf(dy) < ri)
                        ? SDL_sqrtf(SDL_max(0.0f, ri * ri - dy * dy)) : 0.0f;
        float seg = dxo - dxi;
        if (seg <= 0.0f) continue;

        float lx = r.x + rad - dxo;
        float rx = r.x + r.w - rad + dxi;
        float ty = r.y + (float)i;
        float by = r.y + r.h - (float)i - 1.0f;

        SDL_FRect q[4] = {
            { lx, ty, seg, 1.0f },
            { rx, ty, seg, 1.0f },
            { lx, by, seg, 1.0f },
            { rx, by, seg, 1.0f }
        };
        SDL_RenderFillRects(REN, q, 4);
    }
}

/* soft drop shadow: a few expanded, fading rounded rects */
static void drop_shadow(SDL_FRect r, float rad, float size, SDL_FColor c)
{
    if (!C.shadow || size <= 0.0f) return;
    int layers = C.shadow_layers > 0 ? C.shadow_layers : 3;
    for (int i = layers; i >= 1; --i) {
        float g = size * (float)i / (float)layers;
        SDL_FRect s = { r.x - g, r.y - g, r.w + 2.0f * g, r.h + 2.0f * g };
        SDL_FColor col = with_alpha(c, c.a / (float)(layers * 2));
        fill_round_rect(s, rad + g, col);
    }
}

/* ------------------------------------------------------------ icon lookup
 * app_id (or WM_CLASS) -> .desktop file -> Icon= -> a file on disk.
 * Everything is cached: the .desktop index is built once and lazily, the
 * resulting textures are shared between windows of the same application.
 */

typedef struct {
    char *id;        /* desktop file basename without .desktop */
    char *path;
    char *wmclass;   /* StartupWMClass, may be NULL */
    char *icon;      /* Icon=, may be NULL */
    char *name;      /* Name=, may be NULL */
} DesktopEntry;

static DesktopEntry *g_desktops = NULL;
static int  g_desktop_count = 0;
static bool g_desktop_indexed = false;

static void desktop_scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *de;
    while ((de = readdir(d)) && g_desktop_count < MAX_DESKTOPS) {
        const char *n = de->d_name;
        size_t len = strlen(n);
        if (len < 9 || strcmp(n + len - 8, ".desktop") != 0) continue;

        char *path = fmt_alloc("%s/%s", dir, n);
        FILE *f = fopen(path, "r");
        if (!f) { free(path); continue; }

        char *icon = NULL, *wmclass = NULL, *dname = NULL;
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (!icon && strncmp(line, "Icon=", 5) == 0) {
                char *v = str_trim(line + 5);
                if (*v) icon = xstrdup(v);
            } else if (!wmclass && strncmp(line, "StartupWMClass=", 15) == 0) {
                char *v = str_trim(line + 15);
                if (*v) wmclass = xstrdup(v);
            } else if (!dname && strncmp(line, "Name=", 5) == 0) {
                char *v = str_trim(line + 5);
                if (*v) dname = xstrdup(v);
            }
            if (icon && wmclass && dname) break;
        }
        fclose(f);

        if (!icon && !dname) { free(wmclass); free(path); continue; }

        DesktopEntry *e = &g_desktops[g_desktop_count++];
        e->path    = path;
        e->icon    = icon;
        e->wmclass = wmclass;
        e->name    = dname;
        e->id      = xstrdup(n);
        e->id[len - 8] = 0;
    }
    closedir(d);
}

static void desktop_index_build(void)
{
    if (g_desktop_indexed) return;
    g_desktop_indexed = true;
    g_desktops = (DesktopEntry *)xmalloc(sizeof(DesktopEntry) * MAX_DESKTOPS);

    const char *home = getenv("HOME");
    const char *xdg_data_home = getenv("XDG_DATA_HOME");
    const char *xdg_data_dirs = getenv("XDG_DATA_DIRS");

    char *dirs[16];
    int nd = 0;

    if (xdg_data_home && *xdg_data_home)
        dirs[nd++] = fmt_alloc("%s/applications", xdg_data_home);
    else if (home)
        dirs[nd++] = fmt_alloc("%s/.local/share/applications", home);
    if (home) dirs[nd++] = fmt_alloc("%s/.local/share/flatpak/exports/share/applications", home);

    if (xdg_data_dirs && *xdg_data_dirs) {
        char *copy = xstrdup(xdg_data_dirs);
        char *save = NULL;
        for (char *tok = strtok_r(copy, ":", &save); tok && nd < 14; tok = strtok_r(NULL, ":", &save))
            if (*tok) dirs[nd++] = fmt_alloc("%s/applications", tok);
        free(copy);
    } else {
        dirs[nd++] = xstrdup("/usr/local/share/applications");
        dirs[nd++] = xstrdup("/usr/share/applications");
    }
    if (nd < 15) dirs[nd++] = xstrdup("/var/lib/flatpak/exports/share/applications");

    for (int i = 0; i < nd; ++i) {
        if (is_dir(dirs[i])) desktop_scan_dir(dirs[i]);
        free(dirs[i]);
    }
}

/* org.qutebrowser.qutebrowser -> qutebrowser, org.telegram.desktop -> telegram */
static const char *short_app_name(const char *app_id)
{
    static const char *generic[] = { "desktop", "app", "App", "gui", "GUI", "client",
                                     "Client", "bin", "Bin", "gtk", "qt", "Desktop" };
    if (!app_id || !*app_id) return "unknown";

    const char *dot = strrchr(app_id, '.');
    if (!dot || !dot[1] || strchr(app_id, '.') == dot) return app_id;

    for (size_t i = 0; i < SDL_arraysize(generic); ++i) {
        if (ci_cmp(dot + 1, generic[i]) != 0) continue;
        /* the last part says nothing: use the one before it */
        static char buf[128];
        const char *p = dot - 1;
        while (p > app_id && *p != '.') p--;
        if (*p == '.') p++;
        size_t n = (size_t)(dot - p);
        if (n == 0 || n >= sizeof(buf)) break;
        memcpy(buf, p, n);
        buf[n] = 0;
        return buf;
    }
    return dot + 1;
}


static const DesktopEntry *desktop_find(const char *app_id)
{
    if (!app_id || !*app_id || !C.icons) return NULL;
    desktop_index_build();

    for (int i = 0; i < g_desktop_count; ++i)                    /* exact id     */
        if (ci_cmp(g_desktops[i].id, app_id) == 0) return &g_desktops[i];
    for (int i = 0; i < g_desktop_count; ++i)                    /* WM_CLASS     */
        if (g_desktops[i].wmclass && ci_cmp(g_desktops[i].wmclass, app_id) == 0)
            return &g_desktops[i];

    const char *shrt = short_app_name(app_id);
    for (int i = 0; i < g_desktop_count; ++i)                    /* short name   */
        if (ci_cmp(g_desktops[i].id, shrt) == 0) return &g_desktops[i];
    for (int i = 0; i < g_desktop_count; ++i) {                  /* id suffix    */
        const char *dot = strrchr(g_desktops[i].id, '.');
        if (dot && ci_cmp(dot + 1, shrt) == 0) return &g_desktops[i];
    }
    return NULL;
}

/* Some toolkits hand sway an app_id that identifies nothing: GTK apps
 * launched without one report "GTK Application", and several Electron
 * builds are no better. In that case the process behind the window knows
 * more than the window does. */
static bool app_id_is_useless(const char *id)
{
    static const char *bad[] = {
        "", "gtk application", "gtk-application", "unknown", "wayland",
        "xwayland", "electron", "toplevel", "window", "main", "app"
    };
    if (!id) return true;
    for (size_t i = 0; i < SDL_arraysize(bad); ++i)
        if (ci_cmp(id, bad[i]) == 0) return true;
    return false;
}

static bool is_interpreter(const char *name)
{
    static const char *interp[] = {
        "electron", "node", "python", "python3", "java", "sh", "bash", "zsh",
        "wine", "wine64", "wine-preloader", "flatpak", "snap", "env", "steam"
    };
    for (size_t i = 0; i < SDL_arraysize(interp); ++i)
        if (ci_cmp(name, interp[i]) == 0) return true;
    return false;
}

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* argv[0] of the process, or its comm name; empty when nothing usable */
static void proc_app_name(int pid, char *out, size_t cap)
{
    out[0] = 0;
    if (pid <= 1) return;

    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    FILE *f = fopen(path, "r");
    if (f) {
        char buf[1024];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = 0;
        if (n) {
            const char *arg0 = base_name(buf);
            if (*arg0 && !is_interpreter(arg0)) {
                snprintf(out, cap, "%.*s", (int)cap - 1, arg0);
                return;
            }
            /* an interpreter: the script or the app directory names it */
            size_t off = strlen(buf) + 1;
            while (off < n && out[0] == 0) {
                const char *arg = buf + off;
                if (*arg != '-') {
                    const char *b = base_name(arg);
                    if (*b && !is_interpreter(b) && !strchr(b, '=')) {
                        snprintf(out, cap, "%.*s", (int)cap - 1, b);
                        break;
                    }
                }
                off += strlen(arg) + 1;
            }
            if (out[0]) return;
        }
    }

    snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    f = fopen(path, "r");
    if (!f) return;
    char comm[128] = {0};
    if (fgets(comm, sizeof(comm), f)) {
        str_trim(comm);
        if (comm[0] && !is_interpreter(comm)) snprintf(out, cap, "%.*s", (int)cap - 1, comm);
    }
    fclose(f);
}

/* what we print on a card: the .desktop Name= if we have one */
static const char *app_display_name(const char *app_id)
{
    const DesktopEntry *de = desktop_find(app_id);
    if (de && de->name && *de->name) return de->name;
    return short_app_name(app_id);
}

static char *icon_theme_name(void)
{
    const char *home = getenv("HOME");
    if (home) {
        char *p = fmt_alloc("%s/.config/gtk-3.0/settings.ini", home);
        FILE *f = fopen(p, "r");
        free(p);
        if (f) {
            char line[512];
            while (fgets(line, sizeof(line), f)) {
                if (strncmp(line, "gtk-icon-theme-name", 19) == 0) {
                    char *eq = strchr(line, '=');
                    if (eq) {
                        char *v = str_trim(eq + 1);
                        if (*v) { fclose(f); return xstrdup(v); }
                    }
                }
            }
            fclose(f);
        }
    }
    return xstrdup("hicolor");
}

/* The theme directories are scanned once, in preference order. Looking an
 * icon up afterwards is a handful of access() calls instead of the thousands
 * a blind size/extension sweep would cost. */
typedef struct { char *path; int size; } IconDir;
static IconDir *g_icon_dirs = NULL;
static int  g_icon_dir_count = 0, g_icon_dir_cap = 0;
static bool g_icon_dirs_built = false;

static void icon_dir_add(const char *path, int size)
{
    if (!is_dir(path)) return;
    if (g_icon_dir_count == g_icon_dir_cap) {
        g_icon_dir_cap = g_icon_dir_cap ? g_icon_dir_cap * 2 : 32;
        g_icon_dirs = (IconDir *)xrealloc(g_icon_dirs, (size_t)g_icon_dir_cap * sizeof(IconDir));
    }
    g_icon_dirs[g_icon_dir_count].path = xstrdup(path);
    g_icon_dirs[g_icon_dir_count].size = size;
    g_icon_dir_count++;
}

static int cmp_icon_dir(const void *A, const void *B)
{
    const IconDir *a = (const IconDir *)A, *b = (const IconDir *)B;
    if (a->size != b->size) {
        if (a->size == 0) return -1;           /* scalable wins: crisp at any size */
        if (b->size == 0) return 1;
        return b->size - a->size;              /* then big before small */
    }
    return 0;
}

static void icon_dirs_build(void)
{
    if (g_icon_dirs_built) return;
    g_icon_dirs_built = true;

    static char *theme = NULL;
    if (!theme) theme = icon_theme_name();

    const char *home = getenv("HOME");
    char *roots[4];
    int nr = 0;
    if (home) {
        roots[nr++] = fmt_alloc("%s/.local/share/icons", home);
        roots[nr++] = fmt_alloc("%s/.icons", home);
    }
    roots[nr++] = xstrdup("/usr/share/icons");
    roots[nr++] = xstrdup("/usr/local/share/icons");

    const char *themes[] = { theme, "Adwaita", "breeze", "Papirus", "hicolor" };
    const char *cats[]   = { "apps", "applications" };

    for (int r = 0; r < nr; ++r) {
        for (size_t t = 0; t < SDL_arraysize(themes); ++t) {
            if (!themes[t] || !*themes[t]) continue;
            char *tdir = fmt_alloc("%s/%s", roots[r], themes[t]);
            DIR *d = opendir(tdir);
            if (!d) { free(tdir); continue; }

            struct dirent *de;
            while ((de = readdir(d))) {
                const char *n = de->d_name;
                if (n[0] == '.') continue;
                int size = -1;
                if (strcmp(n, "scalable") == 0) size = 0;
                else if (isdigit((unsigned char)n[0])) size = atoi(n);   /* 48x48 */
                if (size < 0) continue;
                if (strstr(n, "@2x")) continue;                          /* skip hidpi */
                for (size_t c = 0; c < SDL_arraysize(cats); ++c) {
                    char *p = fmt_alloc("%s/%s/%s", tdir, n, cats[c]);
                    icon_dir_add(p, size);
                    free(p);
                }
            }
            closedir(d);
            free(tdir);
        }
    }
    for (int i = 0; i < nr; ++i) free(roots[i]);

    qsort(g_icon_dirs, (size_t)g_icon_dir_count, sizeof(IconDir), cmp_icon_dir);
    icon_dir_add("/usr/share/pixmaps", 0);
}

static char *icon_lookup(const char *name, int want_px)
{
    (void)want_px;
    if (!name || !*name) return NULL;
    if (name[0] == '/') return file_readable(name) ? xstrdup(name) : NULL;

    icon_dirs_build();

    const char *exts[] = { ".svg", ".png", ".xpm" };
    bool has_ext = strstr(name, ".png") || strstr(name, ".svg") || strstr(name, ".xpm");

    for (int d = 0; d < g_icon_dir_count; ++d) {
        for (size_t e = 0; e < SDL_arraysize(exts); ++e) {
            char *p = fmt_alloc("%s/%s%s", g_icon_dirs[d].path, name, has_ext ? "" : exts[e]);
            if (file_readable(p)) return p;
            free(p);
            if (has_ext) break;
        }
    }
    return NULL;
}

static SDL_Texture *icon_load_file(const char *path, int want_px)
{
    SDL_Surface *surf = NULL;
    size_t n = strlen(path);
    if (n > 4 && ci_cmp(path + n - 4, ".svg") == 0) {
        SDL_IOStream *io = SDL_IOFromFile(path, "rb");
        if (io) {
            surf = IMG_LoadSizedSVG_IO(io, want_px, want_px);
            SDL_CloseIO(io);
        }
    }
    if (!surf) surf = IMG_Load(path);
    if (!surf) return NULL;

    SDL_Texture *t = SDL_CreateTextureFromSurface(REN, surf);
    SDL_DestroySurface(surf);
    if (t) {
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    }
    return t;
}

/* a rounded tile with the first letter, used when no icon is found */
static SDL_Texture *icon_make_letter(const char *app_id, int want_px)
{
    const char *nm = short_app_name(app_id);
    char letter[8] = {0};
    /* copy one UTF-8 code point, upper-cased if ASCII */
    unsigned char c0 = (unsigned char)nm[0];
    int clen = (c0 < 0x80) ? 1 : (c0 < 0xe0) ? 2 : (c0 < 0xf0) ? 3 : 4;
    memcpy(letter, nm, (size_t)clen);
    if (clen == 1) letter[0] = (char)toupper(c0);

    SDL_Texture *t = SDL_CreateTexture(REN, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_TARGET, want_px, want_px);
    if (!t) return NULL;
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);

    SDL_Texture *prev = SDL_GetRenderTarget(REN);
    SDL_SetRenderTarget(REN, t);
    SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_NONE);
    set_col((SDL_FColor){0, 0, 0, 0});
    SDL_RenderClear(REN);
    SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_BLEND);

    SDL_FRect box = { 0, 0, (float)want_px, (float)want_px };
    fill_round_rect(box, want_px * 0.24f, mix(C.hl, C.tile, 0.55f));
    stroke_round_rect(box, want_px * 0.24f, SDL_max(1.0f, want_px * 0.05f),
                      with_alpha(C.hl, 0.85f));

    Tex g = text_make(F_LABEL, letter);
    if (g.t) {
        float s = SDL_min((float)want_px * 0.62f / SDL_max(1, g.h), 1.6f);
        SDL_FRect dst = { (want_px - g.w * s) * 0.5f, (want_px - g.h * s) * 0.5f,
                          g.w * s, g.h * s };
        SDL_SetTextureColorModFloat(g.t, C.text.r, C.text.g, C.text.b);
        SDL_RenderTexture(REN, g.t, NULL, &dst);
        tex_free(&g);
    }

    SDL_SetRenderTarget(REN, prev);
    return t;
}

typedef struct { char key[192]; SDL_Texture *tex; } IconCacheEntry;
static IconCacheEntry g_icons[256];
static int g_icon_count = 0;

static SDL_Texture *icon_for_app(const char *app_id, int want_px)
{
    if (!C.icons) return NULL;
    const char *key = (app_id && *app_id) ? app_id : "unknown";

    for (int i = 0; i < g_icon_count; ++i)
        if (strcmp(g_icons[i].key, key) == 0) return g_icons[i].tex;

    SDL_Texture *tex = NULL;
    const DesktopEntry *de = desktop_find(key);
    if (de && de->icon) {
        char *path = icon_lookup(de->icon, want_px);
        if (path) { tex = icon_load_file(path, want_px); free(path); }
    }
    if (!tex) {                                   /* app_id often *is* the icon */
        char *path = icon_lookup(key, want_px);
        if (!path) path = icon_lookup(short_app_name(key), want_px);
        if (path) { tex = icon_load_file(path, want_px); free(path); }
    }
    if (!tex) tex = icon_make_letter(key, want_px);

    if (g_icon_count < (int)SDL_arraysize(g_icons)) {
        snprintf(g_icons[g_icon_count].key, sizeof(g_icons[0].key), "%s", key);
        g_icons[g_icon_count].tex = tex;
        g_icon_count++;
    }
    return tex;
}

static void icons_free(void)
{
    for (int i = 0; i < g_icon_count; ++i)
        if (g_icons[i].tex) SDL_DestroyTexture(g_icons[i].tex);
    g_icon_count = 0;

    for (int i = 0; i < g_desktop_count; ++i) {
        free(g_desktops[i].id);
        free(g_desktops[i].path);
        free(g_desktops[i].icon);
        free(g_desktops[i].wmclass);
        free(g_desktops[i].name);
    }
    for (int i = 0; i < g_icon_dir_count; ++i) free(g_icon_dirs[i].path);
    free(g_icon_dirs);
    g_icon_dirs = NULL;
    g_icon_dir_count = g_icon_dir_cap = 0;
    g_icon_dirs_built = false;

    free(g_desktops);
    g_desktops = NULL;
    g_desktop_count = 0;
    g_desktop_indexed = false;
}

/* ----------------------------------------------------------------- model */

typedef struct {
    int   con_id;
    int   pid;
    char  app_id[128];      /* what sway reports                            */
    char  app_key[128];     /* what we look the icon and the name up with   */
    char  title[512];
    char  ws_name[64];
    char  output[64];

    int   x, y, w, h;          /* absolute geometry, as sway reports it */
    bool  floating, focused, urgent, fullscreen, sticky;
    bool  marked;              /* multi-selection */
    bool  match;               /* passes the current filter */
    bool  visible;             /* the one on top of its tabbed container */
    SDL_FRect tab;             /* its slot in the container's tab strip   */
    bool  has_tab;

    int   ws;                  /* index into WSS */
    SDL_FRect card;            /* where it is drawn (render pixels) */
    SDL_FRect hit;             /* what the mouse acts on: the whole card, or
                                * just the name plate of a floating window   */
    int   group;               /* head index of a tabbed/stacked group, or -1 */

    int   lay_mode;            /* how the card contents are arranged */
    float lay_icon;            /* icon edge length on this card */
    bool  lay_sub;             /* is there room for the title */

    SDL_Texture *icon;         /* borrowed from the icon cache */
    Tex   label, subtitle;
} Win;

typedef struct {
    char  name[64];      /* what sway calls it: "3" or "3:code"           */
    char  label[64];     /* just the name part, "" when it is only a number */
    char  output[64];
    double usage;        /* seconds spent here, from the tracker            */
    int   num;
    bool  focused, visible, urgent;
    int   rx, ry, rw, rh;      /* workspace rect */

    int   first, count;        /* range in WINS */
    int   sel;                 /* selected window, relative to first; -1 = the
                                * workspace itself is selected             */
    int   top[64];             /* top level containers, in order           */
    int   ntop;

    SDL_FRect tile, screen;    /* layout (render pixels) */
    SDL_FRect title_box;       /* the clickable name area in the header */
    SDL_FRect tile_from;       /* where this tile animates from */
    Tex   badge, sub, title;
} Ws;

static Win WINS[MAX_WINDOWS];
static int NWIN = 0;
static Ws  WSS[MAX_WORKSPACES];
static int NWS = 0;

static char FOCUSED_OUTPUT[64] = {0};   /* output sway currently focuses */
static char CUR_OUTPUT[64]     = {0};   /* output the tree walk is inside */

static void model_free(void)
{
    for (int i = 0; i < NWIN; ++i) { tex_free(&WINS[i].label); tex_free(&WINS[i].subtitle); }
    for (int i = 0; i < NWS; ++i)  {
        tex_free(&WSS[i].badge);
        tex_free(&WSS[i].sub);
        tex_free(&WSS[i].title);
    }
    NWIN = NWS = 0;
}

static bool node_is_view(const JV *n)
{
    if (jget(n, "app_id") && jget(n, "app_id")->type == J_STR) return true;
    const JV *wp = jget(n, "window_properties");
    return wp && jget(wp, "class") && jget(wp, "class")->type == J_STR;
}

static void collect_views(const JV *node, Ws *ws, bool floating)
{
    if (!node || node->type != J_OBJ) return;

    if (node_is_view(node) && NWIN < MAX_WINDOWS) {
        Win *w = &WINS[NWIN];
        memset(w, 0, sizeof(*w));

        const char *app = jstr(node, "app_id", NULL);
        if (!app) {
            const JV *wp = jget(node, "window_properties");
            app = wp ? jstr(wp, "class", NULL) : NULL;
        }
        snprintf(w->app_id, sizeof(w->app_id), "%s", app ? app : "unknown");
        memcpy(w->app_key, w->app_id, sizeof(w->app_key));
        snprintf(w->title,  sizeof(w->title),  "%s", jstr(node, "name", ""));
        snprintf(w->ws_name, sizeof(w->ws_name), "%s", ws->name);
        snprintf(w->output,  sizeof(w->output),  "%s", ws->output);

        w->con_id     = jint(node, "id", 0);
        w->pid        = jint(node, "pid", 0);
        w->focused    = jbool(node, "focused", false);
        w->visible    = jbool(node, "visible", true);
        w->urgent     = jbool(node, "urgent", false);
        w->sticky     = jbool(node, "sticky", false);
        w->floating   = floating;
        w->fullscreen = jint(node, "fullscreen_mode", 0) != 0;
        w->match      = true;
        w->ws         = (int)(ws - WSS);

        const JV *r = jget(node, "rect");
        w->x = jint(r, "x", ws->rx);
        w->y = jint(r, "y", ws->ry);
        w->w = jint(r, "width",  100);
        w->h = jint(r, "height", 100);
        if (w->w <= 0) w->w = 100;
        if (w->h <= 0) w->h = 100;

        NWIN++;
        ws->count++;
        return;                                  /* views have no children */
    }

    const JV *nodes = jget(node, "nodes");
    if (nodes && nodes->type == J_ARR)
        for (int i = 0; i < nodes->count; ++i) collect_views(nodes->items[i], ws, floating);

    const JV *fl = jget(node, "floating_nodes");
    if (fl && fl->type == J_ARR)
        for (int i = 0; i < fl->count; ++i) collect_views(fl->items[i], ws, true);
}

static int cmp_win_pos(const void *A, const void *B)
{
    const Win *a = (const Win *)A, *b = (const Win *)B;
    bool af = a->floating || a->fullscreen;      /* both are drawn on top */
    bool bf = b->floating || b->fullscreen;
    if (af != bf) return af ? 1 : -1;
    if (a->y != b->y) return a->y < b->y ? -1 : 1;
    if (a->x != b->x) return a->x < b->x ? -1 : 1;
    return a->con_id - b->con_id;
}

static void walk_outputs(const JV *node)
{
    if (!node || node->type != J_OBJ) return;
    const char *type = jstr(node, "type", "");

    if (strcmp(type, "workspace") == 0) {
        const char *name = jstr(node, "name", "?");
        if (strcmp(name, "__i3_scratchpad") == 0) return;
        if (NWS >= MAX_WORKSPACES) return;

        Ws *ws = &WSS[NWS];
        memset(ws, 0, sizeof(*ws));
        snprintf(ws->name, sizeof(ws->name), "%s", name);
        snprintf(ws->output, sizeof(ws->output), "%s", CUR_OUTPUT);
        ws->num     = jint(node, "num", str_all_digits(name) ? atoi(name) : -1);

        const char *colon = strchr(name, ':');       /* "3:code" -> "code" */
        if (colon) snprintf(ws->label, sizeof(ws->label), "%s", colon + 1);
        else if (!str_all_digits(name)) snprintf(ws->label, sizeof(ws->label), "%s", name);
        ws->focused = jbool(node, "focused", false);
        ws->urgent  = jbool(node, "urgent", false);

        const JV *r = jget(node, "rect");
        ws->rx = jint(r, "x", 0);
        ws->ry = jint(r, "y", 0);
        ws->rw = jint(r, "width",  1920);
        ws->rh = jint(r, "height", 1080);

        ws->first = NWIN;
        ws->count = 0;
        ws->sel   = -1;
        ws->ntop  = 0;
        NWS++;

        /* remember the direct children: moving those keeps their layout */
        const JV *kids[2] = { jget(node, "nodes"), jget(node, "floating_nodes") };
        for (int a = 0; a < 2; ++a)
            if (kids[a] && kids[a]->type == J_ARR)
                for (int b = 0; b < kids[a]->count && ws->ntop < 64; ++b) {
                    int id = jint(kids[a]->items[b], "id", 0);
                    if (id) ws->top[ws->ntop++] = id;
                }

        collect_views(node, ws, false);

        if (ws->count > 1)
            qsort(&WINS[ws->first], (size_t)ws->count, sizeof(Win), cmp_win_pos);
        for (int i = 0; i < ws->count; ++i) WINS[ws->first + i].ws = (int)(ws - WSS);
        return;
    }

    if (strcmp(type, "output") == 0) {
        const char *name = jstr(node, "name", "");
        if (strcmp(name, "__i3") == 0) return;
        if (!C.all_outputs && FOCUSED_OUTPUT[0] && strcmp(name, FOCUSED_OUTPUT) != 0) return;
        snprintf(CUR_OUTPUT, sizeof(CUR_OUTPUT), "%s", name);
    }

    const JV *nodes = jget(node, "nodes");
    if (nodes && nodes->type == J_ARR)
        for (int i = 0; i < nodes->count; ++i) walk_outputs(nodes->items[i]);
}

static int cmp_ws(const void *A, const void *B)
{
    const Ws *a = (const Ws *)A, *b = (const Ws *)B;
    int o = strcmp(a->output, b->output);
    if (o) return o;
    if (a->num >= 0 && b->num >= 0 && a->num != b->num) return a->num < b->num ? -1 : 1;
    if ((a->num >= 0) != (b->num >= 0)) return a->num >= 0 ? -1 : 1;
    return strcmp(a->name, b->name);
}

/* Fetch workspaces + tree and rebuild the model. Returns false on failure. */
static bool model_reload(void)
{
    model_free();

    char focused[64] = {0};
    JV *wsr = sway_query(IPC_GET_WORKSPACES);
    if (wsr && wsr->type == J_ARR) {
        for (int i = 0; i < wsr->count; ++i)
            if (jbool(wsr->items[i], "focused", false))
                snprintf(focused, sizeof(focused), "%s", jstr(wsr->items[i], "output", ""));
    }

    /* sway reports no focused workspace for a moment after a rename; keeping
     * the previous output beats falling back to "every output" */
    if (focused[0]) snprintf(FOCUSED_OUTPUT, sizeof(FOCUSED_OUTPUT), "%s", focused);

    JV *tree = sway_query(IPC_GET_TREE);
    if (!tree) { jfree(wsr); return false; }

    CUR_OUTPUT[0] = 0;
    walk_outputs(tree);

    /* per-output names + visible flags from get_workspaces */
    if (wsr && wsr->type == J_ARR) {
        for (int i = 0; i < NWS; ++i) {
            for (int j = 0; j < wsr->count; ++j) {
                const JV *o = wsr->items[j];
                if (strcmp(jstr(o, "name", ""), WSS[i].name) != 0) continue;
                snprintf(WSS[i].output, sizeof(WSS[i].output), "%s", jstr(o, "output", ""));
                WSS[i].visible = jbool(o, "visible", false);
                WSS[i].focused = jbool(o, "focused", WSS[i].focused);
                WSS[i].urgent  = jbool(o, "urgent",  WSS[i].urgent);
                WSS[i].num     = jint(o, "num", WSS[i].num);
                break;
            }
        }
    }

    jfree(wsr);
    jfree(tree);

    /* drop empty workspaces if the user does not want them */
    if (!C.show_empty) {
        int k = 0;
        for (int i = 0; i < NWS; ++i)
            if (WSS[i].count > 0) WSS[k++] = WSS[i];
        NWS = k;
    }

    /* sorting workspaces moves them around; fix up window -> workspace links */
    if (NWS > 1) qsort(WSS, (size_t)NWS, sizeof(Ws), cmp_ws);
    for (int i = 0; i < NWS; ++i)
        for (int j = 0; j < WSS[i].count; ++j) WINS[WSS[i].first + j].ws = i;

    usage_load();
    double pending = usage_pending();
    bool   changed = false;

    for (int i = 0; i < NUSAGE; ) {                  /* forget what is gone */
        bool alive = false;
        for (int j = 0; j < NWS; ++j)
            if (strcmp(WSS[j].name, USAGE[i].name) == 0 && WSS[j].count > 0) alive = true;
        if (alive) { ++i; continue; }
        USAGE[i] = USAGE[--NUSAGE];                  /* an empty workspace resets */
        changed = true;
    }
    if (changed) usage_save();

    USAGE_MAX = 0.0;
    for (int i = 0; i < NWS; ++i) {
        Usage *u = usage_find(WSS[i].name, false);
        WSS[i].usage = u ? u->secs : 0.0;
        if (strcmp(WSS[i].name, USAGE_CUR) == 0) WSS[i].usage += pending;
        if (WSS[i].count == 0) WSS[i].usage = 0.0;   /* empty means start over */
        if (WSS[i].usage > USAGE_MAX) USAGE_MAX = WSS[i].usage;
    }

    for (int i = 0; i < NWIN; ++i) {
        Win *w = &WINS[i];
        if (app_id_is_useless(w->app_id)) {          /* ask the process instead */
            char nm[128];
            proc_app_name(w->pid, nm, sizeof(nm));
            if (nm[0]) snprintf(w->app_key, sizeof(w->app_key), "%s", nm);
        }
        w->icon = icon_for_app(w->app_key, px((float)C.icon_px * 1.6f));
    }

    return true;
}

/* ---------------------------------------------------------------- layout */

typedef struct { int ws; int num; SDL_FRect tile, from; } Slot;  /* ws < 0 = ghost */
static Slot SLOTS[MAX_WORKSPACES + 16];
static int  NSLOTS;
static bool drag_ws_mode;         /* a workspace is being dragged right now  */
static int  ghost_lo = -1, ghost_hi = -1;   /* free numbers currently offered */

static Slot   DYING[16];          /* ghosts on their way out                 */
static int    NDYING;
static Uint64 dying_start;
static bool   laid_out_once;

static Uint64 anim_start;         /* tiles glide when the grid changes       */

static bool anim_running(void)
{
    if (C.anim_ms <= 0.0f) return false;
    if (anim_start && (float)(SDL_GetTicks() - anim_start) < C.anim_ms) return true;
    return NDYING > 0 && (float)(SDL_GetTicks() - dying_start) < C.anim_ms;
}

static float phase_since(Uint64 start)
{
    if (!start || C.anim_ms <= 0.0f) return 1.0f;
    float t = (float)(SDL_GetTicks() - start) / C.anim_ms;
    if (t >= 1.0f) return 1.0f;
    if (t <= 0.0f) return 0.0f;
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

/* 0..1, ease-out so the movement settles instead of stopping dead */
static float anim_phase(void)
{
    if (!anim_start || C.anim_ms <= 0.0f) return 1.0f;
    float t = (float)(SDL_GetTicks() - anim_start) / C.anim_ms;
    if (t >= 1.0f) return 1.0f;
    if (t <= 0.0f) return 0.0f;
    float inv = 1.0f - t;
    return 1.0f - inv * inv * inv;
}

static SDL_FRect rect_lerp(SDL_FRect a, SDL_FRect b, float t)
{
    return (SDL_FRect){ a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t,
                        a.w + (b.w - a.w) * t, a.h + (b.h - a.h) * t };
}

static SDL_FRect rect_shrink(SDL_FRect r, float f)
{
    float w = r.w * f, h = r.h * f;
    return (SDL_FRect){ r.x + (r.w - w) * 0.5f, r.y + (r.h - h) * 0.5f, w, h };
}

static int   GRID_COLS = 1;       /* columns the tiles are arranged in       */
static int   RW, RH;              /* size of the render target, in pixels   */
static float HEADER_H, FOOTER_H;

/* position strings: none | top-left | top-center | top-right |
 *                          bottom-left | bottom-center | bottom-right       */
static bool pos_is_none(const char *p)   { return !p || !*p || ci_cmp(p, "none") == 0 ||
                                                  ci_cmp(p, "off") == 0 || ci_cmp(p, "hidden") == 0; }
static bool pos_is_top(const char *p)    { return !pos_is_none(p) && strncmp(p, "top", 3) == 0; }
static bool pos_is_bottom(const char *p) { return !pos_is_none(p) && !pos_is_top(p); }

/* x of a text of width w for the horizontal half of the position string */
static float pos_x(const char *p, float w, float margin)
{
    const char *dash = strchr(p ? p : "", '-');
    const char *h = dash ? dash + 1 : "right";
    if (ci_cmp(h, "left") == 0)   return margin;
    if (ci_cmp(h, "center") == 0 || ci_cmp(h, "centre") == 0)
        return ((float)RW - w) * 0.5f;
    return (float)RW - margin - w;
}

static int  sel_ws = 0;
static bool sel_active = true;   /* false: no tile and no window highlighted */
static int  hov_ws = -1, hov_win = -1;
static char query[128];
static int  qlen = 0;
static bool filtering = false;   /* "/" — hides everything that does not match */
static bool searching = false;   /* "s" — highlights what matches, hides nothing */

static bool query_active(void) { return filtering || searching; }

/* The key press that opens a mode is followed by its own text input event —
 * "s" would otherwise be the first character of the search. */
static bool swallow_next_text = false;
static bool confirm_kill = false;

/* renaming a workspace in place, from a click on its title */
static bool editing = false;
static int  edit_ws = -1;
static char edit_buf[64];
static int  edit_len = 0;

/* NULL means "the workspace itself is selected, not a window in it" */
static Win *ws_sel_win(Ws *w)
{
    if (w->count <= 0 || w->sel < 0) return NULL;
    if (w->sel >= w->count) w->sel = w->count - 1;
    return &WINS[w->first + w->sel];
}

static bool rect_overlap_same(const Win *a, const Win *b)
{
    int tol = 6;
    return SDL_abs(a->x - b->x) <= tol && SDL_abs(a->y - b->y) <= tol &&
           SDL_abs(a->w - b->w) <= tol && SDL_abs(a->h - b->h) <= tol;
}

static void layout_cards(Ws *ws)
{
    if (ws->count <= 0) return;

    /* the windows live in an area that keeps an equal border on all four
     * sides of the mini screen, so nothing ever touches the tile frame */
    float sp = C.screen_pad * SC;
    SDL_FRect area = { ws->screen.x + sp, ws->screen.y + sp,
                       ws->screen.w - 2.0f * sp, ws->screen.h - 2.0f * sp };
    if (area.w < 8.0f || area.h < 8.0f) area = ws->screen;

    float sx = area.w / (float)SDL_max(1, ws->rw);
    float sy = area.h / (float)SDL_max(1, ws->rh);
    float inset = C.win_gap * SC * 0.5f;
    float min_w = 74.0f * SC, min_h = 52.0f * SC;
    min_w = SDL_min(min_w, area.w);
    min_h = SDL_min(min_h, area.h);

    for (int i = 0; i < ws->count; ++i) {
        Win *w = &WINS[ws->first + i];
        SDL_FRect r = {
            area.x + (float)(w->x - ws->rx) * sx + inset,
            area.y + (float)(w->y - ws->ry) * sy + inset,
            (float)w->w * sx - 2.0f * inset,
            (float)w->h * sy - 2.0f * inset
        };
        if (r.w < min_w) { r.x -= (min_w - r.w) * 0.5f; r.w = min_w; }
        if (r.h < min_h) { r.y -= (min_h - r.h) * 0.5f; r.h = min_h; }
        w->card = r;
    }

    /* Tabbed / stacked containers report identical geometry for every child.
     * Split such a group into side by side slices so all of them stay
     * visible and clickable. */
    bool done[MAX_WINDOWS];
    memset(done, 0, sizeof(bool) * (size_t)ws->count);
    for (int i = 0; i < ws->count; ++i) {
        WINS[ws->first + i].group = -1;
        WINS[ws->first + i].has_tab = false;
    }

    for (int i = 0; i < ws->count; ++i) {
        if (done[i]) continue;
        int group[64];
        int n = 0;
        group[n++] = i;
        for (int j = i + 1; j < ws->count && n < 64; ++j) {
            if (done[j]) continue;
            if (WINS[ws->first + i].floating != WINS[ws->first + j].floating) continue;
            if (rect_overlap_same(&WINS[ws->first + i], &WINS[ws->first + j])) {
                group[n++] = j;
                done[j] = true;
            }
        }
        done[i] = true;
        if (n < 2) continue;

        float ge = 3.0f * SC;                    /* room for the group outline */
        SDL_FRect base = WINS[ws->first + i].card;
        base.x += ge; base.y += ge; base.w -= 2.0f * ge; base.h -= 2.0f * ge;
        if (base.w < 4.0f || base.h < 4.0f) base = WINS[ws->first + i].card;
        for (int k = 0; k < n; ++k) WINS[ws->first + group[k]].group = i;

        /* Which child is on top: sway marks it visible, otherwise the focused
         * one, otherwise the first. */
        int active = 0;
        for (int k = 0; k < n; ++k) {
            Win *cw = &WINS[ws->first + group[k]];
            if (cw->focused) { active = k; break; }
            if (cw->visible) active = k;
        }

        float strip = SDL_min(32.0f * SC, base.h * 0.36f);
        bool  tabs_fit = strip >= 16.0f * SC && base.w / (float)n >= 26.0f * SC;

        if (tabs_fit) {
            /* a tab per window across the top, the active one showing its
             * contents underneath — the same shape sway draws */
            float tw = base.w / (float)n;
            for (int k = 0; k < n; ++k) {
                Win *cw = &WINS[ws->first + group[k]];
                cw->tab = (SDL_FRect){ base.x + (float)k * tw + inset * 0.5f, base.y,
                                       tw - inset, strip };
                cw->has_tab = true;
                cw->card = (k == active)
                    ? (SDL_FRect){ base.x, base.y + strip + inset * 0.5f,
                                   base.w, base.h - strip - inset * 0.5f }
                    : cw->tab;
            }
        } else {
            bool horiz = base.w >= base.h;          /* too small: plain slices */
            for (int k = 0; k < n; ++k) {
                SDL_FRect r = base;
                if (horiz) {
                    r.w = (base.w - (float)(n - 1) * inset * 2.0f) / (float)n;
                    r.x = base.x + (float)k * (r.w + inset * 2.0f);
                } else {
                    r.h = (base.h - (float)(n - 1) * inset * 2.0f) / (float)n;
                    r.y = base.y + (float)k * (r.h + inset * 2.0f);
                }
                WINS[ws->first + group[k]].card = r;
                WINS[ws->first + group[k]].has_tab = false;
            }
        }
    }

    /* keep everything inside the tile */
    for (int i = 0; i < ws->count; ++i) {
        SDL_FRect *r = &WINS[ws->first + i].card;
        if (r->w > area.w) r->w = area.w;
        if (r->h > area.h) r->h = area.h;
        if (r->x < area.x) r->x = area.x;
        if (r->y < area.y) r->y = area.y;
        if (r->x + r->w > area.x + area.w) r->x = area.x + area.w - r->w;
        if (r->y + r->h > area.y + area.h) r->y = area.y + area.h - r->h;
    }
}

/* shallow cards (a tab, say) cannot spare 7px top and bottom */
static float card_pad(const Win *w)
{
    return SDL_min(7.0f * SC, w->card.h * 0.13f);
}

enum { CL_ICON, CL_STACK, CL_ROW, CL_TEXT };

/* Decide how much fits on a card: icon + app name + title, icon + name,
 * a row (icon left, text right) on wide flat cards, or just an icon. */
static void compute_card_layout(Win *w)
{
    float pad    = card_pad(w);
    float availw = w->card.w - 2.0f * pad;
    float availh = w->card.h - 2.0f * pad;
    float lab_h  = (float)TTF_GetFontHeight(F_LABEL);
    float sub_h  = (float)TTF_GetFontHeight(F_TITLE);
    float g1 = 4.0f * SC, g2 = 1.0f * SC;
    float icon_max = C.icons ? SDL_min((float)C.icon_px * SC,
                                       SDL_min(availw * 0.72f, availh * 0.72f)) : 0.0f;
    float icon_min = 16.0f * SC;

    w->lay_mode = CL_ICON;
    w->lay_icon = icon_max;
    w->lay_sub  = false;

    if (availw <= 4.0f || availh <= 4.0f) return;

    if (availh >= icon_max + g1 + lab_h + g2 + sub_h) {          /* everything */
        w->lay_mode = CL_STACK;
        w->lay_icon = icon_max;
        w->lay_sub  = true;
        return;
    }
    if (availw >= availh * 1.15f && availw >= lab_h * 5.0f &&
        availh >= lab_h + g2 + sub_h) {                          /* wide and flat */
        w->lay_mode = CL_ROW;
        w->lay_icon = C.icons ? SDL_min(icon_max, SDL_min(availh * 0.9f, availw * 0.3f)) : 0.0f;
        if (w->lay_icon < icon_min) w->lay_icon = 0.0f;
        w->lay_sub  = true;
        return;
    }
    if (availh - (lab_h + g1 + g2 + sub_h) >= icon_min) {        /* shrink the icon */
        w->lay_mode = CL_STACK;
        w->lay_icon = availh - (lab_h + g1 + g2 + sub_h);
        w->lay_sub  = true;
        return;
    }
    if (availh >= icon_min + g1 + lab_h) {                       /* icon + name */
        w->lay_mode = CL_STACK;
        w->lay_icon = SDL_min(icon_max, availh - lab_h - g1);
        w->lay_sub  = false;
        return;
    }
    if (availh >= lab_h) {                                       /* name only */
        w->lay_mode = CL_TEXT;
        w->lay_icon = 0.0f;
        w->lay_sub  = false;
        return;
    }
    w->lay_icon = SDL_min(availw, availh);                       /* icon only */
}

/* Floating and fullscreen windows are drawn as see-through frames with a name
 * plate at the top. The plate is the only part that takes the mouse, so the
 * windows underneath stay clickable. */
static bool card_is_overlay(const Win *w) { return w->floating || w->fullscreen; }

static bool overlay_plate(const Win *w, SDL_FRect r, SDL_FRect *plate, float *icon_w)
{
    if (!w->label.t) return false;

    float ip = 6.0f * SC;
    float ih = SDL_min(22.0f * SC, (float)C.icon_px * SC * 0.6f);
    bool  has_icon = C.icons && w->icon && w->lay_icon >= 10.0f * SC && ih >= 10.0f * SC;
    float iw = has_icon ? ih + 5.0f * SC : 0.0f;
    float pw = iw + (float)w->label.w + 2.0f * ip;
    float ph = SDL_max((float)w->label.h, ih) + ip;

    if (pw > r.w - 4.0f * SC) {                 /* tight: drop the icon first */
        iw = 0.0f;
        pw = (float)w->label.w + 2.0f * ip;
        ph = (float)w->label.h + ip;
    }
    if (pw > r.w - 4.0f * SC || ph > r.h * 0.6f) return false;

    *plate = (SDL_FRect){ r.x + (r.w - pw) * 0.5f, r.y + 7.0f * SC, pw, ph };
    if (icon_w) *icon_w = iw;
    return true;
}

static float card_text_width(const Win *w)
{
    float pad = card_pad(w);
    float availw = w->card.w - 2.0f * pad;
    if (w->lay_mode == CL_ROW && w->lay_icon > 0.0f) availw -= w->lay_icon + 6.0f * SC;
    return availw;
}

/* The name printed in bold on a card. Sway's app_id is used when it says
 * something; otherwise the process behind the window has already been asked
 * (app_key), and failing that the tail of the title is the best guess:
 * "document.txt - Some Editor". */
static const char *card_label_name(const Win *w)
{
    /* sway knew the app: that is the best answer */
    if (!app_id_is_useless(w->app_id)) return app_display_name(w->app_id);

    /* the process behind it matched a .desktop file: also solid */
    const DesktopEntry *de = desktop_find(w->app_key);
    if (de && de->name && *de->name) return de->name;

    /* otherwise the tail of the title names the app more often than a binary
     * does: "document.txt - Some Editor", or plainly "Claude" */
    if (w->title[0]) {
        const char *tail = NULL;
        for (const char *p = strstr(w->title, " - "); p; p = strstr(p + 3, " - ")) tail = p + 3;
        for (const char *p = strstr(w->title, " \xe2\x80\x94 "); p; p = strstr(p + 5, " \xe2\x80\x94 "))
            tail = p + 5;
        if (tail && *tail) return tail;

        /* no separator at all: a short title is usually the app name itself
         * ("Claude"), a long one is a document and the binary is the better
         * answer */
        if (strlen(w->title) <= 24) return w->title;
    }

    /* last resort: the executable name, without its extension */
    if (!app_id_is_useless(w->app_key)) {
        static char buf[128];
        snprintf(buf, sizeof(buf), "%s", w->app_key);
        char *dot = strrchr(buf, '.');
        if (dot && (ci_cmp(dot, ".py") == 0 || ci_cmp(dot, ".sh") == 0 ||
                    ci_cmp(dot, ".js") == 0 || ci_cmp(dot, ".bin") == 0 ||
                    ci_cmp(dot, ".exe") == 0)) *dot = 0;
        if (buf[0]) return buf;
    }
    return w->title[0] ? w->title : "unknown";
}

static void build_texts(void)
{
    for (int i = 0; i < NWS; ++i) {
        Ws *ws = &WSS[i];
        tex_free(&ws->badge);
        tex_free(&ws->sub);

        char num[16];
        if (ws->num >= 0) snprintf(num, sizeof(num), "%d", ws->num);
        else              snprintf(num, sizeof(num), "%s", ws->name);
        ws->badge = text_make(F_BADGE, num);
        ws->title = ws->label[0] ? text_make(F_LABEL, ws->label) : (Tex){0};

        char sub[192];
        if (ws->count == 0)          snprintf(sub, sizeof(sub), "empty");
        else if (ws->count == 1)     snprintf(sub, sizeof(sub), "1 window");
        else                         snprintf(sub, sizeof(sub), "%d windows", ws->count);

        if (C.usage_dots && ws->usage >= 60.0) {   /* say what the dots mean */
            int mins = (int)(ws->usage / 60.0);
            char tmp[224];
            if (mins < 60) snprintf(tmp, sizeof(tmp), "%s \xc2\xb7 %dm", sub, mins);
            else snprintf(tmp, sizeof(tmp), "%s \xc2\xb7 %dh %dm", sub, mins / 60, mins % 60);
            snprintf(sub, sizeof(sub), "%.191s", tmp);
        }
        if (C.all_outputs && ws->output[0]) {
            char tmp[256];
            snprintf(tmp, sizeof(tmp), "%s \xc2\xb7 %s", ws->output, sub);
            snprintf(sub, sizeof(sub), "%.191s", tmp);
        }
        ws->sub = text_make(F_HINT, sub);
    }

    for (int i = 0; i < NWIN; ++i) {
        Win *w = &WINS[i];
        tex_free(&w->label);
        tex_free(&w->subtitle);
        w->hit = w->card;                    /* the whole card, unless a plate
                                              * takes over below */

        compute_card_layout(w);
        int maxw = (int)card_text_width(w);
        if (maxw < 12 || w->lay_mode == CL_ICON) continue;

        const char *name = card_label_name(w);
        w->label = text_make_fit(F_LABEL, name, maxw);
        if (w->lay_sub && strcmp(name, w->title) != 0)
            w->subtitle = text_make_fit(F_TITLE, w->title, maxw);

        SDL_FRect plate;
        if (card_is_overlay(w) && overlay_plate(w, w->card, &plate, NULL))
            w->hit = plate;
    }
}

static void layout(void)
{
    float m   = C.margin * SC;
    float gap = C.gap * SC;

    bool head_on  = C.show_header && !pos_is_none(C.header_pos);
    bool hints_on = C.show_hints  && !pos_is_none(C.hints_pos);
    float line = (float)C.hint_px * SC * C.ui_scale * 2.4f;

    bool top    = (head_on && pos_is_top(C.header_pos))    || (hints_on && pos_is_top(C.hints_pos));
    bool bottom = (head_on && pos_is_bottom(C.header_pos)) || (hints_on && pos_is_bottom(C.hints_pos));

    HEADER_H = top ? line : m;
    FOOTER_H = bottom ? line : 0.0f;

    float ax = m;
    float ay = HEADER_H;
    float aw = (float)RW - 2.0f * m;
    float ah = (float)RH - HEADER_H - FOOTER_H - m;
    if (aw < 40.0f || ah < 40.0f || NWS == 0) return;

    /* aspect of the screen we are mirroring */
    float aspect = 16.0f / 9.0f;
    for (int i = 0; i < NWS; ++i)
        if (WSS[i].rw > 0 && WSS[i].rh > 0) { aspect = (float)WSS[i].rw / (float)WSS[i].rh; break; }

    int best_rows = 1, best_cols = NWS;
    float best_area = -1.0f;

    if (C.rows > 0 || C.cols > 0) {
        best_rows = C.rows > 0 ? C.rows : (NWS + C.cols - 1) / C.cols;
        best_cols = C.cols > 0 ? C.cols : (NWS + C.rows - 1) / C.rows;
        if (best_rows * best_cols < NWS) best_cols = (NWS + best_rows - 1) / best_rows;
    } else {
        for (int rows = 1; rows <= NWS; ++rows) {
            int cols = (NWS + rows - 1) / rows;
            float tw = (aw - gap * (float)(cols - 1)) / (float)cols;
            float th = (ah - gap * (float)(rows - 1)) / (float)rows;
            if (tw <= 0.0f || th <= 0.0f) continue;
            /* tiles keep the screen aspect (plus room for the tile header) */
            float ch = tw / aspect;
            if (ch > th) tw = th * aspect; else th = ch;
            float area = tw * th;
            if (area > best_area) { best_area = area; best_rows = rows; best_cols = cols; }
        }
    }

    /* While a workspace is dragged every free number 0..10 becomes a small
     * ghost slot, so it can be dropped on a workspace that does not exist
     * yet. Otherwise the slots are just the workspaces themselves. */
    /* Slots are the workspaces themselves. Only while something is dragged
     * into a gap do the free numbers of that gap join them, as small ghost
     * tiles to drop on — they are not shown before they mean anything. */
    /* Where everything is *right now*, mid-glide included: a relayout during
     * an animation has to continue from what the eye sees, otherwise the
     * movement snaps. */
    Slot old[MAX_WORKSPACES + 16];
    int  nold = SDL_min(NSLOTS, (int)SDL_arraysize(SLOTS));
    float ph_now = anim_phase();
    for (int i = 0; i < nold; ++i) {
        old[i] = SLOTS[i];
        old[i].tile = rect_lerp(SLOTS[i].from, SLOTS[i].tile, ph_now);
    }

    NSLOTS = 0;
    for (int i = 0; i < NWS && NSLOTS < (int)SDL_arraysize(SLOTS); ++i) {
        if (WSS[i].num < 0) continue;
        SLOTS[NSLOTS].ws  = i;
        SLOTS[NSLOTS].num = WSS[i].num;
        NSLOTS++;
    }
    for (int n = ghost_lo; ghost_lo >= 0 && n <= ghost_hi &&
                           NSLOTS < (int)SDL_arraysize(SLOTS); ++n) {
        bool taken = false;
        for (int i = 0; i < NWS; ++i) if (WSS[i].num == n) { taken = true; break; }
        if (taken) continue;
        SLOTS[NSLOTS].ws  = -1;
        SLOTS[NSLOTS].num = n;
        NSLOTS++;
    }
    for (int i = 0; i + 1 < NSLOTS; ++i)              /* order by number */
        for (int j = i + 1; j < NSLOTS; ++j)
            if (SLOTS[j].num < SLOTS[i].num) {
                Slot t = SLOTS[i]; SLOTS[i] = SLOTS[j]; SLOTS[j] = t;
            }
    for (int i = 0; i < NWS && NSLOTS < (int)SDL_arraysize(SLOTS); ++i)
        if (WSS[i].num < 0) {                         /* named workspaces last */
            SLOTS[NSLOTS].ws  = i;
            SLOTS[NSLOTS].num = -1;
            NSLOTS++;
        }

    int n_tiles = NSLOTS;
    if (C.rows <= 0 && C.cols <= 0 && n_tiles != NWS) {   /* redo the grid search */
        best_area = -1.0f;
        for (int rows = 1; rows <= n_tiles; ++rows) {
            int cols = (n_tiles + rows - 1) / rows;
            float tw = (aw - gap * (float)(cols - 1)) / (float)cols;
            float th = (ah - gap * (float)(rows - 1)) / (float)rows;
            if (tw <= 0.0f || th <= 0.0f) continue;
            float ch = tw / aspect;
            if (ch > th) tw = th * aspect; else th = ch;
            float area = tw * th;
            if (area > best_area) { best_area = area; best_rows = rows; best_cols = cols; }
        }
    }

    int cols = best_cols, rows = best_rows;
    if (cols * rows < n_tiles) cols = (n_tiles + rows - 1) / rows;
    GRID_COLS = SDL_max(1, cols);
    float tw = (aw - gap * (float)(cols - 1)) / (float)cols;
    float th = (ah - gap * (float)(rows - 1)) / (float)rows;
    float head_h = (float)C.ws_px * SC * 1.55f;
    float body_aspect_h = tw / aspect + head_h;
    if (body_aspect_h < th) th = body_aspect_h;

    float used_w = (float)cols * tw + (float)(cols - 1) * gap;
    float used_h = (float)rows * th + (float)(rows - 1) * gap;
    float ox = ax + (aw - used_w) * 0.5f;
    float oy = ay + (ah - used_h) * 0.5f;

    for (int sidx = 0; sidx < NSLOTS; ++sidx) {
        int r = sidx / cols, c = sidx % cols;
        SLOTS[sidx].tile = (SDL_FRect){ ox + (float)c * (tw + gap),
                                        oy + (float)r * (th + gap), tw, th };
        if (SLOTS[sidx].ws < 0) continue;                 /* ghost: nothing to lay out */

        Ws *ws = &WSS[SLOTS[sidx].ws];
        ws->tile = SLOTS[sidx].tile;

        float p = C.pad * SC;
        float gutter = C.usage_dots ? C.dot_px * SC * 2.0f : 0.0f;
        SDL_FRect body = { ws->tile.x + p + gutter,
                           ws->tile.y + head_h,
                           ws->tile.w - 2.0f * p - gutter,
                           ws->tile.h - head_h - p };
        if (body.h < 8.0f) body.h = 8.0f;

        /* letterbox the body to the real screen aspect so the mini layout
         * has exactly the proportions of the monitor */
        float wa = (ws->rw > 0 && ws->rh > 0) ? (float)ws->rw / (float)ws->rh : aspect;
        float sw = body.w, sh = body.w / wa;
        if (sh > body.h) { sh = body.h; sw = body.h * wa; }
        ws->screen = (SDL_FRect){ body.x + (body.w - sw) * 0.5f,
                                  body.y + (body.h - sh) * 0.5f, sw, sh };
        layout_cards(ws);
    }

    /* match every slot with where it was a moment ago; things that were not
     * there yet grow out of their own centre */
    bool moved = false;
    for (int i = 0; i < NSLOTS; ++i) {
        SLOTS[i].from = rect_shrink(SLOTS[i].tile, 0.55f);
        bool matched = false;
        for (int j = 0; j < nold; ++j) {
            bool same = (SLOTS[i].ws >= 0)
                          ? (old[j].ws == SLOTS[i].ws)
                          : (old[j].ws < 0 && old[j].num == SLOTS[i].num);
            if (same && old[j].tile.w > 0.0f) {
                SLOTS[i].from = old[j].tile;
                matched = true;
                break;
            }
        }
        if (!laid_out_once) SLOTS[i].from = SLOTS[i].tile;      /* no intro */
        if (!matched || SDL_fabsf(SLOTS[i].from.x - SLOTS[i].tile.x) > 0.5f ||
                        SDL_fabsf(SLOTS[i].from.y - SLOTS[i].tile.y) > 0.5f ||
                        SDL_fabsf(SLOTS[i].from.w - SLOTS[i].tile.w) > 0.5f)
            moved = true;
        if (SLOTS[i].ws >= 0) WSS[SLOTS[i].ws].tile_from = SLOTS[i].from;
    }

    /* ghosts that are no longer part of the grid shrink away instead of
     * blinking out; they are drawn from this list until they are done */
    int dying = 0;
    for (int j = 0; j < nold && dying < (int)SDL_arraysize(DYING); ++j) {
        if (old[j].ws >= 0) continue;
        bool still_there = false;
        for (int i = 0; i < NSLOTS; ++i)
            if (SLOTS[i].ws < 0 && SLOTS[i].num == old[j].num) { still_there = true; break; }
        if (still_there) continue;

        DYING[dying].ws   = -1;
        DYING[dying].num  = old[j].num;
        DYING[dying].from = old[j].tile;
        DYING[dying].tile = rect_shrink(old[j].tile, 0.55f);
        dying++;
    }
    if (dying > 0) {                       /* a later relayout must not eat them */
        NDYING = dying;
        dying_start = SDL_GetTicks();
        moved = true;
    }

    if (moved && laid_out_once) anim_start = SDL_GetTicks();
    laid_out_once = true;

    build_texts();

    /* the clickable name area, between the count and the number */
    for (int i = 0; i < NWS; ++i) {
        Ws *ws = &WSS[i];
        float p = C.pad * SC;
        float head_h = (float)C.ws_px * SC * 1.55f;
        float left  = ws->tile.x + p + (ws->sub.t ? (float)ws->sub.w : 0.0f) + p;
        float right = ws->tile.x + ws->tile.w - p -
                      (ws->badge.t ? (float)ws->badge.w : 0.0f) - p;
        ws->title_box = (SDL_FRect){ left, ws->tile.y, SDL_max(right - left, 0.0f), head_h };
    }
}

/* ------------------------------------------------------------- navigation */

static bool win_visible(const Win *w) { return !filtering || w->match; }

/* first window of the workspace that matches the query, if any */
static int ws_first_visible_match(const Ws *ws)
{
    for (int i = 0; i < ws->count; ++i)
        if (WINS[ws->first + i].match) return i;
    return -1;
}

static int ws_first_visible(const Ws *ws)
{
    for (int i = 0; i < ws->count; ++i)
        if (win_visible(&WINS[ws->first + i])) return i;
    return -1;
}

/* select a workspace; keep_window=false drops down to workspace level */
static void select_ws(int idx, bool keep_window);
static void select_ws(int idx, bool keep_window)
{
    if (NWS == 0) return;
    sel_ws = (idx % NWS + NWS) % NWS;
    Ws *ws = &WSS[sel_ws];
    if (!keep_window || ws->count == 0) { ws->sel = -1; return; }
    if (ws->sel >= ws->count || (ws->sel >= 0 && !win_visible(&WINS[ws->first + ws->sel]))) {
        int v = ws_first_visible(ws);
        ws->sel = v;
    }
}

static void step_ws(int dir)
{
    for (int i = 1; i <= NWS; ++i) {
        int cand = (((sel_ws + dir * i) % NWS) + NWS) % NWS;
        if (qlen == 0 || WSS[cand].count == 0) { select_ws(cand, false); return; }
        if (ws_first_visible(&WSS[cand]) >= 0) { select_ws(cand, false); return; }
    }
}

/* one row up or down in the grid, wrapping through the list */
static void step_ws_row(int dir)
{
    if (NWS == 0) return;
    int step = SDL_max(1, GRID_COLS) * dir;
    int cand = ((sel_ws + step) % NWS + NWS) % NWS;
    select_ws(cand, false);
}

static float rect_cx(SDL_FRect r) { return r.x + r.w * 0.5f; }
static float rect_cy(SDL_FRect r) { return r.y + r.h * 0.5f; }

/* move the selection one step into direction (dx, dy); crosses tiles */
static void navigate(int dx, int dy)
{
    if (NWS == 0) return;
    Ws *ws = &WSS[sel_ws];

    if (ws->sel < 0 && ws->count > 0) {          /* step into the workspace */
        int v = ws_first_visible(ws);
        if (v >= 0) { ws->sel = v; return; }
    }
    Win *cur = ws->count ? ws_sel_win(ws) : NULL;

    float fx = cur ? rect_cx(cur->card) : rect_cx(ws->tile);
    float fy = cur ? rect_cy(cur->card) : rect_cy(ws->tile);

    int best = -1;
    float best_score = 1e30f;

    for (int i = 0; i < ws->count; ++i) {
        Win *w = &WINS[ws->first + i];
        if (w == cur || !win_visible(w)) continue;
        float cx = rect_cx(w->card) - fx, cy = rect_cy(w->card) - fy;
        float along = cx * (float)dx + cy * (float)dy;
        float perp  = SDL_fabsf(cx * (float)dy - cy * (float)dx);
        if (along <= 4.0f * SC) continue;
        float score = along + perp * 2.0f;
        if (score < best_score) { best_score = score; best = i; }
    }
    if (best >= 0) { ws->sel = best; return; }

    /* nothing in that direction inside the tile: jump to the next tile */
    int best_ws = -1;
    best_score = 1e30f;
    for (int i = 0; i < NWS; ++i) {
        if (i == sel_ws) continue;
        float cx = rect_cx(WSS[i].tile) - rect_cx(ws->tile);
        float cy = rect_cy(WSS[i].tile) - rect_cy(ws->tile);
        float along = cx * (float)dx + cy * (float)dy;
        float perp  = SDL_fabsf(cx * (float)dy - cy * (float)dx);
        if (along <= 1.0f) continue;
        float score = along + perp * 3.0f;
        if (score < best_score) { best_score = score; best_ws = i; }
    }
    if (best_ws < 0) {
        /* nothing further in that direction: wrap around to the far side,
         * staying as close to the current row or column as possible */
        float bs = 1e30f;
        for (int i = 0; i < NWS; ++i) {
            if (i == sel_ws) continue;
            float cx = rect_cx(WSS[i].tile), cy = rect_cy(WSS[i].tile);
            float along = cx * (float)dx + cy * (float)dy;          /* far side */
            float perp  = SDL_fabsf((cx - rect_cx(ws->tile)) * (float)dy -
                                    (cy - rect_cy(ws->tile)) * (float)dx);
            float score = along + perp * 3.0f;
            if (score < bs) { bs = score; best_ws = i; }
        }
    }

    if (best_ws >= 0) {
        select_ws(best_ws, false);
        Ws *nw = &WSS[sel_ws];
        if (nw->count > 0) {                       /* land on the closest card */
            int bi = -1;
            float bs = 1e30f;
            for (int i = 0; i < nw->count; ++i) {
                Win *w = &WINS[nw->first + i];
                if (!win_visible(w)) continue;
                float d = SDL_fabsf(rect_cx(w->card) - fx) + SDL_fabsf(rect_cy(w->card) - fy);
                if (d < bs) { bs = d; bi = i; }
            }
            if (bi >= 0) nw->sel = bi;
        }
    }
}

static void apply_filter(void)
{
    for (int i = 0; i < NWIN; ++i) {
        Win *w = &WINS[i];
        w->match = (qlen == 0) || ci_contains(w->title, query) ||
                   ci_contains(w->app_id, query) || ci_contains(w->app_key, query) ||
                   ci_contains(w->ws_name, query);
    }
    if (qlen > 0) {                       /* jump to the first workspace with a hit */
        for (int i = 0; i < NWS; ++i) {
            int v = ws_first_visible_match(&WSS[i]);
            if (v >= 0) { sel_ws = i; WSS[i].sel = v; sel_active = true; return; }
        }
    } else {
        select_ws(sel_ws, true);
    }
}

static int hit_test(float mx, float my, int *out_ws)
{
    *out_ws = -1;
    for (int i = 0; i < NWS; ++i) {
        Ws *ws = &WSS[i];
        if (mx < ws->tile.x || mx >= ws->tile.x + ws->tile.w ||
            my < ws->tile.y || my >= ws->tile.y + ws->tile.h) continue;
        *out_ws = i;
        for (int j = ws->count - 1; j >= 0; --j) {      /* floats are last = on top */
            Win *w = &WINS[ws->first + j];
            SDL_FRect r = (w->hit.w > 0.0f) ? w->hit : w->card;
            if (mx >= r.x && mx < r.x + r.w && my >= r.y && my < r.y + r.h)
                return ws->first + j;
        }
        return -1;
    }
    return -1;
}

static char *escape_arg(const char *s)
{
    size_t n = strlen(s);
    char *out = (char *)xmalloc(n * 2 + 1);
    char *o = out;
    for (const char *p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') *o++ = '\\';
        *o++ = *p;
    }
    *o = 0;
    return out;
}

/* ------------------------------------------------------------ drag & drop
 * A press only arms a drag. If the pointer stays put, the release focuses
 * (that is the "press and release on the same thing" rule); if it moves, we
 * are dragging either a window or a whole workspace.
 */

typedef enum {
    DROP_NONE,
    DROP_WIN_WS,     /* window onto a workspace tile                       */
    DROP_WIN_NEAR,   /* window next to another window, splitting h or v    */
    DROP_WS_SWAP,    /* workspace onto another workspace                   */
    DROP_WS_NUM,     /* workspace onto a free number (a ghost slot)        */
    DROP_WIN_NEWWS   /* window onto a free number: sway creates it          */
} DropKind;

enum { EDGE_LEFT, EDGE_RIGHT, EDGE_TOP, EDGE_BOTTOM };

static bool  press_down;
static float press_x, press_y;
static int   press_win = -1, press_ws = -1;
static bool  drag_active;
static float drag_x, drag_y;
static SDL_FRect drag_src_tile;   /* where the dragged window's workspace was
                                   * when the drag began — a fixed rectangle,
                                   * so the reflow cannot move the threshold */

static DropKind drop_kind;
static int   drop_ws = -1, drop_win = -1, drop_num = -1, drop_edge = EDGE_RIGHT;
static bool  drop_insert;        /* WS_SWAP position means "insert here"   */

static void layout(void);
/* the workspace sway is showing right now */
static void select_current_workspace(void)
{
    sel_ws = 0;
    for (int i = 0; i < NWS; ++i)
        if (WSS[i].focused) { sel_ws = i; return; }
    for (int i = 0; i < NWS; ++i)
        if (WSS[i].visible) { sel_ws = i; return; }
}

static void reload_model(void);

static int slot_at(float x, float y)
{
    for (int i = 0; i < NSLOTS; ++i) {
        SDL_FRect t = SLOTS[i].tile;
        if (x >= t.x && x < t.x + t.w && y >= t.y && y < t.y + t.h) return i;
    }
    return -1;
}

/* which side of a card the pointer is closest to */
static int edge_at(SDL_FRect r, float x, float y)
{
    float dx = (x - (r.x + r.w * 0.5f)) / SDL_max(1.0f, r.w);
    float dy = (y - (r.y + r.h * 0.5f)) / SDL_max(1.0f, r.h);
    if (SDL_fabsf(dx) >= SDL_fabsf(dy)) return dx < 0.0f ? EDGE_LEFT : EDGE_RIGHT;
    return dy < 0.0f ? EDGE_TOP : EDGE_BOTTOM;
}

static bool point_in(SDL_FRect r, float x, float y)
{
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void set_ghosts(int lo, int hi)
{
    if (lo == ghost_lo && hi == ghost_hi) return;
    ghost_lo = lo;
    ghost_hi = hi;
    layout();      /* the ghosts take their place, layout() starts the glide */
}

/* A workspace being dragged always wants the free numbers on screen. A window
 * asks for them once it leaves the workspace it lives on — and then keeps
 * them for the rest of the drag. Toggling them back off would move that very
 * workspace back under the pointer, which asks for them again: the tiles would
 * shudder along the border instead of settling. */
static void update_ghosts(float x, float y)
{
    if (ghost_lo >= 0) return;                     /* already out, leave them */
    if (anim_running()) return;                    /* never decide mid-glide  */

    bool want = false;
    if (drag_ws_mode) {
        want = true;
    } else if (press_win >= 0) {
        want = !point_in(drag_src_tile, x, y);
    }
    if (want) set_ghosts(0, 10);
}

static void drag_update_target(float x, float y)
{
    drop_kind = DROP_NONE;
    drop_ws = drop_win = drop_num = -1;
    drop_insert = false;

    update_ghosts(x, y);

    int gs = slot_at(x, y);
    if (gs >= 0 && SLOTS[gs].ws < 0) {                /* a ghost: a free number */
        drop_kind = drag_ws_mode ? DROP_WS_NUM : DROP_WIN_NEWWS;
        drop_num  = SLOTS[gs].num;
        return;
    }

    if (drag_ws_mode) {
        int si = gs;
        if (si < 0) return;
        if (SLOTS[si].ws == press_ws) return;         /* itself */

        SDL_FRect t = SLOTS[si].tile;
        float rel = (x - t.x) / SDL_max(1.0f, t.w);
        drop_ws  = SLOTS[si].ws;
        drop_num = SLOTS[si].num;
        if (rel < 0.22f || rel > 0.78f) {             /* the edges insert */
            drop_kind   = DROP_WS_SWAP;
            drop_insert = true;
            drop_edge   = rel < 0.22f ? EDGE_LEFT : EDGE_RIGHT;
        } else {
            drop_kind = DROP_WS_SWAP;                 /* the middle swaps */
        }
        return;
    }

    int ws_idx = -1;
    int win_idx = hit_test(x, y, &ws_idx);
    if (ws_idx < 0) return;

    if (win_idx >= 0 && win_idx != press_win) {
        drop_kind = DROP_WIN_NEAR;
        drop_win  = win_idx;
        drop_ws   = ws_idx;
        drop_edge = edge_at(WINS[win_idx].card, x, y);
        return;
    }
    if (win_idx < 0) {
        drop_kind = DROP_WIN_WS;
        drop_ws   = ws_idx;
    }
}

/* "7:chat" keeps its label when it becomes workspace 8 */
static char *ws_name_with_num(const Ws *ws, int num)
{
    const char *colon = strchr(ws->name, ':');
    if (colon) return fmt_alloc("%d%s", num, colon);
    return fmt_alloc("%d", num);
}

static void ws_rename(const Ws *ws, const char *to)
{
    char *from = escape_arg(ws->name);
    char *dst  = escape_arg(to);
    sway_cmd("rename workspace \"%s\" to \"%s\"", from, dst);
    free(from);
    free(dst);
}

static void act_ws_assign(int ws_idx, int num)
{
    if (ws_idx < 0 || ws_idx >= NWS) return;
    char *to = ws_name_with_num(&WSS[ws_idx], num);
    ws_rename(&WSS[ws_idx], to);
    free(to);
}

static void act_ws_swap(int a, int b)
{
    if (a < 0 || b < 0 || a == b) return;
    int na = WSS[a].num, nb = WSS[b].num;
    if (na < 0 || nb < 0) return;

    char *tmp = fmt_alloc("swov_tmp_%d", na);
    ws_rename(&WSS[a], tmp);
    act_ws_assign(b, na);

    char *to = ws_name_with_num(&WSS[a], nb);          /* a is called tmp now */
    char *from = escape_arg(tmp);
    char *dst  = escape_arg(to);
    sway_cmd("rename workspace \"%s\" to \"%s\"", from, dst);
    free(from);
    free(dst);
    free(to);
    free(tmp);
}

/* Insert the dragged workspace at `num`, pushing the occupied run up by one.
 * With 1, 2, 5 on screen, inserting at 2 moves 2 to 3 and stops there. */
static void act_ws_insert(int ws_idx, int num)
{
    if (ws_idx < 0 || num < 0) return;
    if (WSS[ws_idx].num == num) return;

    int run[MAX_WORKSPACES];
    int n = 0;
    for (int want = num; n < MAX_WORKSPACES; ++want) {
        int found = -1;
        for (int i = 0; i < NWS; ++i)
            if (i != ws_idx && WSS[i].num == want) { found = i; break; }
        if (found < 0) break;
        run[n++] = found;
    }
    for (int i = n - 1; i >= 0; --i)                   /* top down, no clashes */
        act_ws_assign(run[i], WSS[run[i]].num + 1);
    act_ws_assign(ws_idx, num);
}

/* Put the dragged window next to `target`, splitting the target the right
 * way first. A mark is the only reliable way to say "there" to sway. */
static void act_win_drop_near(Win *drag, Win *target, int edge)
{
    if (!drag || !target || drag == target) return;
    bool horiz = (edge == EDGE_LEFT || edge == EDGE_RIGHT);

    sway_cmd("[con_id=%d] mark --add _swov_drop", target->con_id);
    sway_cmd("[con_id=%d] split %s", target->con_id, horiz ? "h" : "v");
    sway_cmd("[con_id=%d] move container to mark _swov_drop", drag->con_id);
    if (edge == EDGE_LEFT || edge == EDGE_TOP)
        sway_cmd("[con_id=%d] move %s", drag->con_id, horiz ? "left" : "up");
    sway_cmd("unmark _swov_drop");
}

static void drag_finish(void)
{
    switch (drop_kind) {
    case DROP_WIN_WS:
        if (press_win >= 0 && drop_ws >= 0 && WINS[press_win].ws != drop_ws) {
            Ws *ws = &WSS[drop_ws];
            if (ws->num >= 0)
                sway_cmd("[con_id=%d] move container to workspace number %d",
                         WINS[press_win].con_id, ws->num);
            else {
                char *e = escape_arg(ws->name);
                sway_cmd("[con_id=%d] move container to workspace \"%s\"",
                         WINS[press_win].con_id, e);
                free(e);
            }
        }
        break;

    case DROP_WIN_NEAR:
        if (press_win >= 0 && drop_win >= 0)
            act_win_drop_near(&WINS[press_win], &WINS[drop_win], drop_edge);
        break;

    case DROP_WS_SWAP:
        if (drop_insert) {
            int num = drop_num + (drop_edge == EDGE_RIGHT ? 1 : 0);
            act_ws_insert(press_ws, num);
        } else {
            act_ws_swap(press_ws, drop_ws);
        }
        break;

    case DROP_WS_NUM:
        act_ws_assign(press_ws, drop_num);
        break;

    case DROP_WIN_NEWWS:                     /* sway creates the workspace */
        if (press_win >= 0 && drop_num >= 0)
            sway_cmd("[con_id=%d] move container to workspace number %d",
                     WINS[press_win].con_id, drop_num);
        break;

    case DROP_NONE:
    default:
        break;
    }

    drag_active = drag_ws_mode = false;
    drop_kind = DROP_NONE;
    press_win = press_ws = -1;
    ghost_lo = ghost_hi = -1;
    layout();
    reload_model();          /* the event socket refreshes again once sway settles */
}

static void drag_cancel(void)
{
    bool had_ghosts = ghost_lo >= 0;
    drag_active = drag_ws_mode = false;
    drop_kind = DROP_NONE;
    press_down = false;
    press_win = press_ws = -1;
    ghost_lo = ghost_hi = -1;
    if (had_ghosts) layout();
}

/* ---------------------------------------------------------------- chrome */

static Tex T_HEADER, T_HINTS, T_QUERY;

static void rebuild_chrome(void)
{
    tex_free(&T_HEADER);
    tex_free(&T_HINTS);
    tex_free(&T_QUERY);

    if (C.show_header) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s   %d window%s on %d workspace%s",
                 FOCUSED_OUTPUT[0] ? FOCUSED_OUTPUT : "sway",
                 NWIN, NWIN == 1 ? "" : "s", NWS, NWS == 1 ? "" : "s");
        T_HEADER = text_make(F_HINT, buf);
    }
    if (C.show_hints) {
        /* when both lines share a band the hints get whatever width is left */
        int avail = RW - (int)(C.margin * SC * 2.0f);
        bool same_band = C.show_header && !pos_is_none(C.header_pos) &&
                         !pos_is_none(C.hints_pos) &&
                         pos_is_top(C.header_pos) == pos_is_top(C.hints_pos);
        if (same_band && T_HEADER.t) avail -= T_HEADER.w + (int)(C.margin * SC);

        T_HINTS = text_make_fit(F_HINT,
            "\xe2\x86\xb5 focus    drag to move    tab workspace    0-9 go to    "
            "ctrl+0-9 move    space mark    x close    f find    / filter    "
            "esc quit", avail);
    }

    if (confirm_kill) {
        int n = 0;
        for (int i = 0; i < NWIN; ++i) if (WINS[i].marked) n++;
        if (n == 0 && NWS > 0) {
            Win *w = ws_sel_win(&WSS[sel_ws]);
            n = w ? 1 : WSS[sel_ws].count;
        }
        char buf[192];
        snprintf(buf, sizeof(buf), "close %d window%s?   enter to confirm",
                 n, n == 1 ? "" : "s");
        T_QUERY = text_make(F_HINT, buf);
    } else if (query_active()) {
        char buf[192];
        snprintf(buf, sizeof(buf), "%s  %s\xe2\x96\x8f",
                 filtering ? "filter:" : "search:", query);
        T_QUERY = text_make(F_HINT, buf);
    }
}

/* --------------------------------------------------------------- drawing */

/* While the grid is settling, a workspace is drawn where it currently is on
 * its way, and everything inside it rides along on the same transform. */
static float XF_SX = 1.0f, XF_SY = 1.0f, XF_X0, XF_Y0, XF_DX, XF_DY;
static bool  XF_ON;

static void xf_set(SDL_FRect from, SDL_FRect to)
{
    XF_ON = true;
    XF_SX = (from.w > 0.0f) ? to.w / from.w : 1.0f;
    XF_SY = (from.h > 0.0f) ? to.h / from.h : 1.0f;
    XF_X0 = from.x;
    XF_Y0 = from.y;
    XF_DX = to.x;
    XF_DY = to.y;
}

static void xf_off(void) { XF_ON = false; XF_SX = XF_SY = 1.0f; }

static SDL_FRect xf(SDL_FRect r)
{
    if (!XF_ON) return r;
    return (SDL_FRect){ XF_DX + (r.x - XF_X0) * XF_SX, XF_DY + (r.y - XF_Y0) * XF_SY,
                        r.w * XF_SX, r.h * XF_SY };
}

static void draw_icon(SDL_Texture *icon, float cx, float top, float size)
{
    if (!icon) return;
    float w = size, h = size;
    float tw = 0.0f, th = 0.0f;
    if (SDL_GetTextureSize(icon, &tw, &th) && tw > 0.0f && th > 0.0f) {
        if (tw > th) h = size * (th / tw);
        else if (th > tw) w = size * (tw / th);
    }
    SDL_FRect dst = { cx - w * 0.5f, top + (size - h) * 0.5f, w, h };
    SDL_RenderTexture(REN, icon, NULL, &dst);
}

static void draw_card(Win *w, bool tile_selected)
{
    SDL_FRect r = xf(w->card);
    if (r.w < 4.0f || r.h < 4.0f) return;

    bool selected = tile_selected && sel_active && ws_sel_win(&WSS[w->ws]) == w;
    bool is_hovered = selected;          /* pointer and keyboard share a cursor */
    bool dimmed   = filtering && !w->match;
    bool is_hit   = searching && qlen > 0 && w->match;
    float rad     = SDL_min(C.radius * SC * 0.62f, SDL_min(r.w, r.h) * 0.32f);

    SDL_FColor fill = w->focused ? C.card_focus : C.card;
    if (is_hovered) fill = C.card_hover;
    if (is_hit)     fill = mix(fill, C.match, 0.22f);
    if (confirm_kill && (w->marked || selected)) fill = mix(fill, C.urgent, 0.25f);
    if (selected)   fill = mix(fill, C.hl, is_hovered ? 0.16f : 0.10f);
    if (dimmed)     fill = with_alpha(mix(fill, C.tile, 0.6f), fill.a * 0.45f);

    /* A floating dialog, and especially a fullscreen window, sits on top of
     * the tiled ones. Drawing it solid would hide the whole workspace, so it
     * gets see-through — the more of the screen it covers, the more so. Its
     * border, icon and label stay at full strength, so it is still obvious
     * which window it is. */
    float over = 1.0f, cover = 0.0f;
    bool on_top = w->floating || w->fullscreen;
    if (on_top) {
        const Ws *ws = &WSS[w->ws];
        float screen_area = ws->screen.w * ws->screen.h;
        cover = (screen_area > 1.0f) ? (w->card.w * w->card.h) / screen_area : 1.0f;
        cover = SDL_clamp(cover, 0.0f, 1.0f);
        over = C.float_alpha * (1.0f - 0.55f * cover);
        fill = with_alpha(fill, fill.a * over);
    }
    /* Anything drawn on top of other windows gets a frame and a name plate
     * rather than a filled card: its label would otherwise sit in the middle,
     * exactly where the label of the window showing through already is. */
    bool as_overlay = on_top;

    if (w->floating) drop_shadow(r, rad, 9.0f * SC, with_alpha(C.shadow_col, C.shadow_col.a * over));
    fill_round_rect(r, rad, fill);

    /* border: selection beats hover beats mark beats plain */
    float bw = SDL_max(1.0f, C.border * SC * 0.75f);
    if (confirm_kill && (w->marked || selected))
                           stroke_round_rect(r, rad, bw * 1.35f, C.urgent);
    else if (selected)     stroke_round_rect(r, rad, bw * 1.35f, C.hl);
    else if (is_hit)       stroke_round_rect(r, rad, bw * 1.2f, C.match);
    else if (is_hovered)   stroke_round_rect(r, rad, bw, C.accent);
    else if (w->marked)    stroke_round_rect(r, rad, bw, with_alpha(C.hl, 0.85f));
    else if (w->urgent)    stroke_round_rect(r, rad, bw, C.urgent);
    else if (on_top)       stroke_round_rect(r, rad, SDL_max(1.0f, bw * 0.7f),
                                             with_alpha(C.accent, dimmed ? 0.25f : 0.5f));
    else                   stroke_round_rect(r, rad, SDL_max(1.0f, bw * 0.4f),
                                             with_alpha(C.outline, dimmed ? 0.3f : 0.7f));

    /* the window sway has focused: marked as current, not as selected */
    if (w->focused && !dimmed) {
        SDL_FRect bar = { r.x + bw * 0.6f, r.y + rad * 0.7f,
                          SDL_max(2.0f, 3.0f * SC), r.h - rad * 1.4f };
        fill_round_rect(bar, bar.w * 0.5f, C.current);
    }

    /* multi selection marker */
    if (w->marked) {
        float d = SDL_max(6.0f, 9.0f * SC);
        SDL_FRect dot = { r.x + r.w - d - 5.0f * SC, r.y + 5.0f * SC, d, d };
        fill_round_rect(dot, d * 0.5f, C.hl);
    }

    float pad = card_pad(w);
    float a   = dimmed ? 0.45f : 1.0f;
    SDL_FColor lab_col = (selected || is_hovered) ? C.text : C.subtext;
    if (w->urgent) lab_col = C.urgent;
    SDL_FColor sub_col = mix(C.dim, C.text, (selected || is_hovered) ? 0.55f : 0.32f);
    lab_col = with_alpha(lab_col, lab_col.a * a);
    sub_col = with_alpha(sub_col, sub_col.a * a);

    bool has_icon = C.icons && w->icon && w->lay_icon >= 10.0f * SC;
    if (has_icon) SDL_SetTextureAlphaModFloat(w->icon, a);

    SDL_FRect plate;
    float plate_iw = 0.0f;
    if (as_overlay && overlay_plate(w, r, &plate, &plate_iw)) {
        /* Centre would land on top of the cards showing through, so the name
         * rides in a small plate at the top edge instead. That plate is also
         * the only part of this window the mouse can grab. */
        float ip = 6.0f * SC;
        fill_round_rect(plate, plate.h * 0.32f, with_alpha(mix(C.card, C.bg, 0.25f), 0.94f));
        stroke_round_rect(plate, plate.h * 0.32f, SDL_max(1.0f, 1.4f * SC),
                          is_hovered ? C.accent : with_alpha(C.accent, 0.55f));

        float tx = plate.x + ip;
        if (plate_iw > 0.0f && has_icon) {
            float ih = plate_iw - 5.0f * SC;
            draw_icon(w->icon, tx + ih * 0.5f, plate.y + (plate.h - ih) * 0.5f, ih);
            tx += plate_iw;
        }
        tex_draw(w->label, tx, plate.y + (plate.h - (float)w->label.h) * 0.5f,
                 with_alpha(C.text, a));
        if (has_icon) SDL_SetTextureAlphaModFloat(w->icon, 1.0f);
        return;
    }

    float lab_h = w->label.t    ? (float)w->label.h    : 0.0f;
    float sub_h = w->subtitle.t ? (float)w->subtitle.h : 0.0f;
    float g1 = 4.0f * SC, g2 = 1.0f * SC;

    switch (w->lay_mode) {
    case CL_ROW: {
        float tx = r.x + pad;
        if (has_icon) {
            draw_icon(w->icon, tx + w->lay_icon * 0.5f,
                      r.y + (r.h - w->lay_icon) * 0.5f, w->lay_icon);
            tx += w->lay_icon + 6.0f * SC;
        }
        float block = lab_h + (sub_h > 0.0f ? g2 + sub_h : 0.0f);
        float y = r.y + (r.h - block) * 0.5f;
        tex_draw(w->label, tx, y, lab_col);
        if (sub_h > 0.0f) tex_draw(w->subtitle, tx, y + lab_h + g2, sub_col);
        break;
    }
    case CL_TEXT: {
        float y = r.y + (r.h - lab_h) * 0.5f;
        tex_draw_center(w->label, r.x + r.w * 0.5f, y, lab_col);
        break;
    }
    case CL_ICON: {
        if (has_icon)
            draw_icon(w->icon, r.x + r.w * 0.5f,
                      r.y + (r.h - w->lay_icon) * 0.5f, w->lay_icon);
        break;
    }
    default: {                                     /* CL_STACK */
        float icon = has_icon ? w->lay_icon : 0.0f;
        float block = icon + (lab_h > 0.0f ? g1 + lab_h : 0.0f) +
                             (sub_h > 0.0f ? g2 + sub_h : 0.0f);
        float cx = r.x + r.w * 0.5f;
        /* a window drawn on top keeps to the upper edge, where it is not
         * sitting on the label of whatever shows through beneath it */
        float y  = as_overlay ? r.y + pad : r.y + (r.h - block) * 0.5f;
        if (has_icon) { draw_icon(w->icon, cx, y, icon); y += icon; }
        if (lab_h > 0.0f) { y += g1; tex_draw_center(w->label, cx, y, lab_col); y += lab_h; }
        if (sub_h > 0.0f) { y += g2; tex_draw_center(w->subtitle, cx, y, sub_col); }
        break;
    }
    }

    if (has_icon) SDL_SetTextureAlphaModFloat(w->icon, 1.0f);
}

static void draw_workspace(int idx)
{
    Ws *ws = &WSS[idx];
    bool selected = sel_active && (idx == sel_ws);
    bool hovered  = false;               /* the selection is the only cursor */
    float rad = C.radius * SC;

    float t = anim_phase();
    if (t < 1.0f && ws->tile_from.w > 0.0f) xf_set(ws->tile, rect_lerp(ws->tile_from, ws->tile, t));
    else xf_off();

    SDL_FRect tile = xf(ws->tile);

    bool has_hit = qlen == 0 || ws->count == 0 || ws_first_visible(ws) >= 0;

    SDL_FColor fill = selected ? C.tile_sel : (hovered ? C.tile_hover : C.tile);
    if (!has_hit) fill = with_alpha(mix(fill, C.bg, 0.35f), fill.a * 0.75f);

    drop_shadow(tile, rad, 7.0f * SC, C.shadow_col);
    fill_round_rect(tile, rad, fill);

    SDL_FColor bc = C.outline;
    float bw = C.border * SC;
    if (selected)          bc = C.hl;
    else if (hovered)      bc = with_alpha(C.accent, 0.9f);
    else if (ws->urgent)   bc = C.urgent;
    else if (searching && qlen > 0 && ws_first_visible_match(ws) >= 0)
                           bc = with_alpha(C.match, 0.8f);
    else if (ws->visible)  bc = with_alpha(C.current, 0.75f);
    else                   bw = SDL_max(1.0f, C.border * SC * 0.45f);
    stroke_round_rect(tile, rad, bw, bc);

    /* ---- tile header: count left, name centred, number right ---- */
    float p = C.pad * SC;
    float top = tile.y + C.badge_top * SC;

    bool live = ws->visible || ws->focused;
    SDL_FColor num_col = selected ? C.hl
                       : live     ? C.current
                                  : mix(C.subtext, C.tile, 0.25f);
    if (ws->urgent && !selected) num_col = C.urgent;

    tex_draw(ws->badge, tile.x + tile.w - p - (float)ws->badge.w, top, num_col);

    float count_right = tile.x + p;
    if (ws->sub.t) {
        float base = top + ((float)ws->badge.h - (float)ws->sub.h) * 0.62f;
        SDL_FColor col = ws->count ? mix(C.dim, C.text, selected ? 0.45f : 0.25f)
                                   : with_alpha(C.dim, 0.85f);
        tex_draw(ws->sub, tile.x + p, base, col);
        count_right += (float)ws->sub.w;
    }

    /* the name sits between the two; clicking it starts a rename */
    (void)count_right;
    SDL_FRect tb = xf(ws->title_box);
    float tb_x = tb.x, tb_w = tb.w;

    bool editing_this = editing && edit_ws == idx;
    if (editing_this) {
        SDL_FRect box = { tb_x, top - p * 0.35f, SDL_max(tb_w, 40.0f * SC),
                          (float)ws->badge.h + p * 0.7f };
        fill_round_rect(box, box.h * 0.3f, mix(C.tile, C.bg, 0.2f));
        stroke_round_rect(box, box.h * 0.3f, SDL_max(1.0f, 1.5f * SC), C.hl);

        char buf[80];
        snprintf(buf, sizeof(buf), "%s\xe2\x96\x8f", edit_buf);
        Tex t = text_make_fit(F_LABEL, buf[0] ? buf : "\xe2\x96\x8f", (int)(box.w - p));
        tex_draw(t, box.x + p * 0.5f, box.y + (box.h - (float)t.h) * 0.5f, C.text);
        tex_free(&t);
    } else if (ws->title.t && tb_w > 20.0f * SC) {
        Tex t = ws->title;
        float x = tb_x + (tb_w - (float)t.w) * 0.5f;
        if ((float)t.w > tb_w) x = tb_x;
        SDL_FColor col = selected ? C.text : mix(C.subtext, C.tile, 0.15f);
        tex_draw(t, x, top + ((float)ws->badge.h - (float)t.h) * 0.55f, col);
    }

    /* How much this workspace gets used, as a column of dots down the left
     * edge: the more of them are lit, the more time is spent here. The scale
     * is relative to the busiest workspace. */
    if (C.usage_dots && C.dot_count > 0) {
        float d = C.dot_px * SC;
        float gap = d * 0.62f;

        SDL_FRect screen_now = xf(ws->screen);
        int fits = (int)((screen_now.h + gap) / (d + gap));
        int n = SDL_clamp(SDL_min(C.dot_count, fits), 0, 40);
        if (n < 2) n = 0;
        float total = (float)n * d + (float)(n - 1) * gap;
        float cx = tile.x + C.pad * SC + d * 0.55f;
        float y0 = screen_now.y + (screen_now.h - total) * 0.5f;

        float frac = (USAGE_MAX > 0.0) ? (float)(ws->usage / USAGE_MAX) : 0.0f;
        int lit = (ws->usage > 0.0) ? (int)SDL_ceilf(frac * (float)n) : 0;
        lit = SDL_clamp(lit, 0, n);

        for (int i = 0; i < n; ++i) {
            /* fills from the bottom like a level gauge */
            bool on = (n - i) <= lit;
            SDL_FRect dot = { cx - d * 0.5f, y0 + (float)i * (d + gap), d, d };
            fill_round_rect(dot, d * 0.5f,
                            on ? C.current : with_alpha(C.dim, 0.38f));
        }
    }

    /* ---- the mini screen ---- */
    SDL_FRect screen = xf(ws->screen);
    float srad = SDL_min(rad * 0.7f, SDL_min(screen.w, screen.h) * 0.5f);
    fill_round_rect(screen, srad, C.mini_bg);
    stroke_round_rect(screen, srad, SDL_max(1.0f, 1.2f * SC), with_alpha(C.outline, 0.55f));

    if (ws->count == 0) {
        /* nothing here: keep the tile obviously "a screen, but empty" */
        xf_off();
        return;
    }

    /* a container whose children share one rectangle (tabbed or stacked) is
     * drawn as slices; a shared outline shows that they belong together */
    for (int i = 0; i < ws->count; ++i) {
        if (WINS[ws->first + i].group != i) continue;
        SDL_FRect u = WINS[ws->first + i].card;
        for (int j = i + 1; j < ws->count; ++j) {
            if (WINS[ws->first + j].group != i) continue;
            SDL_FRect c = WINS[ws->first + j].card;
            float x1 = SDL_max(u.x + u.w, c.x + c.w), y1 = SDL_max(u.y + u.h, c.y + c.h);
            u.x = SDL_min(u.x, c.x);
            u.y = SDL_min(u.y, c.y);
            u.w = x1 - u.x;
            u.h = y1 - u.y;
        }
        u = xf(u);
        float e = 3.0f * SC;
        SDL_FRect box = { u.x - e, u.y - e, u.w + 2.0f * e, u.h + 2.0f * e };
        stroke_round_rect(box, C.radius * SC * 0.7f, SDL_max(1.0f, 1.6f * SC),
                          with_alpha(C.accent, 0.5f));

        if (WINS[ws->first + i].has_tab) {       /* the strip the tabs sit on */
            SDL_FRect strip = xf(WINS[ws->first + i].tab);
            strip.x = box.x;
            strip.w = box.w;
            strip.y -= e * 0.5f;
            strip.h += e;
            fill_round_rect(strip, C.radius * SC * 0.5f, with_alpha(C.mini_bg, 0.8f));
        }
    }

    for (int i = 0; i < ws->count; ++i) {
        int gi = ws->first + i;
        draw_card(&WINS[gi], selected);
    }
    xf_off();
}


/* ---- what the drop would do, shown while the button is still held ---- */
static void draw_drop_indicator(void)
{
    float thick = SDL_max(2.0f, C.border * SC * 1.2f);

    switch (drop_kind) {
    case DROP_WIN_WS:
        if (drop_ws >= 0) {
            SDL_FRect r = WSS[drop_ws].screen;
            fill_round_rect(r, C.radius * SC * 0.7f, with_alpha(C.hl, 0.13f));
            stroke_round_rect(r, C.radius * SC * 0.7f, thick, C.hl);
        }
        break;

    case DROP_WIN_NEAR:
        if (drop_win >= 0) {
            SDL_FRect r = WINS[drop_win].card;
            stroke_round_rect(r, C.radius * SC * 0.6f, SDL_max(1.0f, thick * 0.6f),
                              with_alpha(C.accent, 0.8f));

            /* the bar shows which side the window lands on, and therefore
             * whether the container ends up split horizontally or vertically */
            float b = thick * 1.6f;
            SDL_FRect bar = r;
            if (drop_edge == EDGE_LEFT)        bar = (SDL_FRect){ r.x - b * 0.5f, r.y, b, r.h };
            else if (drop_edge == EDGE_RIGHT)  bar = (SDL_FRect){ r.x + r.w - b * 0.5f, r.y, b, r.h };
            else if (drop_edge == EDGE_TOP)    bar = (SDL_FRect){ r.x, r.y - b * 0.5f, r.w, b };
            else                               bar = (SDL_FRect){ r.x, r.y + r.h - b * 0.5f, r.w, b };
            fill_round_rect(bar, b * 0.5f, C.hl);
        }
        break;

    case DROP_WS_SWAP:
        if (drop_ws >= 0) {
            SDL_FRect t = WSS[drop_ws].tile;
            if (drop_insert) {
                float b = thick * 1.8f;
                float x = (drop_edge == EDGE_LEFT) ? t.x - b : t.x + t.w;
                SDL_FRect bar = { x, t.y, b, t.h };
                fill_round_rect(bar, b * 0.5f, C.hl);
            } else {
                fill_round_rect(t, C.radius * SC, with_alpha(C.hl, 0.16f));
                stroke_round_rect(t, C.radius * SC, thick, C.hl);
            }
        }
        break;

    default:
        break;
    }

    /* the thing being dragged, held under the pointer */
    if (drag_ws_mode && press_ws >= 0) {
        SDL_FRect t = WSS[press_ws].tile;
        float w = t.w * 0.34f, h = t.h * 0.34f;
        SDL_FRect g = { drag_x - w * 0.5f, drag_y - h * 0.5f, w, h };
        fill_round_rect(g, C.radius * SC * 0.7f, with_alpha(C.tile_sel, 0.92f));
        stroke_round_rect(g, C.radius * SC * 0.7f, thick, C.hl);
        tex_draw_in_box(WSS[press_ws].badge, g, F_BADGE, C.text);
    } else if (press_win >= 0) {
        Win *w = &WINS[press_win];
        SDL_FRect r = w->card;
        SDL_FRect g = { drag_x - r.w * 0.5f, drag_y - r.h * 0.5f, r.w, r.h };
        fill_round_rect(g, C.radius * SC * 0.6f, with_alpha(C.card_hover, 0.9f));
        stroke_round_rect(g, C.radius * SC * 0.6f, thick, C.hl);
        if (w->icon && C.icons) {
            float sz = SDL_min(SDL_min(g.w, g.h) * 0.5f, (float)C.icon_px * SC);
            draw_icon(w->icon, g.x + g.w * 0.5f, g.y + (g.h - sz) * 0.5f, sz);
        }
    }
}

static void render(void)
{
    SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_NONE);
    set_col(C.bg);
    SDL_RenderClear(REN);
    SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_BLEND);

    if (NWS == 0) {
        Tex t = text_make(F_LABEL, "no windows found on this output");
        tex_draw_center(t, (float)RW * 0.5f, (float)RH * 0.5f, C.subtext);
        tex_free(&t);
        return;
    }

    if (ghost_lo >= 0)                            /* free numbers to drop on */
        for (int i = 0; i < NSLOTS; ++i) {
            if (SLOTS[i].ws >= 0) continue;
            float ph = anim_phase();
            SDL_FRect t = rect_lerp(SLOTS[i].from, SLOTS[i].tile, ph);
            float ix = t.w * 0.22f, iy = t.h * 0.22f;
            SDL_FRect g = { t.x + ix, t.y + iy, t.w - 2.0f * ix, t.h - 2.0f * iy };
            bool hot = (drop_kind == DROP_WS_NUM || drop_kind == DROP_WIN_NEWWS) &&
                       drop_num == SLOTS[i].num;

            fill_round_rect(g, C.radius * SC * 0.8f,
                            with_alpha(hot ? C.hl : C.tile, (hot ? 0.30f : 0.55f) * ph));
            stroke_round_rect(g, C.radius * SC * 0.8f, SDL_max(1.0f, 2.0f * SC),
                              with_alpha(hot ? C.hl : C.accent, (hot ? 1.0f : 0.35f) * ph));

            char num[8];
            snprintf(num, sizeof(num), "%d", SLOTS[i].num);
            Tex t2 = text_make(F_BADGE, num);
            tex_draw_in_box(t2, g, F_BADGE, with_alpha(hot ? C.hl : C.dim, 0.9f * ph));
            tex_free(&t2);
        }

    if (NDYING > 0) {                             /* ghosts fading away */
        float ph = phase_since(dying_start);
        if (ph >= 1.0f) NDYING = 0;
        for (int i = 0; i < NDYING; ++i) {
            SDL_FRect t = rect_lerp(DYING[i].from, DYING[i].tile, ph);
            float ix = t.w * 0.22f, iy = t.h * 0.22f;
            SDL_FRect g = { t.x + ix, t.y + iy, t.w - 2.0f * ix, t.h - 2.0f * iy };
            float a = 1.0f - ph;

            fill_round_rect(g, C.radius * SC * 0.8f, with_alpha(C.tile, 0.55f * a));
            stroke_round_rect(g, C.radius * SC * 0.8f, SDL_max(1.0f, 2.0f * SC),
                              with_alpha(C.accent, 0.35f * a));

            char num[8];
            snprintf(num, sizeof(num), "%d", DYING[i].num);
            Tex t2 = text_make(F_BADGE, num);
            tex_draw_in_box(t2, g, F_BADGE, with_alpha(C.dim, 0.9f * a));
            tex_free(&t2);
        }
    }

    for (int i = 0; i < NWS; ++i)
        if (i != sel_ws) draw_workspace(i);
    draw_workspace(sel_ws);                       /* selection is drawn last */

    if (drag_active) draw_drop_indicator();

    float m = C.margin * SC;
    float head_x0 = (float)RW, head_x1 = 0.0f;
    bool  head_top = pos_is_top(C.header_pos);

    if (T_HEADER.t && !pos_is_none(C.header_pos)) {
        float y = head_top ? (HEADER_H - (float)T_HEADER.h) * 0.5f
                           : (float)RH - FOOTER_H * 0.5f - (float)T_HEADER.h * 0.5f;
        head_x0 = pos_x(C.header_pos, (float)T_HEADER.w, m);
        head_x1 = head_x0 + (float)T_HEADER.w;
        tex_draw(T_HEADER, head_x0, y, C.hint);
    }

    if (T_QUERY.t) {
        float p = 10.0f * SC;
        SDL_FRect box = { (float)RW * 0.5f - ((float)T_QUERY.w * 0.5f + p),
                          (HEADER_H - (float)T_QUERY.h) * 0.55f - p * 0.5f,
                          (float)T_QUERY.w + 2.0f * p, (float)T_QUERY.h + p };
        SDL_FColor qc = confirm_kill ? C.urgent : (filtering ? C.accent : C.match);
        fill_round_rect(box, box.h * 0.35f, mix(C.tile, C.bg, 0.15f));
        stroke_round_rect(box, box.h * 0.35f, SDL_max(1.0f, 1.5f * SC), with_alpha(qc, 0.8f));
        tex_draw(T_QUERY, box.x + p, box.y + p * 0.5f, qc);
    }

    if (T_HINTS.t && !pos_is_none(C.hints_pos)) {
        bool hint_top = pos_is_top(C.hints_pos);
        float y = hint_top ? (HEADER_H - (float)T_HINTS.h) * 0.5f
                           : (float)RH - FOOTER_H * 0.5f - (float)T_HINTS.h * 0.5f;
        float x = pos_x(C.hints_pos, (float)T_HINTS.w, m);

        /* both lines in the same band: keep the hints clear of the header */
        if (T_HEADER.t && head_top == hint_top && x + (float)T_HINTS.w > head_x0 - m * 0.5f) {
            if (head_x0 > (float)RW * 0.5f) x = SDL_min(x, head_x0 - m * 0.5f - (float)T_HINTS.w);
            else                            x = SDL_max(x, head_x1 + m * 0.5f);
            x = SDL_max(x, m * 0.5f);
        }
        tex_draw(T_HINTS, x, y, with_alpha(C.hint, 0.8f));
    }
}


/* --------------------------------------------------------------- actions */

static bool running = true;
static bool dirty   = true;      /* a frame is only drawn when this is set */

static void reload_model(void)
{
    /* remember what was selected so a reload does not lose the cursor */
    int keep_con = -1;
    char keep_ws[64] = {0};
    if (NWS > 0) {
        snprintf(keep_ws, sizeof(keep_ws), "%s", WSS[sel_ws].name);
        Win *w = ws_sel_win(&WSS[sel_ws]);
        if (w) keep_con = w->con_id;
    }
    int marks[MAX_WINDOWS];
    int nmarks = 0;
    for (int i = 0; i < NWIN && nmarks < MAX_WINDOWS; ++i)
        if (WINS[i].marked) marks[nmarks++] = WINS[i].con_id;

    if (!model_reload()) { running = false; return; }

    for (int i = 0; i < NWIN; ++i)
        for (int j = 0; j < nmarks; ++j)
            if (WINS[i].con_id == marks[j]) WINS[i].marked = true;

    select_current_workspace();

    for (int i = 0; i < NWS; ++i) {
        WSS[i].sel = -1;
        if (keep_ws[0] && strcmp(WSS[i].name, keep_ws) == 0) sel_ws = i;
        for (int j = 0; j < WSS[i].count; ++j)
            if (WINS[WSS[i].first + j].con_id == keep_con) { sel_ws = i; WSS[i].sel = j; }
    }

    layout();
    apply_filter();
    rebuild_chrome();
    hov_ws = hov_win = -1;
}

static void act_focus_window(Win *w)
{
    if (!w) return;
    sway_cmd("[con_id=%d] focus", w->con_id);
    running = false;
}

static void act_goto_workspace(const Ws *ws)
{
    if (!ws) return;
    char *e = escape_arg(ws->name);
    sway_cmd("workspace --no-auto-back-and-forth \"%s\"", e);
    free(e);
    if (C.track) usage_switch(ws->name);
    running = false;
}

/* windows the next action applies to: all marked ones, else the selected one.
 * Returns 0 when the selection is a whole workspace rather than windows. */
static int gather_targets(int *out, int cap)
{
    int n = 0;
    for (int i = 0; i < NWIN && n < cap; ++i)
        if (WINS[i].marked) out[n++] = WINS[i].con_id;
    if (n == 0 && NWS > 0) {
        Win *w = ws_sel_win(&WSS[sel_ws]);
        if (w) out[n++] = w->con_id;
    }
    return n;
}

static void act_move_to_workspace(int number)
{
    int ids[MAX_WINDOWS];
    int n = gather_targets(ids, MAX_WINDOWS);

    if (n > 0) {
        for (int i = 0; i < n; ++i)
            sway_cmd("[con_id=%d] move container to workspace number %d", ids[i], number);
    } else if (NWS > 0 && WSS[sel_ws].ntop > 0) {
        /* Whole workspace: move its top level containers, not the single
         * windows. A container carries its own split/tab layout along, so
         * the arrangement survives the move. */
        Ws *ws = &WSS[sel_ws];
        if (ws->num == number) return;
        for (int i = 0; i < ws->ntop; ++i)
            sway_cmd("[con_id=%d] move container to workspace number %d", ws->top[i], number);
    } else return;

    for (int i = 0; i < NWIN; ++i) WINS[i].marked = false;
    if (C.quit_after_action) { running = false; return; }
    reload_model();
}

static void act_close(void)
{
    int ids[MAX_WINDOWS];
    int n = gather_targets(ids, MAX_WINDOWS);

    if (n == 0 && NWS > 0 && WSS[sel_ws].sel < 0) {   /* the whole workspace */
        Ws *ws = &WSS[sel_ws];
        for (int i = 0; i < ws->count && n < MAX_WINDOWS; ++i)
            ids[n++] = WINS[ws->first + i].con_id;
    }
    for (int i = 0; i < n; ++i) sway_cmd("[con_id=%d] kill", ids[i]);
    for (int i = 0; i < NWIN; ++i) WINS[i].marked = false;
    reload_model();
}

static void act_toggle_mark(Win *w)
{
    if (w) w->marked = !w->marked;
}

/* mark every window of the workspace, or clear them all if they already are */
static void toggle_ws_marks(Ws *ws)
{
    bool all = ws->count > 0;
    for (int i = 0; i < ws->count; ++i)
        if (!WINS[ws->first + i].marked) { all = false; break; }
    for (int i = 0; i < ws->count; ++i) WINS[ws->first + i].marked = !all;
}

/* ----------------------------------------------------------------- fonts */

/* One fc-match run resolves the regular and the bold cut together; spawning
 * a process twice is the most expensive thing startup would otherwise do. */
static char *FC_REGULAR, *FC_BOLD;

static void fontconfig_resolve(const char *family)
{
    /* the family ends up in a shell command, so keep it to harmless characters */
    char fam[128] = "sans";
    if (family && *family) {
        size_t n = 0;
        for (const char *p = family; *p && n < sizeof(fam) - 1; ++p)
            if (isalnum((unsigned char)*p) || *p == ' ' || *p == '-' ||
                *p == '_' || *p == '.' || *p == ',') fam[n++] = *p;
        fam[n] = 0;
        if (!n) snprintf(fam, sizeof(fam), "sans");
    }

    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "fc-match -f '%%{file}\n' '%s' 2>/dev/null; "
             "fc-match -f '%%{file}\n' '%s:bold' 2>/dev/null", fam, fam);

    FILE *p = popen(cmd, "r");
    if (!p) return;
    char l1[1024] = {0}, l2[1024] = {0};
    if (fgets(l1, sizeof(l1), p)) str_trim(l1);
    if (fgets(l2, sizeof(l2), p)) str_trim(l2);
    pclose(p);

    if (l1[0] && file_readable(l1)) FC_REGULAR = xstrdup(l1);
    if (l2[0] && file_readable(l2)) FC_BOLD    = xstrdup(l2);
}

static char *pick_font(const char *spec, bool bold)
{
    if (spec && *spec && strchr(spec, '/') && file_readable(spec)) return xstrdup(spec);

    const char *hit = bold ? FC_BOLD : FC_REGULAR;
    if (hit) return xstrdup(hit);

    const char *fallback[] = {
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/liberation/LiberationSans-Regular.ttf"
    };
    for (size_t i = 0; i < SDL_arraysize(fallback); ++i)
        if (file_readable(fallback[i])) return xstrdup(fallback[i]);
    return NULL;
}

static TTF_Font *open_font(const char *path, float size, bool bold, bool synth_bold)
{
    TTF_Font *f = TTF_OpenFont(path, size);
    if (!f) return NULL;
    if (bold && synth_bold) TTF_SetFontStyle(f, TTF_STYLE_BOLD);
    TTF_SetFontHinting(f, TTF_HINTING_LIGHT);
    return f;
}

static void load_fonts(void)
{
    /* a configured path needs no fontconfig at all */
    bool have_paths = C.font[0] && strchr(C.font, '/') &&
                      (!C.font_bold[0] || strchr(C.font_bold, '/'));
    if (!have_paths) fontconfig_resolve(C.font);

    char *regular = pick_font(C.font, false);
    if (!regular) die("no usable font found (install fontconfig and a TTF font)");
    char *bold = C.font_bold[0] ? pick_font(C.font_bold, true) : pick_font(C.font, true);
    bool synth = false;
    if (!bold) { bold = xstrdup(regular); synth = true; }
    else if (strcmp(bold, regular) == 0) synth = true;

    float u = C.ui_scale * SC;
    F_BADGE = open_font(bold,    (float)C.ws_px    * u, true, synth);
    F_LABEL = open_font(bold,    (float)C.label_px * u, true, synth);
    F_TITLE = open_font(regular, (float)C.title_px * u, false, false);
    F_HINT  = open_font(regular, (float)C.hint_px  * u, false, false);
    free(regular);
    free(bold);
    free(FC_REGULAR);
    free(FC_BOLD);
    FC_REGULAR = FC_BOLD = NULL;

    if (!F_BADGE || !F_LABEL || !F_TITLE || !F_HINT) die("could not open fonts: %s", SDL_GetError());
}

/* ------------------------------------------------------------------ main */

static SDL_Window  *WIN_HANDLE;
static const char  *SHOT_PATH;
static float        SHOT_MX = -1.0f, SHOT_MY = -1.0f;
static SDL_Texture *TARGET;
static SDL_Cursor  *CUR_ARROW, *CUR_HAND;
static float        MOUSE_SCALE = 1.0f;

static void usage(void)
{
    printf(
"swov — window and workspace overview for sway\n"
"\n"
"usage: swov [options] [key=value ...]\n"
"  -g, --go N|NAME     switch to that workspace and exit, without a window\n"
"  -b, --back          switch to the previously used workspace and exit\n"
"      --usage         print how long each workspace has been used, and exit\n"
"      --info          print every path and setting swov is using, and exit\n"
"  -c, --config PATH   read this config file instead of the default\n"
"  -n, --no-config     ignore the config file\n"
"      --shot PATH     render one frame to a PNG and exit (handy for tuning)\n"
"  -h, --help          this text\n"
"\n"
"Every config key is also a command line option, in three spellings:\n"
"      swov --ui_scale=1.2 hl=ff8800 -s ssaa=1\n"
"\n"
"default config: ${XDG_CONFIG_HOME:-~/.config}/swov/config\n"
"keys: ssaa icons icon_px shadow shadow_layers vsync ui_scale ws_px label_px\n"
"      title_px hint_px font font_bold cols rows margin gap pad win_gap\n"
"      screen_pad radius border show_empty all_outputs header_pos hints_pos\n"
"      start_selection quit_on_focus_loss quit_after_action\n"
"      colors: bg tile tile_sel tile_hover mini_bg card card_hover card_focus\n"
"              hl text subtext dim accent hltext hint urgent outline\n"
"\n"
"keys:\n"
"  enter / click       focus the window under the cursor and leave\n"
"  click on a tile     switch to that workspace\n"
"  arrows or hjkl      move the selection; it walks through tile borders\n"
"  tab / shift+tab     previous / next workspace\n"
"  ctrl+tab            one row down in the grid, with shift one row up\n"
"  w                   switch between window and whole-workspace selection\n"
"  space / right click mark or unmark the window under the cursor\n"
"  shift+space, a      mark or unmark every window of the workspace\n"

"  c                   clear all marks\n"
"  0-9                 switch to that workspace and leave\n"
"  ctrl+0-9            move marked or selected windows to that workspace;\n"
"                      with a whole workspace selected it moves everything\n"
"                      there, keeping the container layout intact\n"

"  x / delete          close marked or selected windows, enter confirms\n"
"  f                   find windows by app id, title or workspace name\n"
"  /                   filter: same search, but hides everything else\n"
"  r                   reload the tree\n"
"  esc / q             quit\n");
}

static bool create_target(void)
{
    if (TARGET) { SDL_DestroyTexture(TARGET); TARGET = NULL; }

    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(WIN_HANDLE, &pw, &ph);
    if (pw <= 0 || ph <= 0) return false;

    int ssaa = C.ssaa;
    while (ssaa > 1 && (long long)pw * ph * ssaa * ssaa > 34000000LL) ssaa--;

    if (ssaa > 1) {
        TARGET = SDL_CreateTexture(REN, SDL_PIXELFORMAT_RGBA32,
                                   SDL_TEXTUREACCESS_TARGET, pw * ssaa, ph * ssaa);
        if (TARGET) {
            SDL_SetTextureScaleMode(TARGET, SDL_SCALEMODE_LINEAR);
            SDL_SetTextureBlendMode(TARGET, SDL_BLENDMODE_BLEND);
        }
    }
    SC = TARGET ? (float)ssaa : 1.0f;
    RW = TARGET ? pw * ssaa : pw;
    RH = TARGET ? ph * ssaa : ph;

    int ww = 0, wh = 0;
    SDL_GetWindowSize(WIN_HANDLE, &ww, &wh);
    MOUSE_SCALE = (ww > 0) ? (float)RW / (float)ww : 1.0f;
    return true;
}

static void goto_workspace_number(int num)
{
    for (int i = 0; i < NWS; ++i)
        if (WSS[i].num == num) { act_goto_workspace(&WSS[i]); return; }

    sway_cmd("workspace number %d", num);           /* it does not exist yet */
    if (C.track) {
        char name[16];
        snprintf(name, sizeof(name), "%d", num);
        usage_switch(name);
    }
    running = false;
}

static int digit_from_scancode(SDL_Scancode sc)
{
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return (int)(sc - SDL_SCANCODE_1) + 1;
    if (sc == SDL_SCANCODE_0) return 0;
    return -1;
}

static void begin_edit(int idx);
static void end_edit(bool commit);

static void handle_key(const SDL_KeyboardEvent *k)
{
    bool shift = (k->mod & SDL_KMOD_SHIFT) != 0;
    bool was_active = sel_active;
    sel_active = true;
    SDL_Keycode key = k->key;

    if (editing) {
        switch (key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: end_edit(true);  reload_model(); return;
        case SDLK_ESCAPE:   end_edit(false); return;
        case SDLK_BACKSPACE:
            while (edit_len > 0) {
                unsigned char c = (unsigned char)edit_buf[--edit_len];
                edit_buf[edit_len] = 0;
                if ((c & 0xc0) != 0x80) break;
            }
            return;
        default: return;
        }
    }

    if (editing) {
        switch (key) {
        case SDLK_RETURN:
        case SDLK_KP_ENTER: end_edit(true);  reload_model(); return;
        case SDLK_ESCAPE:   end_edit(false); return;
        case SDLK_BACKSPACE:
            while (edit_len > 0) {
                unsigned char c = (unsigned char)edit_buf[--edit_len];
                edit_buf[edit_len] = 0;
                if ((c & 0xc0) != 0x80) break;
            }
            return;
        default: return;
        }
    }

    if (confirm_kill) {                            /* enter confirms, anything
                                                    * else calls it off */
        confirm_kill = false;
        if (key == SDLK_RETURN || key == SDLK_KP_ENTER) act_close();
        rebuild_chrome();
        return;
    }

    if (query_active()) {
        switch (key) {
        case SDLK_ESCAPE:
            filtering = searching = false;
            query[0] = 0;
            qlen = 0;
            apply_filter();
            rebuild_chrome();
            return;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            if (NWS > 0 && WSS[sel_ws].count > 0) act_focus_window(ws_sel_win(&WSS[sel_ws]));
            else running = false;
            return;
        case SDLK_BACKSPACE:
            while (qlen > 0) {                     /* drop one code point */
                unsigned char c = (unsigned char)query[--qlen];
                query[qlen] = 0;
                if ((c & 0xc0) != 0x80) break;
            }
            apply_filter();
            rebuild_chrome();
            return;
        case SDLK_TAB:
            if (k->mod & SDL_KMOD_CTRL) step_ws_row(shift ? -1 : 1);
            else                        step_ws(shift ? -1 : 1);
            return;
        case SDLK_LEFT:  navigate(-1, 0); return;
        case SDLK_RIGHT: navigate( 1, 0); return;
        case SDLK_UP:    navigate(0, -1); return;
        case SDLK_DOWN:  navigate(0,  1); return;
        default: return;
        }
    }

    /* A digit only counts when it is pressed bare, or with ctrl. With shift
     * held the layout may well be producing something else entirely — on a
     * German keyboard shift+7 is "/" — and that has to reach text input. */
    int digit = digit_from_scancode(k->scancode);
    if (digit >= 0 && !shift && !(k->mod & SDL_KMOD_ALT)) {
        if (k->mod & SDL_KMOD_CTRL) {
            if (was_active) act_move_to_workspace(digit);   /* ctrl: take it there */
        } else {
            goto_workspace_number(digit);                   /* bare: go there */
        }
        return;
    }
    if (digit >= 0) return;                    /* shifted digit: let text input have it */

    switch (key) {
    case SDLK_ESCAPE:
        if (drag_active) { drag_cancel(); break; }
        running = false;
        break;
    case SDLK_Q:
        running = false;
        break;

    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        if (NWS == 0) { running = false; break; }
        if (ws_sel_win(&WSS[sel_ws])) act_focus_window(ws_sel_win(&WSS[sel_ws]));
        else                          act_goto_workspace(&WSS[sel_ws]);
        break;

    case SDLK_TAB:
        if (k->mod & SDL_KMOD_CTRL) step_ws_row(shift ? -1 : 1);
        else                        step_ws(shift ? -1 : 1);
        break;

    case SDLK_W:                                   /* window level <-> workspace */
        if (NWS > 0) {
            Ws *ws = &WSS[sel_ws];
            if (ws->sel >= 0) ws->sel = -1;
            else              ws->sel = ws_first_visible(ws);
        }
        break;

    case SDLK_LEFT:  case SDLK_H: navigate(-1, 0); break;
    case SDLK_RIGHT: case SDLK_L: navigate( 1, 0); break;
    case SDLK_UP:    case SDLK_K: navigate(0, -1); break;
    case SDLK_DOWN:  case SDLK_J: navigate(0,  1); break;

    case SDLK_SPACE:
        if (NWS > 0) {
            Ws *ws = &WSS[sel_ws];
            if (!shift && ws->sel >= 0) act_toggle_mark(ws_sel_win(ws));
            else                        toggle_ws_marks(ws);
        }
        break;

    case SDLK_A:
        if (NWS > 0) toggle_ws_marks(&WSS[sel_ws]);
        break;

    case SDLK_C:
        for (int i = 0; i < NWIN; ++i) WINS[i].marked = false;
        break;

    case SDLK_X:
    case SDLK_DELETE:
        if (NWS > 0) {                             /* ask before killing */
            int ids[MAX_WINDOWS];
            if (gather_targets(ids, MAX_WINDOWS) > 0 ||
                (WSS[sel_ws].sel < 0 && WSS[sel_ws].count > 0)) {
                confirm_kill = true;
                rebuild_chrome();
            }
        }
        break;

    case SDLK_R:
        reload_model();
        break;

    case SDLK_SLASH:
        filtering = true;
        searching = false;
        query[0] = 0;
        qlen = 0;
        swallow_next_text = true;
        rebuild_chrome();
        break;

    case SDLK_F:                                   /* find, without hiding */
        searching = true;
        filtering = false;
        query[0] = 0;
        qlen = 0;
        swallow_next_text = true;
        rebuild_chrome();
        break;

    default:
        break;
    }
}

static void begin_edit(int idx)
{
    editing = true;
    edit_ws = idx;
    snprintf(edit_buf, sizeof(edit_buf), "%s", WSS[idx].label);
    edit_len = (int)strlen(edit_buf);
    swallow_next_text = false;
}

static void end_edit(bool commit)
{
    if (commit && edit_ws >= 0 && edit_ws < NWS) {
        Ws *ws = &WSS[edit_ws];
        char *to = edit_buf[0] ? (ws->num >= 0 ? fmt_alloc("%d:%s", ws->num, edit_buf)
                                               : xstrdup(edit_buf))
                               : (ws->num >= 0 ? fmt_alloc("%d", ws->num)
                                               : xstrdup(ws->name));
        if (strcmp(to, ws->name) != 0) ws_rename(ws, to);
        free(to);
    }
    editing = false;
    edit_ws = -1;
    edit_buf[0] = 0;
    edit_len = 0;
}

static void handle_mouse_press(const SDL_MouseButtonEvent *b)
{
    float mx = b->x * MOUSE_SCALE, my = b->y * MOUSE_SCALE;
    int ws_idx = -1;
    int win_idx = hit_test(mx, my, &ws_idx);

    if (editing) end_edit(true);                   /* a click elsewhere commits */

    if (b->button == SDL_BUTTON_LEFT && ws_idx >= 0 && win_idx < 0) {
        SDL_FRect tb = WSS[ws_idx].title_box;
        if (mx >= tb.x && mx < tb.x + tb.w && my >= tb.y && my < tb.y + tb.h) {
            sel_active = true;
            sel_ws = ws_idx;
            WSS[ws_idx].sel = -1;
            begin_edit(ws_idx);
            return;
        }
    }

    if (ws_idx < 0) {                       /* the empty background */
        if (b->button == SDL_BUTTON_LEFT) running = false;
        return;
    }

    sel_active = true;
    sel_ws = ws_idx;
    WSS[ws_idx].sel = (win_idx >= 0) ? win_idx - WSS[ws_idx].first : -1;

    switch (b->button) {
    case SDL_BUTTON_LEFT:                   /* arm a drag, act on release */
        press_down = true;
        press_x = mx;
        press_y = my;
        press_win = win_idx;
        press_ws  = ws_idx;
        break;
    case SDL_BUTTON_RIGHT:
        if (win_idx >= 0) act_toggle_mark(&WINS[win_idx]);
        break;
    case SDL_BUTTON_MIDDLE:
        if (win_idx >= 0) {
            sway_cmd("[con_id=%d] kill", WINS[win_idx].con_id);
            reload_model();
        }
        break;
    default: break;
    }
}

static void handle_mouse_release(const SDL_MouseButtonEvent *b)
{
    if (b->button != SDL_BUTTON_LEFT) return;

    float mx = b->x * MOUSE_SCALE, my = b->y * MOUSE_SCALE;

    if (drag_active) {
        drag_update_target(mx, my);
        drag_finish();
        press_down = false;
        return;
    }
    if (!press_down) return;
    press_down = false;

    /* a plain click: only acts when press and release landed on the same
     * window, or on the same workspace */
    int ws_idx = -1;
    int win_idx = hit_test(mx, my, &ws_idx);
    if (ws_idx < 0 || ws_idx != press_ws) { press_win = press_ws = -1; return; }

    if (win_idx >= 0 && win_idx == press_win) act_focus_window(&WINS[win_idx]);
    else if (win_idx < 0 && press_win < 0)    act_goto_workspace(&WSS[ws_idx]);

    press_win = press_ws = -1;
}

static void handle_mouse_motion(const SDL_MouseMotionEvent *mo)
{
    float mx = mo->x * MOUSE_SCALE, my = mo->y * MOUSE_SCALE;
    drag_x = mx;
    drag_y = my;

    if (press_down && !drag_active) {
        float dx = mx - press_x, dy = my - press_y;
        if (dx * dx + dy * dy > (7.0f * SC) * (7.0f * SC)) {
            drag_active = true;
            hov_ws = hov_win = -1;               /* hover is meaningless now */
            drag_src_tile = (SDL_FRect){ 0, 0, 0, 0 };
            if (press_win >= 0 && WINS[press_win].ws >= 0 && WINS[press_win].ws < NWS)
                drag_src_tile = WSS[WINS[press_win].ws].tile;
            if (press_win < 0 && press_ws >= 0 && WSS[press_ws].num >= 0)
                drag_ws_mode = true;         /* dragging the whole workspace */
            update_ghosts(mx, my);
            SDL_SetCursor(CUR_HAND);
        }
    }

    if (drag_active) {
        drag_update_target(mx, my);
        dirty = true;
        return;
    }

    int ws_idx = -1;
    int win_idx = hit_test(mx, my, &ws_idx);
    if (ws_idx == hov_ws && win_idx == hov_win) return;

    hov_ws = ws_idx;
    hov_win = win_idx;
    SDL_SetCursor(ws_idx >= 0 ? CUR_HAND : CUR_ARROW);
    dirty = true;

    /* Pointing at something selects it, so space, x, ctrl+digit and the rest
     * act on whatever is under the pointer, exactly as they do on whatever
     * the arrow keys picked. */
    if (ws_idx >= 0) {
        sel_active = true;
        sel_ws = ws_idx;
        WSS[ws_idx].sel = (win_idx >= 0) ? win_idx - WSS[ws_idx].first : -1;
    }
}

static void handle_event(const SDL_Event *e)
{
    /* Mouse motion fires constantly; it only earns a redraw when it changes
     * what is under the cursor. Everything else is a real state change. */
    if (e->type != SDL_EVENT_MOUSE_MOTION) dirty = true;

    switch (e->type) {
    case SDL_EVENT_QUIT:
        running = false;
        break;

    case SDL_EVENT_KEY_DOWN:
        handle_key(&e->key);
        break;

    case SDL_EVENT_TEXT_INPUT:
        /* the key that opened a mode sends its own character right after */
        if (swallow_next_text) { swallow_next_text = false; break; }

        if (editing) {                             /* renaming a workspace */
            size_t add = strlen(e->text.text);
            if (edit_len + (int)add < (int)sizeof(edit_buf) - 1) {
                memcpy(edit_buf + edit_len, e->text.text, add + 1);
                edit_len += (int)add;
            }
            break;
        }

        if (!query_active()) {                     /* "/" opens the filter */
            if (strcmp(e->text.text, "/") == 0) {
                filtering = true;
                query[0] = 0;
                qlen = 0;
                rebuild_chrome();
            }
            break;
        }

        {
            size_t add = strlen(e->text.text);
            if (qlen + (int)add < (int)sizeof(query) - 1) {
                memcpy(query + qlen, e->text.text, add + 1);
                qlen += (int)add;
                apply_filter();
                rebuild_chrome();
            }
        }
        break;

    case SDL_EVENT_MOUSE_MOTION:
        handle_mouse_motion(&e->motion);
        break;

    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        handle_mouse_press(&e->button);
        break;

    case SDL_EVENT_MOUSE_BUTTON_UP:
        handle_mouse_release(&e->button);
        break;

    case SDL_EVENT_MOUSE_WHEEL:
        if (e->wheel.y != 0.0f) step_ws(e->wheel.y > 0.0f ? -1 : 1);
        break;

    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_RESIZED:
        if (create_target()) { layout(); rebuild_chrome(); }
        break;

    case SDL_EVENT_WINDOW_FOCUS_LOST:
        if (C.quit_on_focus_loss) running = false;
        break;

    default:
        break;
    }
}

static void present(void)
{
    if (TARGET) SDL_SetRenderTarget(REN, TARGET);
    render();
    if (TARGET) {
        SDL_SetRenderTarget(REN, NULL);
        SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_NONE);
        SDL_SetRenderDrawColorFloat(REN, 0, 0, 0, 0);
        SDL_RenderClear(REN);
        SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_BLEND);
        SDL_RenderTexture(REN, TARGET, NULL, NULL);
    }

    if (SHOT_PATH) {
        SDL_Surface *shot = SDL_RenderReadPixels(REN, NULL);
        if (shot) {
            if (!IMG_SavePNG(shot, SHOT_PATH))
                fprintf(stderr, "swov: cannot write %s: %s\n", SHOT_PATH, SDL_GetError());
            SDL_DestroySurface(shot);
        }
        running = false;
    }
    SDL_RenderPresent(REN);
}

/* place the overlay on the output sway is using, fall back to the pointer */
static void pick_bounds(SDL_Rect *out)
{
    *out = (SDL_Rect){ 0, 0, 1280, 720 };

    int count = 0;
    SDL_DisplayID *ids = SDL_GetDisplays(&count);
    if (!ids || count <= 0) { SDL_free(ids); return; }

    SDL_DisplayID chosen = 0;
    if (FOCUSED_OUTPUT[0]) {
        for (int i = 0; i < count; ++i) {
            const char *n = SDL_GetDisplayName(ids[i]);
            if (n && strcmp(n, FOCUSED_OUTPUT) == 0) { chosen = ids[i]; break; }
        }
    }
    if (!chosen) {
        float mx = 0.0f, my = 0.0f;
        SDL_GetGlobalMouseState(&mx, &my);
        chosen = ids[0];
        for (int i = 0; i < count; ++i) {
            SDL_Rect b;
            if (!SDL_GetDisplayBounds(ids[i], &b)) continue;
            if (mx >= (float)b.x && mx < (float)(b.x + b.w) &&
                my >= (float)b.y && my < (float)(b.y + b.h)) { chosen = ids[i]; break; }
        }
    }
    SDL_GetDisplayBounds(chosen, out);
    SDL_free(ids);
}

int main(int argc, char **argv)
{
    C = cfg_defaults();

    char *cfg_path = NULL;
    bool  use_cfg = true;
    const char *go_name = NULL;
    bool  go_back = false;
    bool  show_usage_stats = false;
    bool  show_info = false;
    const char *sets[64];
    int nsets = 0;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if ((!strcmp(a, "-g") || !strcmp(a, "--go")) && i + 1 < argc) go_name = argv[++i];
        else if (!strcmp(a, "-b") || !strcmp(a, "--back")) go_back = true;
        else if (!strcmp(a, "--usage")) show_usage_stats = true;
        else if (!strcmp(a, "--info")) show_info = true;
        else if ((!strcmp(a, "-c") || !strcmp(a, "--config")) && i + 1 < argc)
            cfg_path = expand_tilde(argv[++i]);
        else if (!strcmp(a, "-n") || !strcmp(a, "--no-config")) use_cfg = false;
        else if (!strcmp(a, "--shot") && i + 1 < argc) SHOT_PATH = argv[++i];
        else if (!strcmp(a, "--mouse") && i + 1 < argc) {
            float fx = 0.0f, fy = 0.0f;
            if (sscanf(argv[++i], "%f,%f", &fx, &fy) == 2) { SHOT_MX = fx; SHOT_MY = fy; }
        }
        else if ((!strcmp(a, "-s") || !strcmp(a, "--set")) && i + 1 < argc && nsets < 64)
            sets[nsets++] = argv[++i];
        else if (strncmp(a, "--", 2) == 0 && strchr(a, '=')) {
            if (nsets < 64) sets[nsets++] = a + 2;      /* --ui_scale=1.2 */
        }
        else if (a[0] != '-' && strchr(a, '=')) {
            if (nsets < 64) sets[nsets++] = a;          /* ui_scale=1.2 */
        }
        else { fprintf(stderr, "swov: unknown argument '%s'\n", a); usage(); return 2; }
    }

    if (use_cfg) {
        bool named = cfg_path != NULL;
        if (!cfg_path) cfg_path = default_config_path();
        if (cfg_path) {
            snprintf(CFG_PATH, sizeof(CFG_PATH), "%s", cfg_path);
            CFG_LOADED = cfg_load_file(&C, cfg_path);
            if (!CFG_LOADED && named) fprintf(stderr, "swov: no config at %s\n", cfg_path);
        }
    }
    free(cfg_path);

    for (int i = 0; i < nsets; ++i) {               /* -s key=value */
        char *copy = xstrdup(sets[i]);
        char *eq = strchr(copy, '=');
        if (eq) { *eq = 0; cfg_set(&C, str_trim(copy), str_trim(eq + 1)); }
        free(copy);
    }

    if (show_info) {
        char exe[PATH_MAX] = {0};
        ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
        if (n > 0) exe[n] = 0;

        char *upath = usage_path();
        usage_load();

        printf("swov %s\n\n", SWOV_VERSION);
        printf("binary        %s\n", exe[0] ? exe : "(unknown)");
        printf("config        %s%s\n", CFG_PATH[0] ? CFG_PATH : "(none)",
               CFG_PATH[0] ? (CFG_LOADED ? "  [loaded]" : "  [missing, defaults in use]") : "");
        printf("usage file    %s\n", upath ? upath : "(none)");
        printf("sway socket   %s\n", getenv("SWAYSOCK") ? getenv("SWAYSOCK") : "(unset)");

        char *theme = icon_theme_name();
        printf("icon theme    %s\n", theme);
        free(theme);

        fontconfig_resolve(C.font);
        printf("font          %s\n", FC_REGULAR ? FC_REGULAR : (C.font[0] ? C.font : "(none)"));
        printf("font bold     %s\n", FC_BOLD ? FC_BOLD : "(synthetic)");
        free(FC_REGULAR);
        free(FC_BOLD);
        FC_REGULAR = FC_BOLD = NULL;

        printf("\nrecorded usage\n");
        if (USAGE_CUR[0])
            printf("  on %s, for %.0fs\n", USAGE_CUR, usage_pending());
        if (NUSAGE == 0) printf("  (nothing yet)\n");
        for (int i = 0; i < NUSAGE; ++i) {
            double secs = USAGE[i].secs +
                          (strcmp(USAGE[i].name, USAGE_CUR) == 0 ? usage_pending() : 0.0);
            printf("  %-20s %7.0fs\n", USAGE[i].name, secs);
        }

        printf("\nsettings\n");
        printf("  ssaa %d   icons %d   icon_px %d   ui_scale %.2f\n",
               C.ssaa, C.icons, C.icon_px, (double)C.ui_scale);
        printf("  ws_px %d   label_px %d   title_px %d   hint_px %d\n",
               C.ws_px, C.label_px, C.title_px, C.hint_px);
        printf("  cols %d   rows %d   margin %.0f   gap %.0f   pad %.0f\n",
               C.cols, C.rows, (double)C.margin, (double)C.gap, (double)C.pad);
        printf("  win_gap %.0f   screen_pad %.0f   radius %.0f   border %.0f\n",
               (double)C.win_gap, (double)C.screen_pad, (double)C.radius, (double)C.border);
        printf("  show_empty %d   all_outputs %d   start_selection %d\n",
               C.show_empty, C.all_outputs, C.start_selection);
        printf("  header_pos %s   hints_pos %s\n", C.header_pos, C.hints_pos);
        printf("  track %d   usage_dots %d   dot_count %d   dot_px %.0f\n",
               C.track, C.usage_dots, C.dot_count, (double)C.dot_px);
        printf("  anim_ms %.0f   shadow %d   float_alpha %.2f   vsync %d\n",
               (double)C.anim_ms, C.shadow, (double)C.float_alpha, C.vsync);

        printf("\ncolours\n");
        struct { const char *k; SDL_FColor c; } cols[] = {
            { "bg", C.bg }, { "tile", C.tile }, { "tile_sel", C.tile_sel },
            { "mini_bg", C.mini_bg }, { "card", C.card }, { "card_hover", C.card_hover },
            { "card_focus", C.card_focus }, { "hl", C.hl }, { "current", C.current },
            { "match", C.match }, { "text", C.text }, { "subtext", C.subtext },
            { "dim", C.dim }, { "accent", C.accent }, { "hint", C.hint },
            { "hltext", C.hltext }, { "urgent", C.urgent }, { "outline", C.outline }
        };
        for (size_t i = 0; i < SDL_arraysize(cols); ++i)
            printf("  %-12s %02x%02x%02x%02x\n", cols[i].k,
                   (unsigned)(cols[i].c.r * 255.0f + 0.5f), (unsigned)(cols[i].c.g * 255.0f + 0.5f),
                   (unsigned)(cols[i].c.b * 255.0f + 0.5f), (unsigned)(cols[i].c.a * 255.0f + 0.5f));

        free(upath);
        return 0;
    }

    if (show_usage_stats) {
        char *path = usage_path();
        usage_load();
        printf("usage file: %s\n", path ? path : "(none)");
        if (USAGE_CUR[0]) printf("on:         %s, for %.0fs\n", USAGE_CUR, usage_pending());
        else              printf("on:         nothing recorded yet\n");
        for (int i = 0; i < NUSAGE; ++i) {
            double secs = USAGE[i].secs +
                          (strcmp(USAGE[i].name, USAGE_CUR) == 0 ? usage_pending() : 0.0);
            int mins = (int)(secs / 60.0);
            printf("  %-20s %6.0fs  (%dh %02dm)\n", USAGE[i].name, secs, mins / 60, mins % 60);
        }
        free(path);
        return 0;
    }

    sway_fd = sway_connect();
    if (sway_fd < 0) die("cannot reach sway (is SWAYSOCK set?)");

    /* Switching does not need a window, a renderer or a font: connect, say it,
     * leave. This is the path a keybinding should use. */
    if (go_back || go_name) {
        bool ok;
        if (go_back) {
            ok = sway_cmd("workspace back_and_forth");
        } else if (str_all_digits(go_name)) {
            ok = sway_cmd("workspace number %d", atoi(go_name));
        } else {
            char *e = escape_arg(go_name);         /* a name, "code" or "3:code" */
            ok = sway_cmd("workspace --no-auto-back-and-forth \"%s\"", e);
            free(e);
        }
        if (ok && C.track) {                       /* stamp the new workspace */
            char now[64];
            focused_workspace(now, sizeof(now));
            usage_switch(now);
        }
        close(sway_fd);
        return ok ? 0 : 1;
    }

    /* the focused output is needed before the window is created */
    JV *wsr = sway_query(IPC_GET_WORKSPACES);
    if (wsr && wsr->type == J_ARR)
        for (int i = 0; i < wsr->count; ++i)
            if (jbool(wsr->items[i], "focused", false))
                snprintf(FOCUSED_OUTPUT, sizeof(FOCUSED_OUTPUT), "%s",
                         jstr(wsr->items[i], "output", ""));
    jfree(wsr);

    SDL_SetHint(SDL_HINT_APP_ID, APP_ID);
    SDL_SetAppMetadata("swov", SWOV_VERSION, "org.swov.overview");

    if (!SDL_Init(SDL_INIT_VIDEO)) die("SDL_Init: %s", SDL_GetError());
    if (!TTF_Init()) die("TTF_Init: %s", SDL_GetError());

    SDL_Rect bounds;
    pick_bounds(&bounds);

    SDL_WindowFlags flags = SDL_WINDOW_BORDERLESS | SDL_WINDOW_TRANSPARENT |
                            SDL_WINDOW_ALWAYS_ON_TOP | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    WIN_HANDLE = SDL_CreateWindow("swov", bounds.w, bounds.h, flags);
    if (!WIN_HANDLE) die("SDL_CreateWindow: %s", SDL_GetError());
    SDL_SetWindowPosition(WIN_HANDLE, bounds.x, bounds.y);

    REN = SDL_CreateRenderer(WIN_HANDLE, NULL);
    if (!REN) die("SDL_CreateRenderer: %s", SDL_GetError());
    SDL_SetRenderDrawBlendMode(REN, SDL_BLENDMODE_BLEND);
    SDL_SetRenderVSync(REN, C.vsync ? 1 : 0);

    if (!create_target()) die("cannot size the window");
    load_fonts();

    CUR_ARROW = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    CUR_HAND  = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_POINTER);

    sway_subscribe_events();
    if (!model_reload()) die("could not read the sway tree");
    select_current_workspace();

    sel_active = C.start_selection != 0;
    if (C.start_selection == 2 && NWS > 0) WSS[sel_ws].sel = ws_first_visible(&WSS[sel_ws]);
    layout();
    apply_filter();
    rebuild_chrome();

    if (SHOT_MX >= 0.0f) {                          /* pretend the pointer is there */
        hov_win = hit_test(SHOT_MX * MOUSE_SCALE, SHOT_MY * MOUSE_SCALE, &hov_ws);
    }

    SDL_StartTextInput(WIN_HANDLE);
    SDL_RaiseWindow(WIN_HANDLE);
    present();

    while (running) {
        SDL_Event e;
        if (anim_running()) dirty = true;
        if (SDL_WaitEventTimeout(&e, anim_running() ? 8 : 60)) {
            handle_event(&e);
            while (running && SDL_PollEvent(&e)) handle_event(&e);   /* coalesce */
        }

        /* sway acknowledges a command before the layout transaction has
         * committed, so the tree is only trustworthy once the events for it
         * have arrived. This also keeps the overview live. */
        if (running && !drag_active && sway_events_pending()) {
            reload_model();
            dirty = true;
        }
        if (running && dirty) { present(); dirty = false; }
    }

    /* cleanup */
    tex_free(&T_HEADER);
    tex_free(&T_HINTS);
    tex_free(&T_QUERY);
    model_free();
    icons_free();

    if (CUR_ARROW) SDL_DestroyCursor(CUR_ARROW);
    if (CUR_HAND)  SDL_DestroyCursor(CUR_HAND);
    if (TARGET)    SDL_DestroyTexture(TARGET);
    if (F_BADGE)   TTF_CloseFont(F_BADGE);
    if (F_LABEL)   TTF_CloseFont(F_LABEL);
    if (F_TITLE)   TTF_CloseFont(F_TITLE);
    if (F_HINT)    TTF_CloseFont(F_HINT);
    TTF_Quit();
    SDL_DestroyRenderer(REN);
    SDL_DestroyWindow(WIN_HANDLE);
    SDL_Quit();
    if (sway_fd >= 0) close(sway_fd);
    if (sway_evt_fd >= 0) close(sway_evt_fd);
    return 0;
}
