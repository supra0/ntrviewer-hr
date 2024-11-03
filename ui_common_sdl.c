#include <SDL2/SDL.h>

#include "ui_common_sdl.h"
#include "ui_renderer_sdl.h"
#include "main.h"
#include "ikcp.h"

enum ui_renderer_t ui_renderer;
SDL_Window *ui_sdl_win[SCREEN_COUNT];
struct nk_context *ui_nk_ctx;
view_mode_t ui_view_mode;
bool ui_fullscreen;

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

event_t update_bottom_screen_evt;

int ui_common_sdl_init(void) {
    SDL_SetHint(SDL_HINT_VIDEO_HIGHDPI_DISABLED, "0");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_AWARENESS, "permonitorv2");
    SDL_SetHint(SDL_HINT_WINDOWS_DPI_SCALING, "1");
    SDL_SetHint(SDL_HINT_RENDER_VSYNC, "1");
#ifdef _WIN32
    SDL_SetHint(SDL_HINT_RENDER_DIRECT3D_THREADSAFE, "1");
    SDL_SetHint(SDL_HINT_WINDOWS_USE_D3D9EX, "1");
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "direct3d11");
    SDL_SetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION, "direct3d11");
#endif

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

    if (!renderer_single_thread && view_mode == VIEW_MODE_SEPARATE) {
        event_rel(&update_bottom_screen_evt);
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
#ifdef _WIN32
        RECT rect = {};
        GetClientRect(ui_hwnd[i], &rect);
        ui_win_drawable_width[i] = rect.right;
        ui_win_drawable_height[i] = rect.bottom;
#endif
    }

    float scale_x = (float)(ui_win_drawable_width[i]) / (float)(ui_win_width[i]);
    float scale_y = (float)(ui_win_drawable_height[i]) / (float)(ui_win_height[i]);
    scale_x = roundf(scale_x * ui_font_scale_step_factor) / ui_font_scale_step_factor;
    scale_y = roundf(scale_y * ui_font_scale_step_factor) / ui_font_scale_step_factor;
    ui_win_scale[i] = (scale_x + scale_y) * 0.5;

    if (i == SCREEN_TOP) {
        ui_nk_width = ui_win_width[i];
        ui_nk_height = ui_win_height[i];
        ui_nk_scale = ui_win_scale[i];
    }
}

#define FRAME_STAT_EVERY_X_US 1000000
static uint64_t windows_titles_last_tick;
#define WINDOW_TITLE_LEN_MAX 512

static double kcp_get_connection_quality(void)
{
    int input_count = __atomic_load_n(&kcp_input_count, __ATOMIC_RELAXED);
    double ret = input_count ? (double)__atomic_load_n(&kcp_recv_pid_count, __ATOMIC_RELAXED) / input_count : 0.0;
    return ret * ret * 100;
}

static void ui_kcp_window_title_update(SDL_Window *win, int tick_diff)
{
    char window_title[WINDOW_TITLE_LEN_MAX];
    snprintf(window_title, sizeof(window_title),
             WIN_TITLE " (FPS %03d %03d | %03d %03d)"
                       " (Connection Quality %.1f%%)"
                       " [Reliable Stream Mode]",
             __atomic_load_n(&frame_rate_decoded_tracker[SCREEN_TOP], __ATOMIC_RELAXED) * FRAME_STAT_EVERY_X_US / (int)tick_diff,
             __atomic_load_n(&frame_rate_decoded_tracker[SCREEN_BOT], __ATOMIC_RELAXED) * FRAME_STAT_EVERY_X_US / (int)tick_diff,
             __atomic_load_n(&frame_rate_displayed_tracker[SCREEN_TOP], __ATOMIC_RELAXED) * FRAME_STAT_EVERY_X_US / (int)tick_diff,
             __atomic_load_n(&frame_rate_displayed_tracker[SCREEN_BOT], __ATOMIC_RELAXED) * FRAME_STAT_EVERY_X_US / (int)tick_diff,
             kcp_get_connection_quality());
    SDL_SetWindowTitle(win, window_title);
}

