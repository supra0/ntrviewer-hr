#include "main.h"
#include "ui_common_sdl.h"
#include "ui_renderer_sdl.h"

#include "const.h"

#include "nuklear_sdl_renderer.h"

SDL_Renderer *sdl_renderer[SCREEN_COUNT];

static SDL_Window *sdl_win[SCREEN_COUNT];
static SDL_Texture *sdl_texture[SCREEN_COUNT][SCREEN_COUNT];
static struct nk_context *nk_ctx;

static int sdl_win_init(void) {
    for (int i = 0; i < SCREEN_COUNT; ++i) {
        sdl_win[i] = SDL_CreateWindow(WIN_TITLE,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            WIN_WIDTH_DEFAULT, WIN_HEIGHT_DEFAULT, SDL_WIN_FLAGS_DEFAULT);
        if (!sdl_win[i]) {
            err_log("SDL_CreateWindow: %s\n", SDL_GetError());
            return -1;
        }
    }

    return 0;
}

static void sdl_win_destroy(void) {
    for (int i = 0; i < SCREEN_COUNT; ++i) {
        if (sdl_win[i]) {
            SDL_DestroyWindow(sdl_win[i]);
            sdl_win[i] = NULL;
        }
    }
}

static int sdl_texture_init(void) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "1");
    for (int j = 0; j < SCREEN_COUNT; ++j) {
        for (int i = 0; i < SCREEN_COUNT; ++i) {
            sdl_texture[j][i] = SDL_CreateTexture(sdl_renderer[j], SDL_FORMAT, SDL_TEXTUREACCESS_STREAMING, SCREEN_WIDTH, i == SCREEN_TOP ? SCREEN_HEIGHT0 : SCREEN_HEIGHT1);
            if (!sdl_texture[j][i]) {
                err_log("SDL_CreateTexture: %s\n", SDL_GetError());
                return -1;
            }
        }
    }

    return 0;
}

static void sdl_texture_destroy(void) {
    for (int j = 0; j < SCREEN_COUNT; ++j) {
        for (int i = 0; i < SCREEN_COUNT; ++i) {
            if (sdl_texture[j][i]) {
                SDL_DestroyTexture(sdl_texture[j][i]);
                sdl_texture[j][i] = NULL;
            }
        }
    }
}

static int sdl_renderer_init(void) {
    Uint32 renderer_flags = is_renderer_sdl_hw() ?
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC :
        SDL_RENDERER_SOFTWARE | SDL_RENDERER_PRESENTVSYNC;

    for (int i = 0; i < SCREEN_COUNT; ++i) {
        sdl_renderer[i] = SDL_CreateRenderer(sdl_win[i], -1, renderer_flags);
        if (!sdl_renderer[i])
        {
            err_log("SDL_CreateRenderer: %s\n", SDL_GetError());
            return -1;
        }
    }

    if (sdl_texture_init()) {
        return -1;
    }

    nk_ctx = nk_sdl_renderer_init(sdl_win[SCREEN_TOP], sdl_renderer[SCREEN_TOP]);
    if (!nk_ctx)
        return -1;

    return 0;
}

static void sdl_renderer_destroy(void) {
    if (nk_ctx) {
        nk_sdl_renderer_shutdown();
        nk_ctx = NULL;
    }

    sdl_texture_destroy();

    for (int i = 0; i < SCREEN_COUNT; ++i) {
        if (sdl_renderer[i]) {
            SDL_DestroyRenderer(sdl_renderer[i]);
            sdl_renderer[i] = NULL;
        }
    }
}

int ui_renderer_sdl_init(void) {
    if (sdl_win_init()) {
        return -1;
    }

    for (int i = 0; i < SCREEN_COUNT; ++i)
        ui_sdl_win[i] = sdl_win[i];

    if (sdl_renderer_init()) {
        return -1;
    }

    ui_nk_ctx = nk_ctx;

    return 0;
}

void ui_renderer_sdl_destroy(void) {
    ui_nk_ctx = NULL;

    sdl_renderer_destroy();

    for (int i = 0; i < SCREEN_COUNT; ++i)
        ui_sdl_win[i] = NULL;

    sdl_win_destroy();
}
