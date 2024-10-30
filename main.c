#include "main.h"
#include "rp_syn.h"

#include "ui_common_sdl.h"
#include "ui_renderer_sdl.h"
#include "ntr_common.h"

#include <SDL2/SDL_syswm.h>

#include <math.h>

static void main_init() {
#ifdef _WIN32
    OSVERSIONINFO osvi = {};
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    GetVersionEx(&osvi);
    err_log("Windows version %d.%d.%d\n", (int)osvi.dwMajorVersion, (int)osvi.dwMinorVersion, (int)osvi.dwBuildNumber);
#endif

    RO_INIT();
    rp_syn_startup();
}

static void main_destroy() {
    RO_UNINIT();
}

static enum ui_renderer_t renderer_list[UI_RENDERER_COUNT];
static int renderer_count;

static void parse_args(int argc, char **argv) {
    (void)argc;
    (void)argv;

    int j = 0;
    for (int i = 0; i < UI_RENDERER_COUNT; ++i) {
        // Do not attempt ANGLE as a default option
        if (i != UI_RENDERER_GLES_ANGLE) {
            renderer_list[j] = i;
            ++j;
        }
    }
    renderer_count = j;
}

static LRESULT CALLBACK main_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    int i = GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    bool need_handle_input = 0;

    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_SIZE: {
            bool resize_top_and_ui = i == SCREEN_TOP;

            ui_win_drawable_width[i] = NK_MAX(LOWORD(lparam), 1);
            ui_win_drawable_height[i] = NK_MAX(HIWORD(lparam), 1);

            ui_win_width[i] = roundf(ui_win_drawable_width[i] / ui_win_scale[i]);
            ui_win_height[i] = roundf(ui_win_drawable_height[i] / ui_win_scale[i]);

            if (resize_top_and_ui) {
                ui_nk_width = ui_win_width[i];
                ui_nk_height = ui_win_height[i];
                ui_nk_scale = ui_win_scale[i];
            }

            break;
        }

        case WM_DPICHANGED:
            ui_win_scale[i] = (float)HIWORD(wparam) / USER_DEFAULT_SCREEN_DPI;
            break;

        case WM_LBUTTONDOWN:
        case WM_LBUTTONUP:
        case WM_RBUTTONDOWN:
        case WM_RBUTTONUP:
        case WM_MBUTTONDOWN:
        case WM_MBUTTONUP:
        case WM_MOUSEMOVE:
        case WM_LBUTTONDBLCLK: {
            int x = (short)LOWORD(lparam);
            int y = (short)HIWORD(lparam);
            x = x / ui_win_scale[i];
            y = y / ui_win_scale[i];
            lparam = MAKELPARAM(x, y);
            need_handle_input = i == SCREEN_TOP;
            break;
        }

        case WM_MOUSEWHEEL:
            need_handle_input = i == SCREEN_TOP;
            break;

        case WM_CHAR:
        case WM_KEYDOWN:
        case WM_KEYUP:
        case WM_SYSKEYDOWN:
        case WM_SYSKEYUP:
            need_handle_input = 1;
            break;
    }

    if (need_handle_input) {
        if (is_renderer_d3d11()) {
            // TODO
        }
    }

    return CallWindowProcA((WNDPROC)ui_sdl_wnd_proc[i], hwnd, msg, wparam, lparam);
}

static struct nk_color nk_window_bgcolor = { 28, 48, 62, 255 };

#ifndef _WIN32
static int sdl_win_resize_evt_watcher(void *, SDL_Event *event) {
  if (event->type == SDL_WINDOWEVENT && event->window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
    int i;
    for (i = 0; i < SCREEN_COUNT; ++i) {
      if (event->window.windowID == ui_sdl_win_id[i]) {
        break;
      }
    }
    if (i < SCREEN_COUNT) {
      ui_window_size_update(i);
    }
  }
  return 0;
}
#endif

static void main_windows(void) {
#ifdef _WIN32
    for (int i = 0; i < SCREEN_COUNT; ++i) {
        SDL_SysWMinfo wmInfo;

        SDL_VERSION(&wmInfo.version);
        SDL_GetWindowWMInfo(ui_sdl_win[i], &wmInfo);

        ui_hwnd[i] = wmInfo.info.win.window;
        ui_hdc[i] = wmInfo.info.win.hdc;
    }

    HBRUSH bg_brush = CreateSolidBrush(
        RGB(nk_window_bgcolor.r, nk_window_bgcolor.g, nk_window_bgcolor.b));
    for (int i = 0; i < SCREEN_COUNT; ++i) {
        SetWindowLongPtrA(ui_hwnd[i], GWLP_USERDATA, i);
        ui_sdl_wnd_proc[i] = GetWindowLongPtrA(ui_hwnd[i], GWLP_WNDPROC);
        SetWindowLongPtrA(ui_hwnd[i], GWLP_WNDPROC, (LONG_PTR)main_window_proc);
        SetClassLongPtr(ui_hwnd[i], GCLP_HBRBACKGROUND, (LONG_PTR)bg_brush);
    }
#else
    // TODO
    SDL_AddEventWatch(sdl_win_resize_evt_watcher, NULL);
#endif

    for (int i = 0; i < SCREEN_COUNT; ++i) {
        ui_sdl_win_id[i] = SDL_GetWindowID(ui_sdl_win[i]);
    }

    SDL_ShowWindow(ui_sdl_win[SCREEN_TOP]);
    ui_view_mode_update(ui_view_mode);
    for (int i = 0; i < SCREEN_COUNT; ++i) {
        ui_window_size_update(i);
    }

    sock_startup();

    ntr_config_set_default();
    ntr_detect_3ds_ip();
    ntr_get_adapter_list();
    ntr_try_auto_select_adaptor();
    // TODO

    sock_cleanup();

#ifdef _WIN32
    DeleteObject(bg_brush);
    for (int i = 0; i < SCREEN_COUNT; ++i) {
        ui_hdc[i] = NULL;
        ui_hwnd[i] = NULL;
    }
#endif
}

int main(int argc, char **argv) {
    main_init();

    parse_args(argc, argv);

    for (int i = 0; i < renderer_count; ++i) {
        ui_renderer = renderer_list[i];
        bool done = 0;
        switch (ui_renderer) {
            default:
                break;

            case UI_RENDERER_SDL_HW:
            case UI_RENDERER_SDL_SW:
                if (ui_renderer_sdl_init()) {
                    ui_renderer_sdl_destroy();
                    break;
                }
                done = 1;
                break;
        }
        if (done)
            break;
    }

    main_windows();

    switch (ui_renderer) {
        default:
            break;

        case UI_RENDERER_SDL_HW:
        case UI_RENDERER_SDL_SW:
            ui_renderer_sdl_destroy();
            break;
    }

    main_destroy();

    return 0;
}
