#include "main.h"
#include "rp_syn.h"

#include "ui_common_sdl.h"
#include "ui_renderer_sdl.h"
#include "ntr_common.h"
#include "nuklear_d3d11.h"
#include "nuklear_sdl_gl3.h"
#include "nuklear_sdl_gles2.h"
#include "nuklear_sdl_renderer.h"
#include "ntr_hb.h"
#include "ntr_rp.h"
#include "ui_main_nk.h"

#include <SDL2/SDL_syswm.h>

#include <math.h>

atomic_bool program_running;

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
static event_t renderer_begin_evt;
static event_t renderer_end_evt;
bool renderer_single_thread;
bool renderer_evt_sync;

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

    ui_renderer = renderer_list[0];
}

static LRESULT CALLBACK main_window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
    int i = GetWindowLongPtrA(hwnd, GWLP_USERDATA);

    bool need_handle_input = 0;
    LPARAM handled_lparam = lparam;
    bool resize_top_and_ui = i == SCREEN_TOP;

    switch (msg) {
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;

        case WM_SIZE: {
            ui_win_drawable_width[i] = NK_MAX(LOWORD(lparam), 1);
            ui_win_drawable_height[i] = NK_MAX(HIWORD(lparam), 1);
            ui_win_width[i] = roundf(ui_win_drawable_width[i] / ui_win_scale[i]);
            ui_win_height[i] = roundf(ui_win_drawable_height[i] / ui_win_scale[i]);
            if (resize_top_and_ui) {
                ui_nk_width = ui_win_width[i];
                ui_nk_height = ui_win_height[i];
            }
            break;
        }

        case WM_DPICHANGED:
            ui_win_scale[i] = (float)HIWORD(wparam) / USER_DEFAULT_SCREEN_DPI;
            if (resize_top_and_ui) {
                ui_nk_scale = ui_win_scale[i];
            }
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
            handled_lparam = MAKELPARAM(x, y);
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
        // TODO input lock/input begin

        if (is_renderer_d3d11()) {
            nk_d3d11_handle_event(hwnd, msg, wparam, handled_lparam);
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

#define MIN_UPDATE_INTERVAL_US (33333)
static bool decode_cond_wait(event_t *event)
{
    int res = event_wait(event, MIN_UPDATE_INTERVAL_US * 1000);
    if (res == ETIMEDOUT) {
    } else if (res) {
        err_log("decode_cond_wait failed: %d\n", res);
        return false;
    }
    return true;
}

static void thread_loop(int i) {
    // TODO csc
    // int ctx_top_bot = i;
    int screen_top_bot = i;
    // bool win_shared = 0;

    int screen_count= SCREEN_COUNT;
    view_mode_t view_mode = __atomic_load_n(&ui_view_mode, __ATOMIC_RELAXED);
    if (view_mode != VIEW_MODE_SEPARATE) {
        screen_count = 1;
        // TODO csc
    }

    if (i >= screen_count) {
        if (!renderer_single_thread)
            event_wait(&update_bottom_screen_evt, NWM_THREAD_WAIT_NS);
        return;
    }

    if (renderer_single_thread) {
        if (i == SCREEN_TOP && !decode_cond_wait(&decode_updated_event))
            return;
    } else {
        int buf_top_bot = view_mode == VIEW_MODE_BOT ? SCREEN_BOT : screen_top_bot;
        if (!decode_cond_wait(&rp_buffer_ctx[buf_top_bot].decode_updated_event))
            return;
    }

    float bg[4];
    nk_color_fv(bg, nk_window_bgcolor);

    if (!renderer_single_thread && renderer_evt_sync)
        if (!cond_mutex_flag_lock(&renderer_begin_evt))
            return;

    if (is_renderer_sdl_renderer()) {
        ui_renderer_sdl_main(screen_top_bot, view_mode, bg);
    }

    // TODO

    if (is_renderer_sdl_renderer()) {
        ui_renderer_sdl_present(screen_top_bot);
    }

    if (!renderer_single_thread && renderer_evt_sync)
        cond_mutex_flag_signal(&renderer_end_evt);
}

static thread_ret_t window_thread_func(void *arg) {
    RO_INIT();

    int i = (int)(uintptr_t)arg;
    // TODO ogl
    while (program_running)
        thread_loop(i);
    // TODO csc

    RO_UNINIT();

    return (thread_ret_t)(uintptr_t)NULL;
}

static void main_loop(void) {
    // TODO csc

    SDL_Event evt;
    while (SDL_PollEvent(&evt))
    {
        if (
            evt.type == SDL_QUIT ||
            (evt.type == SDL_WINDOWEVENT && evt.window.event == SDL_WINDOWEVENT_CLOSE)
        ) {
            program_running = 0;
            return;
        } else if (
            evt.type == SDL_KEYDOWN &&
            evt.key.keysym.sym == SDLK_f
        ) {
            ui_fullscreen = !ui_fullscreen;
        } else if (
            evt.type == SDL_KEYDOWN &&
            evt.key.keysym.sym == SDLK_r
        ) {
#ifdef TDR_TEST_HOTKEY
#error TODO
#endif
        } else if (
            evt.type == SDL_KEYDOWN &&
            evt.key.keysym.sym == SDLK_t
        ) {
#ifdef TDR_TEST_HOTKEY
#error TODO
#endif
        } else {
            switch (evt.type) {
                case SDL_MOUSEMOTION:
                    if (evt.motion.windowID != ui_sdl_win_id[SCREEN_TOP]) {
                        goto skip_evt;
                    }
                    break;
                case SDL_MOUSEBUTTONDOWN:
                case SDL_MOUSEBUTTONUP:
                    if (evt.button.windowID != ui_sdl_win_id[SCREEN_TOP]) {
                        goto skip_evt;
                    }
                    break;
                case SDL_MOUSEWHEEL:
                    if (evt.wheel.windowID != ui_sdl_win_id[SCREEN_TOP]) {
                        goto skip_evt;
                    }
                    break;
                case SDL_KEYDOWN:
                    switch (evt.key.keysym.sym) {
                        case SDLK_TAB: {
                            const Uint8 *state = SDL_GetKeyboardState(0);
                            __atomic_store_n(&nk_nav_cmd, state[SDL_SCANCODE_LSHIFT] || state[SDL_SCANCODE_RSHIFT] ? NK_NAV_PREVIOUS : NK_NAV_NEXT, __ATOMIC_RELAXED);
                            goto skip_evt;
                        }

                        case SDLK_SPACE:
                        case SDLK_RETURN:
                        case SDLK_KP_ENTER:
                            __atomic_store_n(&nk_nav_cmd, NK_NAV_CONFIRM, __ATOMIC_RELAXED);
                            goto skip_evt;

                        case SDLK_ESCAPE:
                            __atomic_store_n(&nk_nav_cmd, NK_NAV_CANCEL, __ATOMIC_RELAXED);
                            goto skip_evt;
                        }
                    break;
            }

            // TODO input lock/input begin
            if (!is_renderer_d3d11()) {
                if (is_renderer_ogl()) {
                    nk_sdl_gl3_handle_event(&evt);
                } else if (is_renderer_gles()) {
                    nk_sdl_gles2_handle_event(&evt);
                } else if (is_renderer_sdl_renderer()) {
                    nk_sdl_renderer_handle_event(&evt);
                }
            }
skip_evt:
        }
    }

    if (renderer_single_thread) {
        for (int i = 0; i < SCREEN_COUNT; ++i)
            thread_loop(i);
    } else if (renderer_evt_sync) {
        cond_mutex_flag_signal(&renderer_begin_evt);
        if (!cond_mutex_flag_lock(&renderer_end_evt))
            return;
    }
}

static void main_ntr(void) {
#ifdef _WIN32
    socket_startup();
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_AWAYMODE_REQUIRED | ES_DISPLAY_REQUIRED);
#endif

    rp_buffer_init();

    ntr_config_set_default();
    ntr_detect_3ds_ip();
    ntr_get_adapter_list();
    ntr_try_auto_select_adaptor();

    program_running = true;
    int ret;

    thread_t udp_recv_thread;
    if ((ret = thread_create(udp_recv_thread, udp_recv_thread_func, NULL)))
    {
        err_log("udp_recv_thread create failed\n");
        program_running = false;
        goto join_udp_recv;
    }
    thread_t menu_tcp_thread;
    struct tcp_thread_arg menu_tcp_thread_arg = {
        &menu_work_state,
        &menu_remote_play,
        8000,
    };
    if ((ret = thread_create(menu_tcp_thread, tcp_thread_func, &menu_tcp_thread_arg)))
    {
        err_log("menu_tcp_thread create failed\n");
        program_running = false;
        goto join_menu_tcp;
    }
    thread_t nwm_tcp_thread;
    struct tcp_thread_arg nwm_tcp_thread_arg = {
        &nwm_work_state,
        NULL,
        5000 + 0x1a,
    };
    if ((ret = thread_create(nwm_tcp_thread, tcp_thread_func, &nwm_tcp_thread_arg)))
    {
        err_log("nwm_tcp_thread create failed\n");
        program_running = false;
        goto join_nwm_tcp;
    }

    thread_t window_top_thread = 0;
    thread_t window_bot_thread = 0;

    if (!renderer_single_thread) {
        if ((ret = thread_create(window_top_thread, window_thread_func, (void *)SCREEN_TOP)))
        {
            err_log("window_top_thread create failed\n");
            program_running = false;
            goto join_win_top;
        }
        if ((ret = thread_create(window_bot_thread, window_thread_func, (void *)SCREEN_BOT)))
        {
            err_log("window_bot_thread create failed\n");
            program_running = false;
            goto join_win_bot;
        }
    }

    while (program_running)
        main_loop();

    if (!renderer_single_thread) {
        thread_join(window_bot_thread);
join_win_bot:
        thread_join(window_top_thread);
join_win_top:
    }

#ifndef _WIN32
    thread_cancel(nwm_tcp_thread);
    thread_cancel(menu_tcp_thread);
    thread_cancel(udp_recv_thread);
#endif
    thread_join(nwm_tcp_thread);
join_nwm_tcp:
    thread_join(menu_tcp_thread);
join_menu_tcp:
    thread_join(udp_recv_thread);
join_udp_recv:

    rp_buffer_destroy();

#ifdef _WIN32
    SetThreadExecutionState(ES_CONTINUOUS);
    socket_shutdown();
#endif
}

static void main_windows(void) {
    for (int i = 0; i < SCREEN_COUNT; ++i) {
        ui_win_scale[i] = 1.0f;
    }
    event_init(&renderer_begin_evt);
    event_init(&renderer_end_evt);

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

    event_init(&update_bottom_screen_evt);

    SDL_ShowWindow(ui_sdl_win[SCREEN_TOP]);
    ui_view_mode_update(ui_view_mode);
    for (int i = 0; i < SCREEN_COUNT; ++i) {
        ui_window_size_update(i);
    }

    main_ntr();

    event_close(&update_bottom_screen_evt);

#ifdef _WIN32
    DeleteObject(bg_brush);
    for (int i = 0; i < SCREEN_COUNT; ++i) {
        ui_hdc[i] = NULL;
        ui_hwnd[i] = NULL;
    }
#endif
    event_close(&renderer_end_evt);
    event_close(&renderer_begin_evt);
}

int main(int argc, char **argv) {
    main_init();

    parse_args(argc, argv);
    if (ui_common_sdl_init()) {
        return -1;
    }

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

    ui_common_sdl_destroy();

    main_destroy();

    return 0;
}

#ifdef _WIN32
static bool qpc_mode = 0;
static int64_t qpc_freq = 1;
#endif
void itimeofday(int64_t *sec, int64_t *usec)
{
#ifdef _WIN32
    int64_t qpc;
    if (__atomic_load_n(&qpc_mode, __ATOMIC_ACQUIRE) == 0)
    {
        if (!QueryPerformanceFrequency((LARGE_INTEGER *)&qpc_freq))
        {
            program_running = 0;
            *sec = *usec = 0;
            return;
        }
        qpc_freq = (qpc_freq == 0) ? 1 : qpc_freq;
        __atomic_store_n(&qpc_mode, 1, __ATOMIC_RELEASE);
    }
    if (!QueryPerformanceCounter((LARGE_INTEGER *)&qpc))
    {
        program_running = 0;
        *sec = *usec = 0;
        return;
    }
    if (sec)
        *sec = (int64_t)(qpc / qpc_freq);
    if (usec)
        *usec = (int64_t)((qpc % qpc_freq) * 1000000 / qpc_freq);
#else
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) < 0)
    {
        running = 0;
        *sec = *usec = 0;
        return;
    }
    if (sec)
        *sec = ts.tv_sec;
    if (usec)
        *usec = ts.tv_nsec / 1000;
#endif
}