static void ui_kcp_windows_titles_update(int ctx_top_bot, int screen_top_bot, int tick_diff)
{
    char window_title[WINDOW_TITLE_LEN_MAX];
    snprintf(window_title, sizeof(window_title),
             ctx_top_bot == SCREEN_TOP
                 ? WIN_TITLE
                 " (FPS %03d | %03d)"
                 " (Connection Quality %.1f%%)"
                 " [Reliable Stream Mode]"
                 : WIN_TITLE
                 " (FPS %03d | %03d)",
             __atomic_load_n(&frame_rate_decoded_tracker[screen_top_bot], __ATOMIC_RELAXED) * FRAME_STAT_EVERY_X_US / tick_diff,
             __atomic_load_n(&frame_rate_displayed_tracker[screen_top_bot], __ATOMIC_RELAXED) * FRAME_STAT_EVERY_X_US / tick_diff,
             kcp_get_connection_quality());
    SDL_SetWindowTitle(ui_sdl_win[ctx_top_bot], window_title);
}

void ui_windows_titles_update(void)
{
    uint64_t next_tick = iclock64();
    uint64_t tick_diff = next_tick - windows_titles_last_tick;
    if (tick_diff >= FRAME_STAT_EVERY_X_US)
    {
        int frame_fully_received = __atomic_load_n(&frame_fully_received_tracker, __ATOMIC_RELAXED);
        int frame_lost = __atomic_load_n(&frame_lost_tracker, __ATOMIC_RELAXED);
        double packet_rate = frame_fully_received ? (double)frame_fully_received / (frame_fully_received + frame_lost) * 100 : 0.0;

        int view_mode = __atomic_load_n(&ui_view_mode, __ATOMIC_RELAXED);

        if (view_mode == VIEW_MODE_TOP_BOT)
        {
            if (kcp_active)
            {
                ui_kcp_window_title_update(ui_sdl_win[SCREEN_TOP], (int)tick_diff);
            }
            else
            {
                char window_title[WINDOW_TITLE_LEN_MAX];
                snprintf(
                    window_title, sizeof(window_title),
                    WIN_TITLE
                    " (FPS %03d %03d | %03d %03d)"
                    " (Packet Rate %.1f%%)"
                    " [Compatibility Mode]",
                    __atomic_load_n(&frame_rate_decoded_tracker[SCREEN_TOP], __ATOMIC_RELAXED) * FRAME_STAT_EVERY_X_US / (int)tick_diff,
                    __atomic_load_n(&frame_rate_decoded_tracker[SCREEN_BOT], __ATOMIC_RELAXED) * FRAME_STAT_EVERY_X_US / (int)tick_diff,
                    __atomic_load_n(&frame_rate_displayed_tracker[SCREEN_TOP], __ATOMIC_RELAXED) * FRAME_STAT_EVERY_X_US / (int)tick_diff,
                    __atomic_load_n(&frame_rate_displayed_tracker[SCREEN_BOT], __ATOMIC_RELAXED) * FRAME_STAT_EVERY_X_US / (int)tick_diff,
                    packet_rate);
                SDL_SetWindowTitle(ui_sdl_win[SCREEN_TOP], window_title);
            }
        }
        else
        {
            for (int screen_top_bot = 0; screen_top_bot < SCREEN_COUNT; ++screen_top_bot)
            {
                int ctx_top_bot = screen_top_bot;
                if (view_mode == VIEW_MODE_BOT)
                {
                    ctx_top_bot = SCREEN_TOP;
                    screen_top_bot = SCREEN_BOT;
                }
                if (kcp_active)
                {
                    ui_kcp_windows_titles_update(ctx_top_bot, screen_top_bot, (int)tick_diff);
                }
                else
                {
                    char window_title[WINDOW_TITLE_LEN_MAX];
                    snprintf(
                        window_title, sizeof(window_title),
                        ctx_top_bot == SCREEN_TOP
                            ? WIN_TITLE
                            " (FPS %03d | %03d) "
                            " (Packet Rate %.1f%%)"
                            " [Compatibility Mode]"
                            : WIN_TITLE
                            " (FPS %03d | %03d) ",
                        __atomic_load_n(&frame_rate_decoded_tracker[screen_top_bot], __ATOMIC_RELAXED) * FRAME_STAT_EVERY_X_US / (int)tick_diff,
                        __atomic_load_n(&frame_rate_displayed_tracker[screen_top_bot], __ATOMIC_RELAXED) * FRAME_STAT_EVERY_X_US / (int)tick_diff,
                        packet_rate);
                    SDL_SetWindowTitle(ui_sdl_win[ctx_top_bot], window_title);
                }
                if (view_mode != VIEW_MODE_SEPARATE)
                {
                    break;
                }
            }
        }

        windows_titles_last_tick = next_tick;
        for (int top_bot = 0; top_bot < SCREEN_COUNT; ++top_bot)
        {
            __atomic_store_n(&frame_rate_decoded_tracker[top_bot], 0, __ATOMIC_RELAXED);
            __atomic_store_n(&frame_rate_displayed_tracker[top_bot], 0, __ATOMIC_RELAXED);
            __atomic_store_n(&frame_size_tracker[top_bot], 0, __ATOMIC_RELAXED);
            __atomic_store_n(&delay_between_packet_tracker[top_bot], 0, __ATOMIC_RELAXED);
        }
        __atomic_store_n(&kcp_input_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&kcp_input_fid_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&kcp_input_pid_count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&kcp_recv_pid_count, 0, __ATOMIC_RELAXED);

        __atomic_store_n(&frame_fully_received_tracker, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&frame_lost_tracker, 0, __ATOMIC_RELAXED);
    }
}

