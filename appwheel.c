/*
 * appwheel — a GTA-V-style radial (weapon-wheel) app launcher for Wayland & X11.
 *
 * Dependencies:
 *   - SDL3            (the only library you link)
 *   - stb_truetype.h  (vendored single-header — smooth fonts)
 *   - stb_image.h     (vendored single-header — PNG/JPG/BMP icons)
 *   - nanosvg.h + nanosvgrast.h  (vendored single-header — SVG icons)
 * The vendored *.h files just sit next to this source; nothing to install/link.
 *
 * Build:
 *   cc appwheel.c -o appwheel $(pkg-config --cflags --libs sdl3) -lm
 *
 * See --help for every option (all config keys also work on the command line).
 */

/* ---------------------------------------------------------------------------
 * How this file is laid out (top to bottom):
 *   1. small helpers        colors, string trim, hex-color + tilde parsing
 *   2. Config               the struct, defaults, key=value setter, file loader
 *   3. text (Font)          stb_truetype atlas + drawing, with a debug fallback
 *   4. AppList              .desktop parsing, dir scanning, include/exclude/sort
 *   5. icons                Icon= resolution + stb_image / nanosvg (lazy, cached)
 *   6. launching + history  detached exec, most-recently-used log
 *   7. geometry             circle/sector/chevron primitives
 *   8. usage()/dump_config  --help text and the default config generator
 *   9. main()               setup, then the event + render loop
 * ------------------------------------------------------------------------- */

#include <SDL3/SDL.h>

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include "stb_image.h"

#include "sw_theme.h"

/* SVG icons via nanosvg (vendored single-header). Third-party headers, so we
   quiet their warnings without affecting our own -Wall build. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#define NANOSVG_IMPLEMENTATION
#define NANOSVG_ALL_COLOR_KEYWORDS
#include "nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvgrast.h"
#pragma GCC diagnostic pop

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <math.h>
#include <dirent.h>
#include <glob.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <limits.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define DEG (M_PI / 180.0)
#define ICON_AL (120.0f*(float)DEG)   /* bottom-left paging icon  */
#define ICON_AR (60.0f*(float)DEG)    /* bottom-right paging icon */

/* ------------------------------------------------------------------ */
typedef struct { Uint8 r, g, b, a; } Col;
static SDL_FColor tofc(Col c){ SDL_FColor f={c.r/255.0f,c.g/255.0f,c.b/255.0f,c.a/255.0f}; return f; }
static char *xstrdup(const char *s){ char*p=malloc(strlen(s)+1); if(p)strcpy(p,s); return p; }
static char *trim(char *s){
    while(*s && isspace((unsigned char)*s)) s++;
    if(!*s) return s;
    char*e=s+strlen(s)-1; while(e>s && isspace((unsigned char)*e)) *e--='\0';
    return s;
}
static int contains_ci(const char*hay,const char*needle){
    if(!*needle) return 1;
    size_t nl=strlen(needle);
    for(const char*p=hay;*p;p++) if(strncasecmp(p,needle,nl)==0) return 1;
    return 0;
}
static void parse_color(const char*s, Col*out){
    if(*s=='#') s++;
    unsigned r,g,b,a=255;
    if(strlen(s)>=8 && sscanf(s,"%2x%2x%2x%2x",&r,&g,&b,&a)==4){}
    else if(sscanf(s,"%2x%2x%2x",&r,&g,&b)==3){}
    else return;
    out->r=(Uint8)r; out->g=(Uint8)g; out->b=(Uint8)b; out->a=(Uint8)a;
}
static int file_exists(const char*p){ struct stat st; return stat(p,&st)==0 && S_ISREG(st.st_mode); }
static float clampf(float v,float lo,float hi){ return v<lo?lo:(v>hi?hi:v); }
/* expand a leading ~/ to $HOME (no shell involved when we launch) */
static void expand_tilde(const char*in,char*out,size_t n){
    if(in[0]=='~' && (in[1]=='/'||in[1]=='\0')){
        const char*home=getenv("HOME");
        if(home){ snprintf(out,n,"%s%s",home,in+1); return; }
    }
    snprintf(out,n,"%s",in);
}

/* ------------------------------------------------------------------ */
/* config                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    int   width,height,fullscreen,close_on_focus_loss;
    int   slots; float arc_deg; int page_ms;
    float radius, y_offset;           /* wheel size + vertical nudge (of min dim) */
    int   icons, icon_px;
    float ui_scale; int font_px;
    float label_px, title_px, search_px, count_px;   /* per-element text sizes */
    int   ssaa;                       /* full-scene supersample (1..4) */
    int   animate, anim_ms;           /* app-data crossfade on results change */
    int   recent_first;               /* bias the best match toward recently-used apps */
    char  font[PATH_MAX];
    char  sort[16], launcher[16], terminal[128];
    char  history[PATH_MAX], dirs[4096], include[8192], exclude[8192];
    Col bg,ring,ring2,hl,text,hltext,center,accent,dim;
} Config;

static void config_defaults(Config*c){
    memset(c,0,sizeof *c);
    c->width=900; c->height=900; c->fullscreen=1; c->close_on_focus_loss=0;
    c->slots=11; c->arc_deg=240.0f; c->page_ms=110;
    c->radius=0.44f; c->y_offset=0.10f;
    c->icons=1; c->icon_px=46; c->ui_scale=1.0f; c->font_px=50;
    c->label_px=24; c->title_px=25; c->search_px=20; c->count_px=20;
    c->ssaa=2;
    c->animate=1; c->anim_ms=90; c->recent_first=1;
    strcpy(c->sort,"recent"); strcpy(c->launcher,"sh");
    const char*term=getenv("TERMINAL");
    snprintf(c->terminal,sizeof c->terminal,"%s",term?term:"xterm");
    const char*home=getenv("HOME"),*xc=getenv("XDG_CACHE_HOME");
    if(xc&&*xc) snprintf(c->history,sizeof c->history,"%s/appwheel/history",xc);
    else        snprintf(c->history,sizeof c->history,"%s/.cache/appwheel/history",home?home:".");
    c->bg    =(Col){0x0d,0x11,0x17,0x00};   /* transparent by default */
    c->ring  =(Col){0x1e,0x27,0x33,0xf2};
    c->ring2 =(Col){0x26,0x31,0x3f,0xf2};
    c->hl    =(Col){0xcb,0x9b,0x00,0xff};
    c->text  =(Col){0xe8,0xe8,0xe8,0xff};
    c->hltext=(Col){0x14,0x14,0x14,0xff};
    c->center=(Col){0x0d,0x11,0x17,0xe6};
    c->accent=(Col){0x89,0xaf,0xc4,0xff};
    c->dim   =(Col){0x5a,0x6b,0x7a,0xff};
}
static void config_set(Config*c,const char*k,const char*v){
    if(!strcmp(k,"width"))c->width=atoi(v);
    else if(!strcmp(k,"height"))c->height=atoi(v);
    else if(!strcmp(k,"fullscreen"))c->fullscreen=atoi(v);
    else if(!strcmp(k,"close_on_focus_loss"))c->close_on_focus_loss=atoi(v);
    else if(!strcmp(k,"slots"))c->slots=atoi(v);
    else if(!strcmp(k,"arc")||!strcmp(k,"arc_deg"))c->arc_deg=(float)atof(v);
    else if(!strcmp(k,"page_ms"))c->page_ms=atoi(v);
    else if(!strcmp(k,"radius")||!strcmp(k,"size"))c->radius=(float)atof(v);
    else if(!strcmp(k,"y_offset")||!strcmp(k,"y"))c->y_offset=(float)atof(v);
    else if(!strcmp(k,"icons"))c->icons=atoi(v);
    else if(!strcmp(k,"icon_px"))c->icon_px=atoi(v);
    else if(!strcmp(k,"ui_scale")||!strcmp(k,"font_scale")||!strcmp(k,"text_scale"))c->ui_scale=(float)atof(v);
    else if(!strcmp(k,"font_px"))c->font_px=atoi(v);
    else if(!strcmp(k,"label_px"))c->label_px=(float)atof(v);
    else if(!strcmp(k,"title_px"))c->title_px=(float)atof(v);
    else if(!strcmp(k,"search_px")||!strcmp(k,"query_px"))c->search_px=(float)atof(v);
    else if(!strcmp(k,"count_px"))c->count_px=(float)atof(v);
    else if(!strcmp(k,"ssaa")||!strcmp(k,"aa"))c->ssaa=atoi(v);
    else if(!strcmp(k,"animate"))c->animate=atoi(v);
    else if(!strcmp(k,"anim_ms"))c->anim_ms=atoi(v);
    else if(!strcmp(k,"recent_first"))c->recent_first=atoi(v);
    else if(!strcmp(k,"font"))snprintf(c->font,sizeof c->font,"%s",v);
    else if(!strcmp(k,"sort"))snprintf(c->sort,sizeof c->sort,"%s",v);
    else if(!strcmp(k,"launcher"))snprintf(c->launcher,sizeof c->launcher,"%s",v);
    else if(!strcmp(k,"terminal"))snprintf(c->terminal,sizeof c->terminal,"%s",v);
    else if(!strcmp(k,"history"))snprintf(c->history,sizeof c->history,"%s",v);
    else if(!strcmp(k,"dirs"))snprintf(c->dirs,sizeof c->dirs,"%s",v);
    else if(!strcmp(k,"include"))snprintf(c->include,sizeof c->include,"%s",v);
    else if(!strcmp(k,"exclude"))snprintf(c->exclude,sizeof c->exclude,"%s",v);
    else if(!strcmp(k,"bg"))parse_color(v,&c->bg);
    else if(!strcmp(k,"ring"))parse_color(v,&c->ring);
    else if(!strcmp(k,"ring2"))parse_color(v,&c->ring2);
    else if(!strcmp(k,"hl"))parse_color(v,&c->hl);
    else if(!strcmp(k,"text"))parse_color(v,&c->text);
    else if(!strcmp(k,"hltext"))parse_color(v,&c->hltext);
    else if(!strcmp(k,"center"))parse_color(v,&c->center);
    else if(!strcmp(k,"accent"))parse_color(v,&c->accent);
    else if(!strcmp(k,"dim"))parse_color(v,&c->dim);
    else fprintf(stderr,"wheel: unknown key '%s'\n",k);
}
/* the shared ~/.config/sw/config, translated into appwheel's own keys */
static void config_set_shared(void*ud,const char*k,const char*v){ config_set((Config*)ud,k,v); }

