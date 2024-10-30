#include <SDL2/SDL.h>

#include "ui_common_sdl.h"
#include "ui_renderer_sdl.h"
#include "main.h"

enum ui_renderer_t ui_renderer;
SDL_Window *ui_sdl_win[SCREEN_COUNT];
struct nk_context *ui_nk_ctx;
view_mode_t ui_view_mode;

#ifdef _WIN32
HWND ui_hwnd[SCREEN_COUNT];
HDC ui_hdc[SCREEN_COUNT];
LONG_PTR ui_sdl_wnd_proc[SCREEN_COUNT];
#endif
Uint32 ui_sdl_win_id[SCREEN_COUNT];

int ui_nk_width, ui_nk_height;
float ui_nk_scale;

int ui_win_width[SCREEN_COUNT], ui_win_height[SCREEN_COUNT];
int ui_win_drawable_width[SCREEN_COUNT], ui_win_drawable_height[SCREEN_COUNT];
float ui_win_scale[SCREEN_COUNT];

int ui_common_sdl_init(void) {
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");

    if (is_renderer_sdl_renderer()) {
        if (SDL_Init(SDL_INIT_VIDEO)) {
            err_log("SDL_Init: %s\n", SDL_GetError());
            return -1;
        }
    } else {
        if (is_renderer_sdl_ogl()) {
            if (is_renderer_gles_angle()) {
                SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
            } else {
                SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "0");
            }
        }

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_EVENTS)) {
            err_log("SDL_Init: %s\n", SDL_GetError());
            return -1;
        }
    }

    return 0;
}

void ui_common_sdl_destroy(void) {
    SDL_Quit();
}

void ui_view_mode_update(view_mode_t view_mode) {
    switch (view_mode) {
        case VIEW_MODE_TOP_BOT:
        case VIEW_MODE_TOP:
        case VIEW_MODE_BOT:
            SDL_HideWindow(ui_sdl_win[SCREEN_BOT]);
            break;

        case VIEW_MODE_SEPARATE:
            SDL_ShowWindow(ui_sdl_win[SCREEN_BOT]);
            break;
    }

    SDL_SetWindowFullscreen(ui_sdl_win[SCREEN_TOP], 0);
    SDL_RestoreWindow(ui_sdl_win[SCREEN_TOP]);
    SDL_SetWindowFullscreen(ui_sdl_win[SCREEN_BOT], 0);
    SDL_RestoreWindow(ui_sdl_win[SCREEN_BOT]);

    switch (view_mode) {
        case VIEW_MODE_TOP_BOT:
            SDL_SetWindowSize(ui_sdl_win[SCREEN_TOP], WIN_WIDTH_DEFAULT, WIN_HEIGHT_DEFAULT);
            break;

        case VIEW_MODE_TOP:
            SDL_SetWindowSize(ui_sdl_win[SCREEN_TOP], WIN_WIDTH_DEFAULT, WIN_HEIGHT12_DEFAULT);
            break;

        case VIEW_MODE_BOT:
            SDL_SetWindowSize(ui_sdl_win[SCREEN_TOP], WIN_WIDTH2_DEFAULT, WIN_HEIGHT12_DEFAULT);
            break;

        case VIEW_MODE_SEPARATE:
            SDL_SetWindowSize(ui_sdl_win[SCREEN_TOP], WIN_WIDTH_DEFAULT, WIN_HEIGHT12_DEFAULT);
            SDL_SetWindowSize(ui_sdl_win[SCREEN_BOT], WIN_WIDTH2_DEFAULT, WIN_HEIGHT12_DEFAULT);
            break;
    }

    if (view_mode == VIEW_MODE_SEPARATE) {
        // TODO
    }
}

void ui_window_size_update(int window_top_bot) {
    int i = window_top_bot;

    SDL_GetWindowSize(ui_sdl_win[i], &ui_win_width[i], &ui_win_height[i]);

    if (is_renderer_sdl_renderer()) {
        SDL_GetRendererOutputSize(sdl_renderer[i], &ui_win_drawable_width[i], &ui_win_drawable_height[i]);
    } else if (is_renderer_sdl_ogl()) {
        SDL_GL_GetDrawableSize(ui_sdl_win[i], &ui_win_drawable_width[i], &ui_win_drawable_height[i]);
    } else if (is_renderer_d3d11()) {
        RECT rect = {};
        GetClientRect(ui_hwnd[i], &rect);
        ui_win_drawable_width[i] = rect.right;
        ui_win_drawable_height[i] = rect.bottom;
    }

    float scale_x = (float)(ui_win_drawable_width[i]) / (float)(ui_win_width[i]);
    float scale_y = (float)(ui_win_drawable_height[i]) / (float)(ui_win_height[i]);
    scale_x = roundf(scale_x * ui_font_scale_step_factor) / ui_font_scale_step_factor;
    scale_y = roundf(scale_y * ui_font_scale_step_factor) / ui_font_scale_step_factor;

    if (i == SCREEN_TOP) {
        ui_nk_width = ui_win_width[i];
        ui_nk_height = ui_win_height[i];
        ui_nk_scale = ui_win_scale[i];
    }
}