static void draw_screen_dispatch(struct rp_buffer_ctx_t *ctx, uint8_t *data, int width, int height, int screen_top_bot, int ctx_top_bot, int index, view_mode_t view_mode, bool win_shared) {
    if (is_renderer_sdl_renderer()) {
        ui_renderer_sdl_draw(data, width, height, screen_top_bot, ctx_top_bot, view_mode);
    }
    (void)ctx;
    (void)index;
    (void)win_shared;
    // TODO
}

int draw_screen(struct rp_buffer_ctx_t *ctx, int width, int height, int screen_top_bot, int ctx_top_bot, view_mode_t view_mode, bool win_shared) {
    sr_next(ctx_top_bot, screen_top_bot, ctx->index_display_2);

    rp_lock_wait(ctx->status_lock);
    enum frame_buffer_status_t status = ctx->status;
    if (ctx->status == FBS_UPDATED_2) {
        int index = ctx->index_ready_display_2;
        ctx->index_ready_display_2 = ctx->index_display_2;
        ctx->index_display_2 = ctx->index_display;
        ctx->index_display = index;
        ctx->status = FBS_UPDATED;
    } else if (ctx->status == FBS_UPDATED) {
        int index = ctx->index_ready_display;
        ctx->index_ready_display = ctx->index_display_2;
        ctx->index_display_2 = ctx->index_display;
        ctx->index_display = index;
        ctx->status = FBS_NOT_UPDATED;
    }
    int index_display = ctx->index_display;
    rp_lock_rel(ctx->status_lock);

    if (status == FBS_NOT_AVAIL)
        return 0;

    uint8_t *data = ctx->screen_decoded[index_display];
    ctx->prev_data = data;
    if (status >= FBS_UPDATED)
    {
        __atomic_add_fetch(&frame_rate_displayed_tracker[screen_top_bot], 1, __ATOMIC_RELAXED);
        draw_screen_dispatch(ctx, data, width, height, screen_top_bot, ctx_top_bot, index_display, view_mode, win_shared);
        return 1;
    }
    else
    {
        draw_screen_dispatch(ctx, NULL, width, height, screen_top_bot, ctx_top_bot, index_display, view_mode, win_shared);
        return -1;
    }
}