static void config_load(Config*c,const char*path){
    FILE*f=fopen(path,"r"); if(!f) return;
    char line[8192];
    while(fgets(line,sizeof line,f)){
        char*s=trim(line); if(!*s||*s=='#'||*s==';') continue;
        char*eq=strchr(s,'='); if(!eq) continue;
        *eq='\0'; config_set(c,trim(s),trim(eq+1));
    }
    fclose(f);
}

/* ------------------------------------------------------------------ */
/* text: TTF atlas via stb_truetype, with debug-font fallback          */
/* ------------------------------------------------------------------ */
#define GLYPH_LO 32
#define GLYPH_HI 255
#define NGLYPH   (GLYPH_HI-GLYPH_LO+1)

typedef struct {
    int ok; SDL_Texture *atlas; int px; float baseline;
    struct { float u,v,w,h,xoff,yoff,adv; } g[NGLYPH];
} Font;

static int try_font_paths(char*out,size_t n){
    const char*cand[]={
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/TTF/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/noto/NotoSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf", NULL };
    for(int i=0;cand[i];i++) if(file_exists(cand[i])){ snprintf(out,n,"%s",cand[i]); return 1; }
    return 0;
}

/* Ask fontconfig (via its fc-match CLI) to resolve a family/pattern — e.g.
   "sans-serif", "monospace", or "JetBrains Mono" — to an actual .ttf path.
   This is how we pick up the DESKTOP's configured default font. No library is
   linked; if fc-match isn't installed we just return 0 and fall back. */
static int fc_match(const char*pattern,char*out,size_t n){
    if(!pattern||!*pattern) pattern="sans-serif";
    char safe[256]; size_t j=0;                    /* strip shell metacharacters */
    for(size_t i=0;pattern[i]&&j<sizeof safe-1;i++){
        char c=pattern[i];
        if(c=='"'||c=='`'||c=='$'||c=='\\'||c==';'||c=='|'||c=='&'||c=='\n'||c=='\r') continue;
        safe[j++]=c;
    }
    safe[j]='\0';
    char cmd[512];
    snprintf(cmd,sizeof cmd,"fc-match --format=%%{file} \"%s\" 2>/dev/null",safe);
    FILE*p=popen(cmd,"r"); if(!p) return 0;
    char buf[PATH_MAX]; size_t r=fread(buf,1,sizeof buf-1,p); buf[r]='\0';
    pclose(p);
    char*s=trim(buf);
    if(*s && file_exists(s)){ snprintf(out,n,"%s",s); return 1; }
    return 0;
}

static int font_load(Font*ft,SDL_Renderer*ren,const char*path,int px){
    memset(ft,0,sizeof *ft); ft->px=px;
    FILE*f=fopen(path,"rb"); if(!f) return 0;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    unsigned char*buf=malloc(sz);
    if(!buf||fread(buf,1,sz,f)!=(size_t)sz){ fclose(f); free(buf); return 0; }
    fclose(f);
    stbtt_fontinfo info;
    if(!stbtt_InitFont(&info,buf,stbtt_GetFontOffsetForIndex(buf,0))){ free(buf); return 0; }
    float sc=stbtt_ScaleForPixelHeight(&info,(float)px);
    int asc,desc,gap; stbtt_GetFontVMetrics(&info,&asc,&desc,&gap); ft->baseline=asc*sc;

    int AW=1024, penx=0,peny=0,rowh=0;
    for(int cp=GLYPH_LO;cp<=GLYPH_HI;cp++){
        int x0,y0,x1,y1; stbtt_GetCodepointBitmapBox(&info,cp,sc,sc,&x0,&y0,&x1,&y1);
        int gw=x1-x0,gh=y1-y0; if(gw<0)gw=0; if(gh<0)gh=0;
        if(penx+gw+1>AW){ penx=0; peny+=rowh+1; rowh=0; }
        if(gh>rowh)rowh=gh;
        penx+=gw+1;
    }
    int AH=peny+rowh+1; if(AH<1)AH=1;
    Uint32*pix=calloc((size_t)AW*AH,4); if(!pix){ free(buf); return 0; }

    penx=0;peny=0;rowh=0;
    for(int cp=GLYPH_LO;cp<=GLYPH_HI;cp++){
        int aw,lsb; stbtt_GetCodepointHMetrics(&info,cp,&aw,&lsb);
        int x0,y0,x1,y1; stbtt_GetCodepointBitmapBox(&info,cp,sc,sc,&x0,&y0,&x1,&y1);
        int gw=x1-x0,gh=y1-y0; if(gw<0)gw=0; if(gh<0)gh=0;
        if(penx+gw+1>AW){ penx=0; peny+=rowh+1; rowh=0; }
        if(gw&&gh){
            unsigned char*bmp=malloc((size_t)gw*gh);
            stbtt_MakeCodepointBitmap(&info,bmp,gw,gh,gw,sc,sc,cp);
            for(int yy=0;yy<gh;yy++)for(int xx=0;xx<gw;xx++)
                pix[(peny+yy)*AW+(penx+xx)]=((Uint32)bmp[yy*gw+xx]<<24)|0x00FFFFFF;
            free(bmp);
        }
        int gi=cp-GLYPH_LO;
        ft->g[gi].u=penx; ft->g[gi].v=peny; ft->g[gi].w=gw; ft->g[gi].h=gh;
        ft->g[gi].xoff=x0; ft->g[gi].yoff=y0; ft->g[gi].adv=aw*sc;
        if(gh>rowh)rowh=gh;
        penx+=gw+1;
    }
    free(buf);
    SDL_Surface*surf=SDL_CreateSurfaceFrom(AW,AH,SDL_PIXELFORMAT_ABGR8888,pix,AW*4);
    if(!surf){ free(pix); return 0; }
    ft->atlas=SDL_CreateTextureFromSurface(ren,surf);
    SDL_DestroySurface(surf); free(pix);
    if(!ft->atlas) return 0;
    SDL_SetTextureScaleMode(ft->atlas,SDL_SCALEMODE_LINEAR);
    SDL_SetTextureBlendMode(ft->atlas,SDL_BLENDMODE_BLEND);
    ft->ok=1; return 1;
}

static unsigned utf8_next(const char**p){
    const unsigned char*s=(const unsigned char*)*p; unsigned cp; int n;
    if(s[0]<0x80){cp=s[0];n=1;}
    else if((s[0]&0xE0)==0xC0){cp=s[0]&0x1F;n=2;}
    else if((s[0]&0xF0)==0xE0){cp=s[0]&0x0F;n=3;}
    else if((s[0]&0xF8)==0xF0){cp=s[0]&0x07;n=4;}
    else{ *p=(const char*)(s+1); return 0xFFFD; }
    for(int i=1;i<n;i++){ if((s[i]&0xC0)!=0x80){n=i;break;} cp=(cp<<6)|(s[i]&0x3F); }
    *p=(const char*)(s+n); return cp;
}
static float text_width(Font*ft,float px_h,const char*s){
    if(!ft->ok) return strlen(s)*8.0f*(px_h/8.0f);
    float ds=px_h/ft->px,w=0; const char*p=s;
    while(*p){ unsigned cp=utf8_next(&p); if(cp<GLYPH_LO||cp>GLYPH_HI)cp='?'; w+=ft->g[cp-GLYPH_LO].adv*ds; }
    return w;
}
static void text_draw(SDL_Renderer*r,Font*ft,float x,float y,float px_h,Col c,const char*s){
    if(!ft->ok){
        /* built-in 8x8 font: compose with any active render scale (e.g. SSAA) */
        float k=px_h/8.0f, csx,csy; SDL_GetRenderScale(r,&csx,&csy);
        SDL_SetRenderDrawColor(r,c.r,c.g,c.b,c.a);
        SDL_SetRenderScale(r,csx*k,csy*k);
        SDL_RenderDebugText(r,x/k,y/k,s);
        SDL_SetRenderScale(r,csx,csy);
        return;
    }
    float ds=px_h/ft->px;
    SDL_SetTextureColorMod(ft->atlas,c.r,c.g,c.b);
    SDL_SetTextureAlphaMod(ft->atlas,c.a);
    float pen=x; const char*p=s;
    while(*p){ unsigned cp=utf8_next(&p); if(cp<GLYPH_LO||cp>GLYPH_HI)cp='?';
        int gi=cp-GLYPH_LO;
        if(ft->g[gi].w>0&&ft->g[gi].h>0){
            SDL_FRect src={ft->g[gi].u,ft->g[gi].v,ft->g[gi].w,ft->g[gi].h};
            SDL_FRect dst={pen+ft->g[gi].xoff*ds,y+(ft->baseline+ft->g[gi].yoff)*ds,ft->g[gi].w*ds,ft->g[gi].h*ds};
            SDL_RenderTexture(r,ft->atlas,&src,&dst);
        }
        pen+=ft->g[gi].adv*ds;
    }
}
static void text_centered(SDL_Renderer*r,Font*ft,float cx,float cy,float px_h,Col c,const char*s){
    float w=text_width(ft,px_h,s); text_draw(r,ft,cx-w/2,cy-px_h/2,px_h,c,s);
}
static void fit_label(Font*ft,float px_h,const char*s,float max_w,char*out,size_t n){
    if(text_width(ft,px_h,s)<=max_w){ snprintf(out,n,"%s",s); return; }
    char buf[256]; snprintf(buf,sizeof buf,"%s",s); int len=(int)strlen(buf);
    while(len>1){ buf[len]='\0';
        char t[260]; snprintf(t,sizeof t,"%.*s..",len,buf);
        if(text_width(ft,px_h,t)<=max_w){ snprintf(out,n,"%.*s",(int)n-1,t); return; }
        len--;
    }
    snprintf(out,n,"..");
}

/* ------------------------------------------------------------------ */
/* application list                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    char *id,*name,*exec,*icon; int terminal;
    SDL_Texture *tex; int icon_tried; int recent;   /* 1 = present in launch history */
} App;
typedef struct { App*v; int n,cap; } AppList;

