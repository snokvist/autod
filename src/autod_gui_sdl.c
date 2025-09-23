// autod_gui_sdl.c — SDL/TTF implementation
#include "autod_gui.h"

#ifdef USE_SDL2_GUI
#include <SDL2/SDL.h>
#ifdef USE_SDL2_TTF
#include <SDL_ttf.h>
#endif
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static volatile int g_gui_should_quit = 0;

#ifdef USE_SDL2_TTF
static TTF_Font *g_font = NULL;

static TTF_Font *load_font(void) {
    const char *env_font = getenv("AUTOD_GUI_FONT");
    const char *fallbacks[] = {
        env_font && *env_font ? env_font : NULL,
        "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationMono-Regular.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        "/usr/share/fonts/truetype/freefont/FreeSans.ttf",
        NULL
    };

    for (int i = 0; fallbacks[i]; i++) {
        g_font = TTF_OpenFont(fallbacks[i], 14);
        if (g_font) return g_font;
    }
    fprintf(stderr, "GUI: failed to open font (set AUTOD_GUI_FONT)\n");
    return NULL;
}
#endif

static void draw_text(SDL_Renderer *R, int x, int y, const char *s) {
#ifdef USE_SDL2_TTF
    if (!g_font || !s || !*s) return;
    SDL_Color col = (SDL_Color){255,255,255,255};
    SDL_Surface *surf = TTF_RenderUTF8_Blended(g_font, s, col);
    if (!surf) return;
    SDL_Texture *tex = SDL_CreateTextureFromSurface(R, surf);
    if (tex) {
        SDL_Rect dst = {x, y, surf->w, surf->h};
        SDL_RenderCopy(R, tex, NULL, &dst);
        SDL_DestroyTexture(tex);
    }
    SDL_FreeSurface(surf);
#else
    (void)R; (void)x; (void)y; (void)s;
#endif
}

typedef struct {
    autod_gui_config_t   cfg;
    autod_gui_snapshot_cb cb;
    void*                 cb_user;
} gui_ctx_t;

