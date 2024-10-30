#ifndef UI_COMMON_SDL_H
#define UI_COMMON_SDL_H

#include "const.h"

#include <stdbool.h>
#include "nuklear/nuklear.h"

enum ui_renderer_t {
    UI_RENDERER_D3D11_CSC,
    UI_RENDERER_D3D11,
    UI_RENDERER_OGL_CSC,
    UI_RENDERER_OGL,
    UI_RENDERER_GLES_CSC,
    UI_RENDERER_GLES,
    UI_RENDERER_GLES_ANGLE,
    UI_RENDERER_SDL_HW,
    UI_RENDERER_SDL_SW,

    UI_RENDERER_COUNT,
};

extern enum ui_renderer_t ui_renderer;
extern SDL_Window *ui_sdl_win[SCREEN_COUNT];
extern struct nk_context *ui_nk_ctx;

typedef enum {
    VIEW_MODE_TOP_BOT,
    VIEW_MODE_SEPARATE,
    VIEW_MODE_TOP,
    VIEW_MODE_BOT,
} view_mode_t;
extern view_mode_t ui_view_mode;

#ifdef _WIN32
#include <windows.h>
extern HWND ui_hwnd[SCREEN_COUNT];
extern HDC ui_hdc[SCREEN_COUNT];
extern LONG_PTR ui_sdl_wnd_proc[SCREEN_COUNT];
#endif
extern Uint32 ui_sdl_win_id[SCREEN_COUNT];

extern int ui_nk_width, ui_nk_height;
extern float ui_nk_scale;

extern int ui_win_width[SCREEN_COUNT], ui_win_height[SCREEN_COUNT];
extern int ui_win_drawable_width[SCREEN_COUNT], ui_win_drawable_height[SCREEN_COUNT];
extern float ui_win_scale[SCREEN_COUNT];

UNUSED static bool is_renderer_sdl_renderer(void) {
    return ui_renderer >= UI_RENDERER_SDL_HW && ui_renderer <= UI_RENDERER_SDL_SW;
}

UNUSED static bool is_renderer_d3d11(void) {
    return ui_renderer >= UI_RENDERER_D3D11_CSC && ui_renderer <= UI_RENDERER_D3D11;
}

UNUSED static bool is_renderer_sdl_ogl(void) {
    return ui_renderer >= UI_RENDERER_OGL_CSC && ui_renderer <= UI_RENDERER_GLES_ANGLE;
}

UNUSED static bool is_renderer_gles_angle(void) {
    return ui_renderer == UI_RENDERER_GLES_ANGLE;
}

UNUSED static bool is_renderer_sdl_hw(void) {
    return ui_renderer == UI_RENDERER_SDL_HW;
}

int ui_common_sdl_init(void);
void ui_common_sdl_destroy(void);

void ui_view_mode_update(view_mode_t view_mode);
void ui_window_size_update(int window_top_bot);

#endif