static void applist_push(AppList*l,App a){
    if(l->n==l->cap){ l->cap=l->cap?l->cap*2:128; l->v=realloc(l->v,l->cap*sizeof*l->v); }
    l->v[l->n++]=a;
}
static int applist_has_id(AppList*l,const char*id){
    for(int i=0;i<l->n;i++) if(!strcmp(l->v[i].id,id)) return 1;
    return 0;
}
static char *strip_field_codes(const char*exec){
    char*out=malloc(strlen(exec)+1),*o=out;
    for(const char*p=exec;*p;p++){ if(*p=='%'){ if(p[1]=='%'){*o++='%';p++;} else if(p[1])p++; } else *o++=*p; }
    *o='\0';
    char*r=out,*w=out; int sp=0;
    for(;*r;r++){ if(*r==' '){ if(!sp)*w++=' '; sp=1; } else { *w++=*r; sp=0; } }
    *w='\0'; return out;
}
static int parse_desktop(const char*path,const char*id,App*out){
    FILE*f=fopen(path,"r"); if(!f) return 0;
    char*name=NULL,*exec=NULL,*type=NULL,*icon=NULL;
    int nodisplay=0,hidden=0,terminal=0,in=0; char line[8192];
    while(fgets(line,sizeof line,f)){
        char*s=trim(line);
        if(*s=='['){ if(in)break; in=!strcmp(s,"[Desktop Entry]"); continue; }
        if(!in||*s=='#') continue;
        char*eq=strchr(s,'='); if(!eq) continue;
        *eq='\0'; char*k=trim(s),*v=trim(eq+1);
        if(!name&&!strcmp(k,"Name"))name=xstrdup(v);
        else if(!exec&&!strcmp(k,"Exec"))exec=xstrdup(v);
        else if(!type&&!strcmp(k,"Type"))type=xstrdup(v);
        else if(!icon&&!strcmp(k,"Icon"))icon=xstrdup(v);
        else if(!strcmp(k,"NoDisplay"))nodisplay=!strcasecmp(v,"true");
        else if(!strcmp(k,"Hidden"))hidden=!strcasecmp(v,"true");
        else if(!strcmp(k,"Terminal"))terminal=!strcasecmp(v,"true");
    }
    fclose(f);
    int ok=1;
    if(!name||!exec) ok=0;
    if(type&&strcmp(type,"Application")) ok=0;
    if(nodisplay||hidden) ok=0;
    if(ok){
        out->id=xstrdup(id); out->name=name; name=NULL;
        out->exec=strip_field_codes(exec); out->icon=icon; icon=NULL;
        out->terminal=terminal; out->tex=NULL; out->icon_tried=0;
    }
    free(name);free(exec);free(type);free(icon);
    return ok;
}
static void scan_dir(AppList*l,const char*dir){
    DIR*d=opendir(dir); if(!d) return;
    struct dirent*e;
    while((e=readdir(d))){
        const char*nm=e->d_name; size_t len=strlen(nm);
        if(len<9||strcmp(nm+len-8,".desktop")) continue;
        char id[256]; snprintf(id,sizeof id,"%.*s",(int)(len-8),nm);
        if(applist_has_id(l,id)) continue;
        char path[PATH_MAX]; snprintf(path,sizeof path,"%s/%s",dir,nm);
        App a; if(parse_desktop(path,id,&a)) applist_push(l,a);
    }
    closedir(d);
}
static void collect_default_dirs(char out[][PATH_MAX],int*n,int max){
    const char*home=getenv("HOME"),*xdh=getenv("XDG_DATA_HOME"),*xdd=getenv("XDG_DATA_DIRS");
    char buf[PATH_MAX];
    if(xdh&&*xdh) snprintf(buf,sizeof buf,"%s/applications",xdh);
    else snprintf(buf,sizeof buf,"%s/.local/share/applications",home?home:".");
    if(*n<max) snprintf(out[(*n)++],PATH_MAX,"%s",buf);
    const char*dirs=(xdd&&*xdd)?xdd:"/usr/local/share:/usr/share";
    char*copy=xstrdup(dirs),*tok=strtok(copy,":");
    while(tok&&*n<max){ snprintf(out[(*n)++],PATH_MAX,"%s/applications",tok); tok=strtok(NULL,":"); }
    free(copy);
    glob_t g;
    if(glob("/opt/*/share/applications",0,NULL,&g)==0)
        for(size_t i=0;i<g.gl_pathc&&*n<max;i++) snprintf(out[(*n)++],PATH_MAX,"%s",g.gl_pathv[i]);
    globfree(&g);
}
static int cmp_name(const void*a,const void*b){ return strcasecmp(((const App*)a)->name,((const App*)b)->name); }

static void apply_config_order(AppList*l,Config*c){
    if(*c->exclude){
        char*copy=xstrdup(c->exclude),*tok=strtok(copy,",");
        while(tok){ char*t=trim(tok);
            for(int i=0;i<l->n;i++)
                if(!strcasecmp(l->v[i].id,t)||!strcasecmp(l->v[i].name,t)){
                    free(l->v[i].id);free(l->v[i].name);free(l->v[i].exec);free(l->v[i].icon);
                    l->v[i]=l->v[--l->n]; i--;
                }
            tok=strtok(NULL,",");
        }
        free(copy);
    }
    if(*c->include){
        AppList kept={0};
        char*copy=xstrdup(c->include),*tok=strtok(copy,",");
        while(tok){ char*t=trim(tok);
            for(int i=0;i<l->n;i++)
                if(l->v[i].id&&(!strcasecmp(l->v[i].id,t)||!strcasecmp(l->v[i].name,t))){
                    applist_push(&kept,l->v[i]); l->v[i].id=NULL; break;
                }
            tok=strtok(NULL,",");
        }
        free(copy);
        for(int i=0;i<l->n;i++)
            if(l->v[i].id){ free(l->v[i].id);free(l->v[i].name);free(l->v[i].exec);free(l->v[i].icon); }
        free(l->v); *l=kept; return;
    }
    if(!strcmp(c->sort,"alpha")) qsort(l->v,l->n,sizeof*l->v,cmp_name);
}
static void apply_recency(AppList*l,Config*c){
    if(*c->include) return;
    if(strcmp(c->sort,"recent")) return;
    FILE*f=fopen(c->history,"r");
    qsort(l->v,l->n,sizeof*l->v,cmp_name);
    if(!f) return;
    App*ord=malloc(l->n*sizeof*ord); int on=0; char*used=calloc(l->n,1); char line[512];
    while(fgets(line,sizeof line,f)){ char*id=trim(line); if(!*id)continue;
        for(int i=0;i<l->n;i++) if(!used[i]&&!strcmp(l->v[i].id,id)){ l->v[i].recent=1; ord[on++]=l->v[i]; used[i]=1; break; } }
    fclose(f);
    for(int i=0;i<l->n;i++) if(!used[i]){ l->v[i].recent=0; ord[on++]=l->v[i]; }
    memcpy(l->v,ord,l->n*sizeof*l->v); free(ord); free(used);
}

/* Search ordering: split matches into "direct" (query starts a word in the name)
   and "close" (query only appears mid-word), then lay them out so the best match
   sits at the TOP, direct matches fan to the RIGHT, close matches to the LEFT.
   Fills filt[] and picks off/selslot so the best is centered (at the top). */
