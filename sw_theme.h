/* sw_theme.h — one look for swov, appwheel and swbr.
 *
 *     ${XDG_CONFIG_HOME:-~/.config}/sw/config
 *
 * key=value, '#' comments, same as every other config here. Keys written
 * before any [section] go to all three programs; keys inside [swov],
 * [appwheel] or [swbr] go to that one only.
 *
 * Outside a section only *role* names are understood (surface, accent, hl,
 * ...). Each program translates them to its own keys, and roles it has no use
 * for are dropped. Inside a section every key of that program works as well.
 *
 * Order: built-in defaults -> this file -> the program's own config -> the
 * command line. The specific always wins over the shared.
 *
 * Drop-in header, no dependencies. Each program calls sw_shared_apply() with
 * its own setter, right before it reads its own config file.
 */
#ifndef SW_THEME_H
#define SW_THEME_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define SW_MAX_PAIRS 256
#define SW_KEYLEN     48
#define SW_VALLEN    256
#define SW_MAXNATIVE   4

/* ------------------------------------------------------------ role table
 * NULL = this program has nothing for that role. Several native keys are
 * separated by commas: swbr paints its focused workspace button with `hl`.
 */
typedef struct {
    const char *role, *swov, *appwheel, *swbr;
} SwRole;

static const SwRole SW_ROLES[] = {
    /* role          swov            appwheel   swbr                           */
    { "bg",          "bg",           "bg",      NULL                           },
    { "surface",     "tile",         "ring",    "bg"                           },
    { "surface_alt", "tile_sel",     "ring2",   NULL                           },
    { "surface_hi",  "tile_hover",   NULL,      "ws_visible_bg"                },
    { "inset",       "mini_bg",      "center",  NULL                           },
    { "card",        "card",         NULL,      "ws_bg"                        },
    { "card_hi",     "card_hover",   NULL,      NULL                           },
    { "card_focus",  "card_focus",   NULL,      NULL                           },
    { "text",        "text",         "text",    "text"                         },
    { "subtext",     "subtext",      NULL,      "ws_fg,ws_visible_fg"          },
    { "dim",         "dim",          "dim",     "dim"                          },
    { "hint",        "hint",         NULL,      NULL                           },
    { "accent",      "accent",       "accent",  "accent,msg_info"              },
    { "hl",          "hl",           "hl",      "hl,ws_focused_bg,mode_bg"     },
    { "hltext",      "hltext",       "hltext",  "ws_focused_fg,mode_fg,ws_urgent_fg" },
    { "current",     "current",      NULL,      NULL                           },
    { "match",       "match",        NULL,      NULL                           },
    { "warn",        NULL,           NULL,      "msg_warn"                     },
    { "urgent",      "urgent",       NULL,      "urgent,ws_urgent_bg,msg_error" },
    { "ok",          NULL,           NULL,      "running"                      },
    { "outline",     "outline",      NULL,      "outline,separator_color"      },
    { "shadow",      "shadow_color", NULL,      NULL                           },

    { "font",          "font",      "font",     "font"     },
    { "font_bold",     "font_bold", NULL,       NULL       },
    { "ui_scale",      "ui_scale",  "ui_scale", "ui_scale" },
    { "anim_ms",       "anim_ms",   "anim_ms",  "anim_ms"  },
    { "corner_radius", "radius",    NULL,       "radius"   },
    { "ssaa",          "ssaa",      "ssaa",     NULL       },
    { "icons",         "icons",     "icons",    NULL       },
    { "icon_px",       "icon_px",   "icon_px",  NULL       },
};

/* ------------------------------------------------------------------ path */

static const char *sw_shared_path(void)
{
    static char path[512];
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)       snprintf(path, sizeof path, "%s/sw/config", xdg);
    else if (home && *home) snprintf(path, sizeof path, "%s/.config/sw/config", home);
    else return NULL;
    return path;
}

/* --------------------------------------------------------------- mapping */

static int sw_ci_eq(const char *a, const char *b)
{
    for (; *a && *b; ++a, ++b)
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return 0;
    return *a == *b;
}

static const char *sw_role_for(const SwRole *r, const char *app)
{
    if (sw_ci_eq(app, "swov"))     return r->swov;
    if (sw_ci_eq(app, "appwheel")) return r->appwheel;
    if (sw_ci_eq(app, "swbr"))     return r->swbr;
    return NULL;
}

/* Translate one shared key into the program's own keys.
 * `scoped` means the key came from a [section], where the program's own key
 * names are allowed too. Returns how many names were written. */
static int sw_map(const char *app, const char *key, int scoped,
                  char out[][SW_KEYLEN], int max)
{
    for (size_t i = 0; i < sizeof SW_ROLES / sizeof *SW_ROLES; ++i) {
        if (!sw_ci_eq(SW_ROLES[i].role, key)) continue;
        const char *nat = sw_role_for(&SW_ROLES[i], app);
        if (!nat) return 0;                       /* not this program's business */
        int n = 0;
        while (*nat && n < max) {
            const char *comma = strchr(nat, ',');
            size_t len = comma ? (size_t)(comma - nat) : strlen(nat);
            if (len >= SW_KEYLEN) len = SW_KEYLEN - 1;
            memcpy(out[n], nat, len);
            out[n][len] = '\0';
            ++n;
            if (!comma) break;
            nat = comma + 1;
        }
        return n;
    }
    if (!scoped) return 0;                        /* global: roles only */
    snprintf(out[0], SW_KEYLEN, "%s", key);       /* [section]: pass through */
    return 1;
}

/* ---------------------------------------------------------------- reader */

static char *sw_trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') ++s;
    char *e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    *e = '\0';
    return s;
}

/* Read the shared file and hand every setting that concerns `app` to `set`,
 * in file order, so a later line overrides an earlier one. Returns 1 if the
 * file was there. */
static int sw_shared_apply(const char *app,
                           void (*set)(void *ud, const char *key, const char *val),
                           void *ud)
{
    const char *path = sw_shared_path();
    if (!path) return 0;
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char line[1024], section[SW_KEYLEN] = "";
    int  mine = 1;                                /* before any section: all */

    while (fgets(line, sizeof line, f)) {
        char *s = sw_trim(line);
        if (!*s || *s == '#' || *s == ';') continue;

        if (*s == '[') {                          /* [swov] */
            char *end = strchr(s, ']');
            if (!end) continue;
            *end = '\0';
            snprintf(section, sizeof section, "%s", sw_trim(s + 1));
            mine = sw_ci_eq(section, app) || sw_ci_eq(section, "all") ||
                   sw_ci_eq(section, "global");
            if (sw_ci_eq(section, "all") || sw_ci_eq(section, "global")) section[0] = '\0';
            continue;
        }
        if (!mine) continue;

        char *eq = strchr(s, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = sw_trim(s), *v = sw_trim(eq + 1);
        char *hash = strchr(v, '#');              /* trailing comment */
        if (hash && hash > v && (hash[-1] == ' ' || hash[-1] == '\t')) {
            *hash = '\0';
            v = sw_trim(v);
        }
        if (!*k) continue;

        char native[SW_MAXNATIVE][SW_KEYLEN];
        int n = sw_map(app, k, section[0] != '\0', native, SW_MAXNATIVE);
        for (int i = 0; i < n; ++i) set(ud, native[i], v);
    }
    fclose(f);
    return 1;
}

#endif /* SW_THEME_H */