static int gui_thread(void *arg) {
    gui_ctx_t *G = (gui_ctx_t*)arg;

    if (SDL_Init(SDL_INIT_VIDEO) != 0) { free(G); return 0; }
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");

#ifdef USE_SDL2_TTF
    if (TTF_Init() != 0) { SDL_Quit(); free(G); return 0; }
    if (!load_font()) { TTF_Quit(); SDL_Quit(); free(G); return 0; }
#endif

    SDL_Window *W = SDL_CreateWindow("autod - nodes",
                                     SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     900, 600, SDL_WINDOW_SHOWN);
    if (!W) {
#ifdef USE_SDL2_TTF
        TTF_Quit();
#endif
        SDL_Quit(); free(G); return 0;
    }
    SDL_Renderer *R = SDL_CreateRenderer(W, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!R) {
        SDL_DestroyWindow(W);
#ifdef USE_SDL2_TTF
        TTF_Quit();
#endif
        SDL_Quit(); free(G); return 0;
    }
    SDL_RenderSetLogicalSize(R, 900, 600);

    int fh = 8;
#ifdef USE_SDL2_TTF
    fh = TTF_FontHeight(g_font);
    if (fh < 8) fh = 8;
#endif
    const int pad      = 8;
    const int header_h = 28;
    const int status_y = header_h + pad;
    const int header_y = status_y + fh + pad;
    const int sep_y    = header_y + fh + 2;
    const int row_y0   = sep_y + 12;
    const int row_h    = fh + 8;

    int sel = 0;
    int trigger_enter = 0;

    while (!g_gui_should_quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { g_gui_should_quit = 1; break; }
            if (e.type == SDL_KEYDOWN) {
                switch (e.key.keysym.sym) {
                    case SDLK_q:
                    case SDLK_ESCAPE: g_gui_should_quit = 1; break;
                    case SDLK_UP:     if (sel > 0) sel--;  break;
                    case SDLK_DOWN:   sel++;               break;
                    case SDLK_RETURN: trigger_enter = 1;   break;
                }
            }
        }

        autod_gui_snapshot_t s = {0};
        if (G->cb) G->cb(&s, G->cb_user);
        if (sel >= s.count) sel = s.count ? (s.count - 1) : 0;
        if (sel < 0) sel = 0;

        if (trigger_enter) {
            trigger_enter = 0;
            if (sel < s.count) {
                fprintf(stderr, "ENTER on %s (role=%s)\n",
                        s.nodes[sel].ip,
                        s.nodes[sel].role[0] ? s.nodes[sel].role : "-");
            }
        }

        SDL_SetRenderDrawColor(R, 18,22,26,255);
        SDL_RenderClear(R);

        SDL_Rect hdr = {0,0,900,header_h};
        SDL_SetRenderDrawColor(R, 32,38,44,255);
        SDL_RenderFillRect(R, &hdr);
        draw_text(R, pad, 10, "autod — Nodes (Up/Down=Select, Enter=Log, Q/Esc=Quit)");

        char st[256];
        snprintf(st, sizeof(st),
                 "Scan: %s  %u/%u  %d%%  |  Port:%d  Role:%.24s  Device:%.24s",
                 s.scanning ? "RUNNING" : "idle", s.done, s.targets, s.progress_pct,
                 G->cfg.port,
                 G->cfg.role[0]   ? G->cfg.role   : "-",
                 G->cfg.device[0] ? G->cfg.device : "-");
        draw_text(R, pad, status_y, st);

        draw_text(R, pad,   header_y, "IP");
        draw_text(R, 180,   header_y, "Role");
        draw_text(R, 360,   header_y, "Device");
        draw_text(R, 600,   header_y, "Version");

        SDL_SetRenderDrawColor(R, 80,80,80,255);
        SDL_RenderDrawLine(R, pad, sep_y, 892, sep_y);

        int y = row_y0;
        for (int i=0; i<s.count && i<64; i++, y += row_h) {
            if (i == sel) {
                SDL_Rect hi = {pad-2, y-2, 888, fh+6};
                SDL_SetRenderDrawColor(R, 24,28,34,255);
                SDL_RenderFillRect(R, &hi);
            }
            draw_text(R, pad,  y, s.nodes[i].ip);
            draw_text(R, 180,  y, s.nodes[i].role[0]?s.nodes[i].role:"-");
            draw_text(R, 360,  y, s.nodes[i].device[0]?s.nodes[i].device:"-");
            draw_text(R, 600,  y, s.nodes[i].version[0]?s.nodes[i].version:"-");
        }

        SDL_Rect bar = {pad, 560, 884, 12};
        SDL_SetRenderDrawColor(R, 32,38,44,255);
        SDL_RenderFillRect(R, &bar);
        SDL_SetRenderDrawColor(R, 54,194,117,255);
        int w = (s.progress_pct * bar.w) / 100;
        SDL_Rect fill = {bar.x, bar.y, w, bar.h};
        SDL_RenderFillRect(R, &fill);

        SDL_RenderPresent(R);
    }

    SDL_DestroyRenderer(R);
    SDL_DestroyWindow(W);
#ifdef USE_SDL2_TTF
    if (g_font) { TTF_CloseFont(g_font); g_font = NULL; }
    TTF_Quit();
#endif
    SDL_Quit();
    free(G);
    return 0;
}

int autod_gui_start(const autod_gui_config_t* cfg,
                    autod_gui_snapshot_cb snapshot_cb,
                    void* snapshot_user)
{
    if (!cfg || !snapshot_cb) return -1;
    g_gui_should_quit = 0;
    gui_ctx_t *G = (gui_ctx_t*)calloc(1, sizeof(*G));
    if (!G) return -1;
    G->cfg = *cfg;
    G->cb  = snapshot_cb;
    G->cb_user = snapshot_user;

    SDL_Thread *th = SDL_CreateThread(gui_thread, "autod_gui", G);
    if (!th) { free(G); return -1; }
    /* Detached: if you want join semantics, store th somewhere. */
    return 0;
}

void autod_gui_request_quit(void) { g_gui_should_quit = 1; }
void autod_gui_join(void) { /* no-op with detached thread */ }

#else /* !USE_SDL2_GUI — stubbed API so core can link */

int  autod_gui_start(const autod_gui_config_t* cfg,
                     autod_gui_snapshot_cb snapshot_cb,
                     void* snapshot_user)
{ (void)cfg; (void)snapshot_cb; (void)snapshot_user; return -1; }

void autod_gui_request_quit(void) { /* no-op */ }
void autod_gui_join(void)         { /* no-op */ }

#endif