static int is_sep(char c){ return c==' '||c=='-'||c=='_'||c=='.'||c=='/'||c==':'; }
/* match quality: 0 exact, 1 whole-name prefix, 2 word-start, 3 mid-word, -1 none */
static int match_tier(const char*name,const char*q,size_t ql){
    if(!strcasecmp(name,q)) return 0;
    if(!strncasecmp(name,q,ql)) return 1;
    int ws=1;
    for(const char*p=name;*p;p++){
        if(ws && !strncasecmp(p,q,ql)) return 2;
        ws=is_sep(*p);
    }
    return contains_ci(name,q)?3:-1;
}
typedef struct { int idx,tier,len,rb; } Match;
static int match_cmp(const void*a,const void*b){
    const Match*x=a,*y=b;
    if(x->rb  !=y->rb  ) return x->rb  -y->rb;          /* recently-used first (if enabled) */
    if(x->tier!=y->tier) return x->tier-y->tier;       /* better tier first */
    if(x->len !=y->len ) return x->len -y->len;         /* shorter (closer) first */
    return x->idx-y->idx;                               /* then recency order */
}
static int build_filter(AppList*apps,const char*query,int*filt,int slots,int recent_first,int*p_off,int*p_selslot){
    int fn=0;
    if(!query[0]){                                      /* no query: all apps, in order */
        for(int i=0;i<apps->n;i++) filt[fn++]=i;
        *p_off=0; *p_selslot=0; return fn;
    }
    size_t ql=strlen(query);
    Match *m=malloc(apps->n*sizeof(Match)); int nm=0;
    if(m) for(int i=0;i<apps->n;i++){
        const char*nm_s=apps->v[i].name, *id_s=apps->v[i].id;
        int tn=match_tier(nm_s,query,ql);                      /* match on the name ... */
        int ti=id_s?match_tier(id_s,query,ql):-1;              /* ...or the id */
        int t = tn<0?ti : (ti<0?tn : (tn<ti?tn:ti));           /* best of the two */
        if(t<0) continue;
        int mlen=1<<30;                                        /* shortest string that hit that tier */
        if(tn==t){ int l=(int)strlen(nm_s); if(l<mlen)mlen=l; }
        if(ti==t){ int l=(int)strlen(id_s); if(l<mlen)mlen=l; }
        m[nm].idx=i; m[nm].tier=t; m[nm].len=mlen;
        m[nm].rb=(recent_first && apps->v[i].recent)?0:1;      /* recently-used matches win */
        nm++;
    }
    if(nm>1) qsort(m,nm,sizeof(Match),match_cmp);       /* best-ranked first */
    int *dir=malloc(nm*sizeof(int)+1), *clo=malloc(nm*sizeof(int)+1);
    int nd=0,nc=0;
    if(dir&&clo) for(int j=0;j<nm;j++){                 /* keep ranked order within each group */
        if(m[j].tier<3) dir[nd++]=m[j].idx; else clo[nc++]=m[j].idx;
    }
    free(m);
    fn=nd+nc;
    if(fn<=0){ free(dir);free(clo); *p_off=0;*p_selslot=0; return 0; }
    int best,*right,nr,*left,nl;
    if(nd>0){ best=dir[0]; right=dir+1; nr=nd-1; left=clo;   nl=nc;   }
    else    { best=clo[0]; right=clo;   nr=0;    left=clo+1; nl=nc-1; }
    int c=(fn-1)/2;                                     /* best sits at the array centre */
    filt[c]=best;
    int rp=0,lp=0;
    for(int ri=c+1; ri<fn; ri++){                      /* right of centre: direct, then overflow */
        if(rp<nr) filt[ri]=right[rp++]; else if(lp<nl) filt[ri]=left[lp++];
    }
    for(int li=c-1; li>=0; li--){                      /* left of centre: close, then overflow */
        if(lp<nl) filt[li]=left[lp++]; else if(rp<nr) filt[li]=right[rp++];
    }
    free(dir); free(clo);
    int vis = fn<slots?fn:slots; if(vis<1)vis=1;
    if(vis>=4 && (vis&1)==0) vis--;                     /* odd -> best lands at the exact top */
    int maxoff = fn>vis?fn-vis:0;
    int off = c-(vis-1)/2; if(off<0)off=0; if(off>maxoff)off=maxoff;
    int ss = c-off; if(ss<0)ss=0; if(ss>vis-1)ss=vis-1;
    *p_off=off; *p_selslot=ss;
    return fn;
}

/* ------------------------------------------------------------------ */
/* icons                                                               */
/* ------------------------------------------------------------------ */
static void resolve_icon(const char*name,char*out,size_t n){
    out[0]='\0'; if(!name||!*name) return;
    if(strchr(name,'/')){ if(file_exists(name)) snprintf(out,n,"%s",name); return; }
    const char*home=getenv("HOME"),*xdh=getenv("XDG_DATA_HOME");
    char bases[8][PATH_MAX]; int nb=0;
    if(xdh&&*xdh) snprintf(bases[nb++],PATH_MAX,"%s/icons",xdh);
    else if(home) snprintf(bases[nb++],PATH_MAX,"%s/.local/share/icons",home);
    if(home) snprintf(bases[nb++],PATH_MAX,"%s/.icons",home);
    snprintf(bases[nb++],PATH_MAX,"/usr/local/share/icons");
    snprintf(bases[nb++],PATH_MAX,"/usr/share/icons");
    const char*themes[]={"hicolor","Adwaita","gnome","breeze","Papirus","Humanity",NULL};
    const char*sizes[]={"48x48","64x64","32x32","128x128","256x256","96x96","24x24","scalable",NULL};
    const char*cats[]={"apps","categories","devices","places","status","mimetypes","actions",NULL};
    const char*exts[]={"png","svg","jpg","bmp",NULL};
    char p[PATH_MAX];
    /* icons dropped directly in an icons dir (not a theme subdir), e.g.
       ~/.local/share/icons/duckduckgo.svg — check these first */
    for(int b=0;b<nb;b++)for(int e=0;exts[e];e++){
        snprintf(p,sizeof p,"%.3500s/%.400s.%s",bases[b],name,exts[e]);
        if(file_exists(p)){ snprintf(out,n,"%s",p); return; }
    }
    for(int b=0;b<nb;b++)for(int t=0;themes[t];t++)for(int s=0;sizes[s];s++)
        for(int ca=0;cats[ca];ca++)for(int e=0;exts[e];e++){
            snprintf(p,sizeof p,"%.3500s/%s/%s/%s/%.400s.%s",bases[b],themes[t],sizes[s],cats[ca],name,exts[e]);
            if(file_exists(p)){ snprintf(out,n,"%s",p); return; }
        }
    const char*pm[]={"/usr/share/pixmaps","/usr/local/share/pixmaps",NULL};
    for(int i=0;pm[i];i++)for(int e=0;exts[e];e++){
        snprintf(p,sizeof p,"%s/%.400s.%s",pm[i],name,exts[e]);
        if(file_exists(p)){ snprintf(out,n,"%s",p); return; }
    }
}
static SDL_Texture *load_icon_tex(SDL_Renderer*ren,const char*path){
    int w,h,ch; unsigned char*data=stbi_load(path,&w,&h,&ch,4);
    if(!data) return NULL;
    SDL_Surface*s=SDL_CreateSurfaceFrom(w,h,SDL_PIXELFORMAT_ABGR8888,data,w*4);
    if(!s){ stbi_image_free(data); return NULL; }
    SDL_Texture*t=SDL_CreateTextureFromSurface(ren,s);
    SDL_DestroySurface(s); stbi_image_free(data);
    if(t){ SDL_SetTextureScaleMode(t,SDL_SCALEMODE_LINEAR); SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND); }
    return t;
}
static int ends_with_ci(const char*s,const char*suf){
    size_t ls=strlen(s),lf=strlen(suf);
    return ls>=lf && strcasecmp(s+ls-lf,suf)==0;
}
/* rasterise an SVG icon to a texture at ~px pixels (aspect preserved) */
static SDL_Texture *load_svg_tex(SDL_Renderer*ren,const char*path,int px){
    if(px<16)px=16; if(px>512)px=512;
    NSVGimage*img=nsvgParseFromFile(path,"px",96.0f);
    if(!img) return NULL;
    if(img->width<=0||img->height<=0){ nsvgDelete(img); return NULL; }
    float big=img->width>img->height?img->width:img->height;
    float sc=(float)px/big;
    int ow=(int)(img->width*sc+0.5f), oh=(int)(img->height*sc+0.5f);
    if(ow<1)ow=1; if(oh<1)oh=1;
    unsigned char*pix=malloc((size_t)ow*oh*4);
    NSVGrasterizer*r=nsvgCreateRasterizer();
    if(!pix||!r){ free(pix); if(r)nsvgDeleteRasterizer(r); nsvgDelete(img); return NULL; }
    nsvgRasterize(r,img,0,0,sc,pix,ow,oh,ow*4);
    nsvgDeleteRasterizer(r); nsvgDelete(img);
    SDL_Surface*s=SDL_CreateSurfaceFrom(ow,oh,SDL_PIXELFORMAT_ABGR8888,pix,ow*4);
    if(!s){ free(pix); return NULL; }
    SDL_Texture*t=SDL_CreateTextureFromSurface(ren,s);
    SDL_DestroySurface(s); free(pix);
    if(t){ SDL_SetTextureScaleMode(t,SDL_SCALEMODE_LINEAR); SDL_SetTextureBlendMode(t,SDL_BLENDMODE_BLEND); }
    return t;
}
static SDL_Texture *app_icon(SDL_Renderer*ren,App*a,Config*c){
    if(!c->icons) return NULL;
    if(!a->icon_tried){ a->icon_tried=1;
        char path[PATH_MAX]; resolve_icon(a->icon,path,sizeof path);
        if(*path){
            if(ends_with_ci(path,".svg")){
                int px=(int)(c->icon_px*c->ui_scale*2.0f); if(px<48)px=48; if(px>256)px=256;
                a->tex=load_svg_tex(ren,path,px);
            } else {
                a->tex=load_icon_tex(ren,path);
            }
        }
    }
    return a->tex;
}

/* ------------------------------------------------------------------ */
/* launching + history                                                 */
/* ------------------------------------------------------------------ */
static void mkparents(const char*path){
    char tmp[PATH_MAX]; snprintf(tmp,sizeof tmp,"%s",path);
    for(char*p=tmp+1;*p;p++) if(*p=='/'){ *p='\0'; mkdir(tmp,0755); *p='/'; }
}
static void history_prepend(Config*c,const char*id){
    mkparents(c->history);
    char*lines[256]; int ln=0; FILE*f=fopen(c->history,"r");
    if(f){ char b[512]; while(ln<255&&fgets(b,sizeof b,f)){ char*t=trim(b);
        if(!*t||!strcmp(t,id))continue; lines[ln++]=xstrdup(t); }
        fclose(f);
    }
    f=fopen(c->history,"w"); if(!f){ for(int i=0;i<ln;i++)free(lines[i]); return; }
    fprintf(f,"%s\n",id);
    for(int i=0;i<ln;i++){ fprintf(f,"%s\n",lines[i]); free(lines[i]); }
    fclose(f);
}
static void launch(App*a,Config*c){
    char cmd[8192];
    if(!strcmp(c->launcher,"gtk-launch")) snprintf(cmd,sizeof cmd,"gtk-launch %s.desktop",a->id);
    else if(a->terminal) snprintf(cmd,sizeof cmd,"%s -e %s",c->terminal,a->exec);
    else snprintf(cmd,sizeof cmd,"%s",a->exec);
    history_prepend(c,a->id);

    /* Launch fully detached from whatever started us. Without this, the child
       inherits our stdin/stdout/stderr and controlling terminal, so a terminal
       app (or anything that touches the tty) corrupts the shell we were run
       from. We: new session (setsid) so there's no controlling tty, a second
       fork so it can never reacquire one and gets reparented to init, and
       std fds pointed at /dev/null. */
    pid_t pid=fork();
    if(pid<0){ fprintf(stderr,"appwheel: fork failed\n"); return; }
    if(pid==0){
        setsid();
        pid_t p2=fork();
        if(p2>0) _exit(0);
        int fd=open("/dev/null",O_RDWR);
        if(fd>=0){ dup2(fd,0); dup2(fd,1); dup2(fd,2); if(fd>2) close(fd); }
        const char*home=getenv("HOME"); if(home){ if(chdir(home)!=0){} }
        execl("/bin/sh","sh","-c",cmd,(char*)NULL);
        _exit(127);
    }
    waitpid(pid,NULL,0);   /* reap the intermediate child; grandchild -> init */
}

/* ------------------------------------------------------------------ */
/* geometry primitives                                                 */
/* ------------------------------------------------------------------ */
static void fill_circle(SDL_Renderer*r,float cx,float cy,float rad,Col c){
    const int SEG=96; SDL_Vertex v[SEG+2]; SDL_FColor fc=tofc(c);
    v[0].position=(SDL_FPoint){cx,cy}; v[0].color=fc; v[0].tex_coord=(SDL_FPoint){0,0};
    for(int i=0;i<=SEG;i++){ float a=(float)(2*M_PI*i/SEG);
        v[i+1].position=(SDL_FPoint){cx+rad*cosf(a),cy+rad*sinf(a)};
        v[i+1].color=fc; v[i+1].tex_coord=(SDL_FPoint){0,0}; }
    int idx[SEG*3]; for(int i=0;i<SEG;i++){ idx[i*3]=0; idx[i*3+1]=i+1; idx[i*3+2]=i+2; }
    SDL_RenderGeometry(r,NULL,v,SEG+2,idx,SEG*3);
}
static void fill_sector(SDL_Renderer*r,float cx,float cy,float r0,float r1,float a0,float a1,Col c){
    int seg=(int)((a1-a0)/0.04f)+2; if(seg<2)seg=2; if(seg>160)seg=160;
    int nv=(seg+1)*2; SDL_Vertex*v=malloc(nv*sizeof*v); SDL_FColor fc=tofc(c);
    for(int i=0;i<=seg;i++){ float a=a0+(a1-a0)*i/seg,ca=cosf(a),sa=sinf(a);
        v[2*i].position=(SDL_FPoint){cx+r0*ca,cy+r0*sa};
        v[2*i+1].position=(SDL_FPoint){cx+r1*ca,cy+r1*sa};
        v[2*i].color=v[2*i+1].color=fc; v[2*i].tex_coord=v[2*i+1].tex_coord=(SDL_FPoint){0,0}; }
    int ni=seg*6,*idx=malloc(ni*sizeof*idx);
    for(int i=0;i<seg;i++){ int b=2*i;
        idx[i*6]=b;idx[i*6+1]=b+1;idx[i*6+2]=b+2; idx[i*6+3]=b+1;idx[i*6+4]=b+3;idx[i*6+5]=b+2; }
    SDL_RenderGeometry(r,NULL,v,nv,idx,ni); free(v); free(idx);
}
static void fill_tri(SDL_Renderer*r,float x0,float y0,float x1,float y1,float x2,float y2,Col c){
    SDL_FColor fc=tofc(c);
    SDL_Vertex v[3]={ {{x0,y0},fc,{0,0}}, {{x1,y1},fc,{0,0}}, {{x2,y2},fc,{0,0}} };
    int idx[3]={0,1,2};
    SDL_RenderGeometry(r,NULL,v,3,idx,3);
}
/* stacked arrowheads pointing dir (+1 right / -1 left); count grows with speed */
static void draw_chevrons(SDL_Renderer*r,float x,float y,int dir,float s,int count,Col c){
    if(count<1)count=1;
    float gap=s*0.72f, total=(count-1)*gap, start=x-dir*total/2;
    for(int i=0;i<count;i++){
        float ax=start+dir*i*gap;
        fill_tri(r, ax+dir*s*0.5f, y,
                    ax-dir*s*0.5f, y-s*0.62f,
                    ax-dir*s*0.5f, y+s*0.62f, c);
    }
}
static float norm_ang(float a){ while(a>M_PI)a-=2*M_PI; while(a<=-M_PI)a+=2*M_PI; return a; }

/* is the cursor on a paging ICON? (used only for the startup arming) */
static int on_icon(float mx,float my,float cx,float cy,float rl,float hitr){
    float lx=cx+rl*cosf(ICON_AL), ly=cy+rl*sinf(ICON_AL);
    float rx=cx+rl*cosf(ICON_AR), ry=cy+rl*sinf(ICON_AR);
    float h2=hitr*hitr;
    float a=mx-lx,b=my-ly, c=mx-rx,d=my-ry;
    return (a*a+b*b<=h2) || (c*c+d*d<=h2);
}

/* ------------------------------------------------------------------ */
static void dump_config(void){
    fputs(
"# appwheel config  —  ~/.config/appwheel/config\n"
"# One key=value per line. '#' starts a comment. Blank lines are ignored.\n"
"# Every key here also works on the command line (key=value or --key=value);\n"
"# command-line values win over the file. See `appwheel --help` for the full list.\n"
"# The values below are the built-in defaults, so this file changes nothing\n"
"# until you edit it.\n"
"\n"
"# --- window ---\n"
"width=900\n"
"height=900\n"
"fullscreen=1     # cover the screen as a borderless overlay (0 = 900x900 window)\n"
"close_on_focus_loss=0   # 1 = quit when the window loses focus (dmenu-style)\n"
"\n"
"# --- ring layout & paging ---\n"
"slots=11          # wide, easy-to-hit slots across the TOP arc\n"
"arc=240           # degrees of the ring used for apps (rest = paging zone)\n"
"radius=0.44       # wheel size, as a fraction of the shorter screen side\n"
"y_offset=0.10     # nudge the wheel down (apps sit up top, so this centers it)\n"
"page_ms=110       # base ms per paged step at the SLOW end. Bigger = calmer\n"
"                  # start. Paging eases in: slow where you enter the bottom\n"
"                  # zone, fast toward straight-down.\n"
"\n"
"# --- rendering ---\n"
"ssaa=2            # full-scene supersampling 1..4 (smooths edges). alias: aa\n"
"animate=1         # crossfade app names/icons when results change\n"
"anim_ms=90        # its duration (0 or animate=0 to turn it off)\n"
"recent_first=1    # bias the best match toward your recently-used apps\n"
"icons=1           # show .desktop icons if found (PNG/SVG/JPG/BMP)\n"
"icon_px=46        # icon size (before ui_scale)\n"
"\n"
"# --- text ---\n"
"ui_scale=1.0      # master text size. 1.3 = bigger, 0.85 = smaller.\n"
"                  # aliases: font_scale, text_scale\n"
"label_px=24       # app labels on the wheel   (all four are multiplied by\n"
"title_px=25       # selected app name (center)  ui_scale, so tweak one or all)\n"
"search_px=20      # the text you type (search box)\n"
"count_px=20       # the \"3 / 42\" counter\n"
"# By default appwheel uses your desktop's configured font (via fontconfig).\n"
"# Set a .ttf path or a family name to override, e.g. font=JetBrains Mono\n"
"# font=/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf\n"
"font_px=50        # glyph atlas height; the sharpness ceiling for big text\n"
"\n"
"# --- launching / ordering ---\n"
"# appwheel logs every app you launch to the history file below and shows the\n"
"# most-recently-opened ones first. This is the default (sort=recent).\n"
"sort=recent       # recent = most-recently-opened first (DEFAULT) | alpha = A-Z\n"
"launcher=sh       # sh = run Exec= ; gtk-launch = launch by desktop id\n"
"terminal=xterm    # used for Terminal=true entries when launcher=sh\n"
"# history=~/.cache/appwheel/history   # the opened-apps log (auto-created)\n"
"\n"
"# which apps show / in what order (an id is the .desktop filename without\n"
"# the extension; run `appwheel --list` to see them):\n"
"# dirs=~/.local/share/applications:/usr/share/applications\n"
"# include=firefox,code,gimp     # show ONLY these ids, in this exact order\n"
"# exclude=htop,xterm            # hide these (matches id OR Name)\n"
"\n"
"# --- colors: #rrggbb or #rrggbbaa ---\n"
"# bg alpha < ff => transparent window background (needs a compositor)\n"
"bg=0d111700\n"
"ring=1e2733f2\n"
"ring2=26313ff2\n"
"hl=cb9b00ff        # highlighted slot\n"
"text=e8e8e8ff      # normal slot text\n"
"hltext=141414ff    # text on the highlighted slot\n"
"center=0d1117e6    # hub disc (keep some alpha so search text stays legible)\n"
"accent=89afc4ff    # typed query text + active paging chevrons\n"
"dim=5a6b7aff       # hints / counters / idle chevrons\n",
    stdout);
}

static void usage(const char*a0){
    printf(
"appwheel — a GTA-V-style radial (weapon-wheel) app launcher (Wayland & X11)\n"
"\n"
"USAGE\n"
"  %s [options] [key=value ...]\n"
"\n"
"  Every config key below is also a command-line argument. Both forms work:\n"
"      appwheel slots=8 arc=220 icons=0\n"
"      appwheel --slots=8 --arc=220 --icons=0\n"
"  CLI values override the config file.\n"
"\n"
"OPTIONS\n"
"  -c, --config PATH   config file (default: $XDG_CONFIG_HOME/wheel/config)\n"
"      --dump-config   print a commented default config (redirect to save it):\n"
"                        appwheel --dump-config > ~/.config/appwheel/config\n"
"      --list          print discovered apps (with resolved icon) and exit\n"
"      --no-recent     rank matches purely by relevance, ignoring recent-app bias\n"
"  -d, --dmenu         read newline-separated items from stdin, print the chosen\n"
"                      one to stdout (a dmenu/bemenu/wofi-style picker)\n"
"  -h, --help          show this help and exit\n"
"\n"
"LAYOUT / INTERACTION\n"
"  slots=11            wide, easy-to-hit app slots across the top arc\n"
"  arc=240             degrees of the ring used for apps (rest = paging zone)\n"
"  radius=0.44         wheel size (fraction of the shorter screen side)\n"
"  y_offset=0.10       nudge the wheel down so it looks vertically centered\n"
"  page_ms=110         base ms per paged step; the farther left/right the\n"
"                      cursor sits in the bottom zone, the faster it pages\n"
"\n"
"ICONS  (PNG, SVG, JPG, BMP)\n"
"  icons=1             1 = show .desktop icons, 0 = labels only\n"
"  icon_px=46          icon draw size (before ui_scale)\n"
"\n"
"TEXT\n"
"  ui_scale=1.0        master text size (aliases: font_scale, text_scale)\n"
"  label_px=24         app labels on the wheel     ) each is multiplied\n"
"  title_px=25         selected app name (center)  ) by ui_scale; set any\n"
"  search_px=20        the text you type           ) one independently\n"
"  count_px=20         the \"3 / 42\" counter        )\n"
"  font=PATH|FAMILY    a .ttf path, or a fontconfig family like \"JetBrains Mono\".\n"
"                      Default: your desktop's configured font (via fc-match).\n"
"  font_px=50          atlas raster height; larger = crisper big text\n"
"\n"
"RENDERING\n"
"  ssaa=2              full-scene supersampling 1..4 (smooths edges; alias: aa)\n"
"  animate=1 anim_ms=90  crossfade names/icons when results change (0 = off)\n"
"  width=900 height=900 fullscreen=1  close_on_focus_loss=0\n"
"\n"
"SOURCES / ORDER\n"
"  An app's \"id\" is just its .desktop filename without the extension:\n"
"      /usr/share/applications/firefox.desktop      -> id \"firefox\"\n"
"      ~/.local/share/applications/spotify.desktop  -> id \"spotify\"\n"
"  Run  appwheel --list  to print every id next to its visible Name.\n"
"\n"
"  sort=recent                    recent (most-recently-opened first, the\n"
"                                 DEFAULT) | alpha (A-Z). Every launch is\n"
"                                 logged to the history file automatically.\n"
"  include=firefox,gimp,spotify   show ONLY these ids, in this exact order\n"
"  exclude=htop,xterm             hide these (matches the id OR the Name)\n"
"  dirs=DIR:DIR                   scan these instead of the default dirs, e.g.\n"
"      dirs=~/.local/share/applications:/usr/share/applications\n"
"  launcher=sh                    sh = run Exec= ; gtk-launch = launch by id\n"
"  terminal=xterm                 terminal for Terminal=true apps (launcher=sh)\n"
"  history=PATH                   recency file (default ~/.cache/wheel/history)\n"
"\n"
"EXAMPLES\n"
"  appwheel --list                          # discover ids\n"
"  appwheel include=firefox,code,gimp       # a curated wheel, in that order\n"
"  appwheel exclude=htop sort=alpha ssaa=3  # hide htop, A-Z, extra-smooth\n"
"  appwheel ui_scale=1.3 icon_px=56         # bigger text and icons\n"
"\n"
"COLORS  (#rrggbb or #rrggbbaa)\n"
"  bg ring ring2 hl text hltext center accent dim\n"
"  bg alpha < ff => transparent window background (needs a compositor)\n"
"\n"
"CONTROLS\n"
"  mouse over top arc      highlight a slot\n"
"  bottom-left / -right    page back / forward (farther out = faster)\n"
"  scroll wheel            scroll the list under the fixed highlight\n"
"  type                    filter by name        Backspace  edit filter\n"
"  Enter / left-click      launch                Esc        clear filter / quit\n"
"  Right-click             quit\n",
    a0);
}

/* Load the app/menu list (stdin for dmenu, else scan .desktop dirs). Kept
   separate so the SDL window can be created first — see main(). */
static void load_apps(AppList*apps, Config*cfg, int dmenu){
    if(dmenu){                                     /* dmenu mode: items come from stdin */
        char line[8192];
        while(fgets(line,sizeof line,stdin)){
            size_t L=strlen(line);
            while(L && (line[L-1]=='\n'||line[L-1]=='\r')) line[--L]=0;
            App a; memset(&a,0,sizeof a);
            a.id=xstrdup(line); a.name=xstrdup(line);
            applist_push(apps,a);                  /* input order preserved */
        }
    } else if(*cfg->dirs){ char*copy=xstrdup(cfg->dirs),*tok=strtok(copy,":");
        while(tok){ char d[PATH_MAX]; expand_tilde(tok,d,sizeof d); scan_dir(apps,d); tok=strtok(NULL,":"); }
        free(copy);
    } else {
        char dirs[64][PATH_MAX]; int nd=0; collect_default_dirs(dirs,&nd,64);
        for(int i=0;i<nd;i++) scan_dir(apps,dirs[i]);
    }
    if(!dmenu){ apply_config_order(apps,cfg); apply_recency(apps,cfg); }
}

int main(int argc,char**argv){
    Config cfg; config_defaults(&cfg);
    char cfgpath[PATH_MAX];
    const char*xc=getenv("XDG_CONFIG_HOME"),*home=getenv("HOME");
    if(xc&&*xc) snprintf(cfgpath,sizeof cfgpath,"%s/appwheel/config",xc);
    else snprintf(cfgpath,sizeof cfgpath,"%s/.config/appwheel/config",home?home:".");

    int want_list=0, dmenu=0;
    for(int i=1;i<argc;i++){
        if((!strcmp(argv[i],"-c")||!strcmp(argv[i],"--config"))&&i+1<argc)
            snprintf(cfgpath,sizeof cfgpath,"%s",argv[++i]);
        else if(!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")){ usage(argv[0]); return 0; }
        else if(!strcmp(argv[i],"--dump-config")){ dump_config(); return 0; }
        else if(!strcmp(argv[i],"--list")) want_list=1;
        else if(!strcmp(argv[i],"--dmenu")||!strcmp(argv[i],"-d")) dmenu=1;
    }
    { char tmp[PATH_MAX]; expand_tilde(cfgpath,tmp,sizeof tmp); snprintf(cfgpath,sizeof cfgpath,"%s",tmp); }
    sw_shared_apply("appwheel",config_set_shared,&cfg);   /* shared first */
    config_load(&cfg,cfgpath);                            /* our own wins  */
    for(int i=1;i<argc;i++){
        if(!strcmp(argv[i],"-c")||!strcmp(argv[i],"--config")){ i++; continue; }
        if(!strcmp(argv[i],"--list")||!strcmp(argv[i],"-h")||!strcmp(argv[i],"--help")||!strcmp(argv[i],"--dump-config")||!strcmp(argv[i],"--dmenu")||!strcmp(argv[i],"-d")) continue;
        if(!strcmp(argv[i],"--no-recent")||!strcmp(argv[i],"--all-apps")){ cfg.recent_first=0; continue; }
        char*a=argv[i]; while(*a=='-') a++;            /* accept --key=value too */
        char*eq=strchr(a,'='); if(eq){ *eq='\0'; config_set(&cfg,a,eq+1); }
    }
    if(cfg.ssaa<1)cfg.ssaa=1; if(cfg.ssaa>4)cfg.ssaa=4;
    if(cfg.slots<1)cfg.slots=1;
    { char tmp[PATH_MAX];
      expand_tilde(cfg.history,tmp,sizeof tmp); snprintf(cfg.history,sizeof cfg.history,"%s",tmp);
      if(*cfg.font){ expand_tilde(cfg.font,tmp,sizeof tmp); snprintf(cfg.font,sizeof cfg.font,"%s",tmp); } }

    AppList apps={0};

    if(want_list){                                 /* --list: no window needed */
        load_apps(&apps,&cfg,dmenu);
        for(int j=0;j<apps.n;j++)
            printf("%-26s | %-32s | icon=%s%s\n",apps.v[j].id,apps.v[j].name,
                   apps.v[j].icon?apps.v[j].icon:"-", apps.v[j].terminal?"  [term]":"");
        return 0;
    }

    /* Identify as AppWheel so the compositor shows a proper name, and set the
       Wayland app_id / X11 WM_CLASS so float/center rules match. Before init. */
    SDL_SetAppMetadata("AppWheel","1.0","org.appwheel.AppWheel");
    SDL_SetHint(SDL_HINT_APP_ID,"appwheel");

    if(!SDL_Init(SDL_INIT_VIDEO)){ fprintf(stderr,"SDL_Init: %s\n",SDL_GetError()); return 1; }

    /* "Fullscreen" here means a borderless window the size of the display — a
       floating overlay, NOT exclusive fullscreen. Exclusive fullscreen makes the
       surface opaque (black instead of transparent) and causes a mode-switch
       glitch on close, so we avoid it. */
    int winw=cfg.width, winh=cfg.height;
    SDL_Rect dbounds; int have_bounds=0;
    if(cfg.fullscreen){
        SDL_DisplayID d=SDL_GetPrimaryDisplay();
        if(d && SDL_GetDisplayBounds(d,&dbounds)){ winw=dbounds.w; winh=dbounds.h; have_bounds=1; }
    }
    SDL_WindowFlags wf=SDL_WINDOW_BORDERLESS|SDL_WINDOW_ALWAYS_ON_TOP;
    if(cfg.bg.a<255) wf|=SDL_WINDOW_TRANSPARENT;
    SDL_Window*win=SDL_CreateWindow("AppWheel",winw,winh,wf);
    if(!win){ fprintf(stderr,"CreateWindow: %s\n",SDL_GetError()); return 1; }
    if(have_bounds) SDL_SetWindowPosition(win,dbounds.x,dbounds.y);
    SDL_Renderer*ren=SDL_CreateRenderer(win,NULL);
    if(!ren){ fprintf(stderr,"CreateRenderer: %s\n",SDL_GetError()); return 1; }
    SDL_SetRenderDrawBlendMode(ren,SDL_BLENDMODE_BLEND);
    SDL_RaiseWindow(win);
    SDL_StartTextInput(win);

    /* Get the window mapped & focused NOW, before the (possibly slow, on a busy
       machine) .desktop scan and font load — so keystrokes typed during loading
       are queued for us by the compositor instead of being lost. */
    SDL_SetRenderDrawColor(ren,cfg.bg.r,cfg.bg.g,cfg.bg.b,cfg.bg.a);
    SDL_RenderClear(ren); SDL_RenderPresent(ren);
    for(int i=0;i<4;i++){ SDL_PumpEvents(); SDL_Delay(1); }

    load_apps(&apps,&cfg,dmenu);                    /* slow part; keystrokes queue meanwhile */

    if(apps.n==0){ fprintf(stderr,"appwheel: no %s\n", dmenu?"input on stdin":".desktop applications found");
                   SDL_DestroyRenderer(ren); SDL_DestroyWindow(win); SDL_Quit(); return 1; }

    Font font; memset(&font,0,sizeof font);
    char fontpath[PATH_MAX]={0};
    if(*cfg.font){                                   /* explicit: a path OR a family name */
        if(file_exists(cfg.font)) snprintf(fontpath,sizeof fontpath,"%s",cfg.font);
        else fc_match(cfg.font,fontpath,sizeof fontpath);
    }
    if(!*fontpath) fc_match("sans-serif",fontpath,sizeof fontpath);  /* the desktop default */
    if(!*fontpath) try_font_paths(fontpath,sizeof fontpath);         /* last-resort known paths */
    int atlas_px=(int)(cfg.font_px*cfg.ui_scale); if(atlas_px<10)atlas_px=10; if(atlas_px>200)atlas_px=200;
    if(*fontpath) font_load(&font,ren,fontpath,atlas_px);
    if(!font.ok) fprintf(stderr,"appwheel: no usable TTF font found; using built-in font\n");

    int *filt=malloc(apps.n*sizeof*filt); int fn=0; char query[256]={0};
    int off=0, selslot=0;                 /* fixed highlight = off+selslot     */
    Uint64 anim0=0;                       /* time of last filter change (animation) */
    /* Per-slot crossfade bookkeeping: last_* is the layout drawn last frame;
       snap_* is frozen at the moment of a results change. A slot only fades if
       its app+position differ from the snapshot, so anything that stays put
       (e.g. the top icon while you refine a query) is not re-animated. */
    #define SNAPMAX 256
    int   snap_item[SNAPMAX], last_item[SNAPMAX]; float snap_ang[SNAPMAX], last_ang[SNAPMAX];
    int   snap_n=0, last_n=0, snap_sel=-1, last_sel=-1;
    #define REBUILD() do{ \
        memcpy(snap_item,last_item,sizeof(int)*last_n); memcpy(snap_ang,last_ang,sizeof(float)*last_n); \
        snap_n=last_n; snap_sel=last_sel; \
        fn=build_filter(&apps,query,filt,cfg.slots,cfg.recent_first,&off,&selslot); anim0=SDL_GetTicks(); }while(0)
    REBUILD();

    int page_dir=0; float page_speed=0;   /* for the paging indicators         */
    float mx=cfg.width/2.0f,my=cfg.height/2.0f;
    Uint64 last_page=0, blink0=SDL_GetTicks();
    int running=1, had_focus=0, page_armed=0, prev_region=0, chose=0;
    #define CHOOSE() do{ if(dmenu){ printf("%s\n",apps.v[filt[sel]].name); fflush(stdout); chose=1; } \
                         else launch(&apps.v[filt[sel]],&cfg); running=0; }while(0)
    float arc=cfg.arc_deg*(float)DEG; if(arc<60*DEG)arc=60*DEG; if(arc>330*DEG)arc=330*DEG;

    SDL_Texture*target=NULL; int tw=0,th=0;
    float ui=cfg.ui_scale;

    while(running){
        int w,h; SDL_GetWindowSize(win,&w,&h);
        int ss=cfg.ssaa;
        float mind=(w<h?w:h);
        float cx=w/2.0f, cy=h/2.0f + cfg.y_offset*mind;
        float R=mind*cfg.radius, ri=R*0.46f, rc=ri*0.95f, rl=(R+ri)/2.0f;

        int searching = query[0]!=0;
        int vis = fn? (fn<cfg.slots?fn:cfg.slots) : 0;
        if(searching && vis>=4 && (vis&1)==0) vis--;   /* odd count -> a real top slot, balanced sides */
        int maxoff = fn>vis?fn-vis:0;
        if(off<0)off=0;
        if(off>maxoff)off=maxoff;
        if(vis>0){ if(selslot<0)selslot=0; if(selslot>vis-1)selslot=vis-1; }
        int sel = fn? off+selslot : 0;
        if(sel>fn-1)sel=fn-1;

        float step = arc/(float)(vis>0?vis:1);   /* equal sectors fill the arc */
        float top=-(float)M_PI/2;
        float astart=top-arc/2;

        /* --- paging: hovering anywhere in the bottom zone pages; deeper toward
           straight-down = faster (slow -> fast). Gated by page_armed so it never
           fires from wherever the cursor happened to open on (startup safety). --- */
        float half_arc = arc/2 + 2.0f*(float)DEG;     /* apps fill the arc; page below it */
        page_dir=0; page_speed=0;
        if(page_armed){
          float dx=mx-cx,dy=my-cy,dist=sqrtf(dx*dx+dy*dy);
          if(dist>rc){
              float pa=atan2f(dy,dx), dtop=norm_ang(pa-top);
              if(fabsf(dtop)>half_arc){               /* below the app arc = paging zone */
                  page_dir = dtop>0?1:-1;             /* right=forward, left=back */
                  float depth=fabsf(dtop)-half_arc, maxd=(float)M_PI-half_arc;
                  page_speed = maxd>0?clampf(depth/maxd,0,1):0;
                  if(fn>vis){
                      Uint64 now=SDL_GetTicks();
                      float t=page_speed*page_speed;   /* ease in: gentle start, fast finish */
                      int iv=(int)(cfg.page_ms*(1.0f-0.82f*t)); if(iv<24)iv=24;
                      if((Sint64)(now-last_page)>=iv){
                          off += page_dir;
                          if(off<0)off=0;
                          if(off>maxoff)off=maxoff;
                          last_page=now;
                      }
                  }
              }
          }
        }

        SDL_Event ev;
        while(SDL_PollEvent(&ev)){
            if(ev.type==SDL_EVENT_QUIT) running=0;
            else if(ev.type==SDL_EVENT_WINDOW_FOCUS_GAINED) had_focus=1;
            else if(ev.type==SDL_EVENT_WINDOW_FOCUS_LOST){ if(cfg.close_on_focus_loss && had_focus) running=0; }
            else if(ev.type==SDL_EVENT_MOUSE_MOTION){
                mx=ev.motion.x; my=ev.motion.y;
                float dx=mx-cx,dy=my-cy,dist=sqrtf(dx*dx+dy*dy);
                int region=1; float adtop=0;   /* 1=apps/center, 2=bottom off-icon, 3=on icon */
                if(dist>rc){
                    float pa=atan2f(dy,dx), dtop=norm_ang(pa-top);
                    adtop=fabsf(dtop);
                    if(adtop<=half_arc){                    /* within the app arc */
                        if(vis>0){
                            int slot = (int)floorf((dtop+arc/2)/(step>0?step:1));
                            if(slot<0)slot=0; if(slot>vis-1)slot=vis-1;
                            selslot=slot;
                        }
                    }
                    else if(on_icon(mx,my,cx,cy,rl,R*0.10f)) region=3;   /* on the icon */
                    else region=2;                                       /* bottom, off icon */
                }
                /* Startup safety (see the 3-state rules):
                     - reaching the apps/center always arms (state 1)
                     - approaching the icon from the bottom arms (state 2 -> icon)
                     - leaving the icon DOWN/SIDEWAYS into the bottom arms; leaving
                       it UP toward the apps does NOT, so you can pick an app
                       without scrolling. Once armed, the whole bottom scrolls. */
                float icon_dtop=fabsf(norm_ang(ICON_AL-top));   /* ~150 deg */
                if(region==1) page_armed=1;
                else if(region==3 && prev_region==2) page_armed=1;
                else if(region==2 && prev_region==3 && adtop>=icon_dtop) page_armed=1;
                prev_region=region;
            }
            else if(ev.type==SDL_EVENT_MOUSE_WHEEL){
                int dir=ev.wheel.y>0?-1:(ev.wheel.y<0?1:0);
                if(!dir&&ev.wheel.x)dir=ev.wheel.x>0?1:-1;
                if(dir){
                    if(maxoff>0){ off+=dir; if(off<0)off=0; if(off>maxoff)off=maxoff; }
                    else if(vis>0){ selslot+=dir; if(selslot<0)selslot=0; if(selslot>vis-1)selslot=vis-1; }
                }
            }
            else if(ev.type==SDL_EVENT_MOUSE_BUTTON_DOWN){
                if(ev.button.button==SDL_BUTTON_LEFT&&fn){ CHOOSE(); }
                else if(ev.button.button==SDL_BUTTON_RIGHT) running=0;
            }
            else if(ev.type==SDL_EVENT_TEXT_INPUT){
                strncat(query,ev.text.text,sizeof query-strlen(query)-1);
                REBUILD();
            }
            else if(ev.type==SDL_EVENT_KEY_DOWN){
                SDL_Keycode k=ev.key.key;
                if(k==SDLK_ESCAPE){ if(query[0]){query[0]='\0';REBUILD();} else running=0; }
                else if(k==SDLK_RETURN||k==SDLK_KP_ENTER){ if(fn){ CHOOSE(); } }
                else if(k==SDLK_BACKSPACE){ int L=strlen(query);
                    if(L>0){ L--; while(L>0&&((unsigned char)query[L]&0xC0)==0x80)L--; query[L]='\0'; REBUILD(); } }
                else if(k==SDLK_RIGHT || (k==SDLK_TAB && !(ev.key.mod&SDL_KMOD_SHIFT))){
                    if(vis>0){ if(selslot<vis-1)selslot++; else if(off<maxoff)off++; } }
                else if(k==SDLK_LEFT || (k==SDLK_TAB && (ev.key.mod&SDL_KMOD_SHIFT))){
                    if(vis>0){ if(selslot>0)selslot--; else if(off>0)off--; } }
                else if(k==SDLK_DOWN){ if(maxoff>0){ off+=vis; if(off>maxoff)off=maxoff; } }
                else if(k==SDLK_UP){ if(maxoff>0){ off-=vis; if(off<0)off=0; } }
            }
        }
        if(!running) break;

        /* Input above may have changed fn/off/selslot (typing, backspace, paging),
           so recompute the whole layout before rendering — otherwise this frame
           would draw with the previous match count and the highlight flickers. */
        searching = query[0]!=0;
        vis = fn? (fn<cfg.slots?fn:cfg.slots) : 0;
        if(searching && vis>=4 && (vis&1)==0) vis--;
        maxoff = fn>vis?fn-vis:0;
        if(off<0)off=0;
        if(off>maxoff)off=maxoff;
        if(vis>0){ if(selslot<0)selslot=0; if(selslot>vis-1)selslot=vis-1; }
        sel = fn? off+selslot : 0;
        if(sel>fn-1)sel=fn-1;
        step = arc/(float)(vis>0?vis:1);

        /* -------------------- render (optionally supersampled) ------------- */
        if(ss>1){
            if(!target||tw!=w*ss||th!=h*ss){
                if(target)SDL_DestroyTexture(target);
                target=SDL_CreateTexture(ren,SDL_PIXELFORMAT_RGBA8888,SDL_TEXTUREACCESS_TARGET,w*ss,h*ss);
                if(target){ SDL_SetTextureScaleMode(target,SDL_SCALEMODE_LINEAR);
                            SDL_SetTextureBlendMode(target,SDL_BLENDMODE_BLEND); tw=w*ss; th=h*ss; }
            }
            if(target){ SDL_SetRenderTarget(ren,target); SDL_SetRenderScale(ren,(float)ss,(float)ss); }
            else ss=1;
        }

        SDL_SetRenderDrawColor(ren,cfg.bg.r,cfg.bg.g,cfg.bg.b,cfg.bg.a);
        SDL_RenderClear(ren);

        float slotw=step;
        float label_px=cfg.label_px*ui;
        /* app-data crossfade: on a results change, the names/icons/highlight text
           fade back in while the ring sectors (background) stay put — smooths fast
           typing without delaying anything. */
        float af = (cfg.animate && cfg.anim_ms>0)
                   ? clampf(0.30f + 0.70f*(float)(SDL_GetTicks()-anim0)/(float)cfg.anim_ms, 0.0f, 1.0f) : 1.0f;
        for(int i=0;i<vis;i++){
            int item=filt[off+i];
            float a = astart+(i+0.5f)*step;
            float a0=a-slotw/2+0.010f, a1=a+slotw/2-0.010f;
            int hot=(off+i==sel);
            Col scol=hot?cfg.hl:(i&1?cfg.ring2:cfg.ring);
            fill_sector(ren,cx,cy,ri,R,a0,a1,scol);            /* sector stays solid */

            /* stationary if this exact app sat at (nearly) this angle before the change */
            int stationary=0;
            for(int k=0;k<snap_n;k++) if(snap_item[k]==item && fabsf(snap_ang[k]-a)<0.02f){ stationary=1; break; }
            float sfa = stationary?1.0f:af; Uint8 sfab=(Uint8)(255*sfa);
            if(i<SNAPMAX){ last_item[i]=item; last_ang[i]=a; }

            float lx=cx+rl*cosf(a), ly=cy+rl*sinf(a);
            Col tc=hot?cfg.hltext:cfg.text; tc.a=(Uint8)(tc.a*sfa);   /* text fades (unless it held still) */
            float chord=2*rl*sinf(slotw/2)*0.84f;
            char lbl[160]; fit_label(&font,label_px,apps.v[item].name,chord,lbl,sizeof lbl);

            SDL_Texture*ic=app_icon(ren,&apps.v[item],&cfg);
            if(ic){
                float isz=cfg.icon_px*ui; if(isz>chord)isz=chord;
                float gap=7.0f*ui;                       /* breathing room */
                float blockH=isz+gap+label_px;
                float topy=ly-blockH/2;
                SDL_FRect dst={lx-isz/2, topy, isz, isz};
                SDL_SetTextureAlphaMod(ic,sfab);         /* icon fades (unless it held still) */
                SDL_RenderTexture(ren,ic,NULL,&dst);
                text_centered(ren,&font,lx, topy+isz+gap+label_px/2, label_px, tc, lbl);
            } else {
                text_centered(ren,&font,lx,ly,label_px,tc,lbl);
            }
        }
        last_n = vis<SNAPMAX?vis:SNAPMAX;

        /* paging indicators (chevrons) — brighten & multiply with speed */
        if(fn>vis){
            float aL=ICON_AL, aR=ICON_AR;   /* bottom-left / -right paging icons */
            float lxp=cx+rl*cosf(aL), lyp=cy+rl*sinf(aL);
            float rxp=cx+rl*cosf(aR), ryp=cy+rl*sinf(aR);
            /* left */
            { int on=(page_dir<0); float sp=on?page_speed:0;
              Col c=on?cfg.accent:cfg.dim; c.a=on?255:210;
              int cnt=1+(on?(int)lroundf(sp*2):0);
              float s=(19.0f+ (on?13.0f*sp:0))*ui;
              draw_chevrons(ren,lxp,lyp,-1,s,cnt,c); }
            /* right */
            { int on=(page_dir>0); float sp=on?page_speed:0;
              Col c=on?cfg.accent:cfg.dim; c.a=on?255:210;
              int cnt=1+(on?(int)lroundf(sp*2):0);
              float s=(19.0f+ (on?13.0f*sp:0))*ui;
              draw_chevrons(ren,rxp,ryp,+1,s,cnt,c); }
        }

        /* center hub */
        fill_circle(ren,cx,cy,rc,cfg.center);
        Uint64 now=SDL_GetTicks(); int caret=((now-blink0)/500)%2==0;
        char shown[300];
        if(query[0]) snprintf(shown,sizeof shown,"%s%s",query,caret?"|":" ");
        else         snprintf(shown,sizeof shown,"type to search");
        Col qc=query[0]?cfg.accent:cfg.dim;
        text_centered(ren,&font,cx,cy-rc*0.42f,cfg.search_px*ui,qc,shown);

        if(fn){
            int sel_item=filt[sel]; last_sel=sel_item;
            float caf = (sel_item==snap_sel)?1.0f:af;      /* same app in the hub -> no fade */
            char nm[200]; fit_label(&font,cfg.title_px*ui,apps.v[sel_item].name,rc*1.7f,nm,sizeof nm);
            Col nmc=cfg.text; nmc.a=(Uint8)(nmc.a*caf);
            text_centered(ren,&font,cx,cy,cfg.title_px*ui,nmc,nm);
            char cnt[64]; snprintf(cnt,sizeof cnt,"%d / %d",sel+1,fn);
            Col cc=cfg.dim; cc.a=(Uint8)(cc.a*caf);
            text_centered(ren,&font,cx,cy+rc*0.40f,cfg.count_px*ui,cc,cnt);
        } else {
            last_sel=-1;
            text_centered(ren,&font,cx,cy,cfg.title_px*ui,cfg.dim,"no matches");
        }

        if(ss>1 && target){
            SDL_SetRenderScale(ren,1,1);
            SDL_SetRenderTarget(ren,NULL);
            SDL_SetRenderDrawColor(ren,0,0,0,0);
            SDL_RenderClear(ren);
            SDL_RenderTexture(ren,target,NULL,NULL);     /* fade is per-element now */
        }
        SDL_RenderPresent(ren);
        SDL_Delay(16);
    }

    if(target)SDL_DestroyTexture(target);
    SDL_StopTextInput(win);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return (dmenu && !chose) ? 1 : 0;   /* dmenu convention: non-zero on cancel */
}
