#include "ui_main_nk.h"
#include "ui_common_sdl.h"
#include "ntr_common.h"
#include "ntr_hb.h"
#include "ntr_rp.h"

enum nk_nav_t nk_nav_cmd;

#include "style.h"
#include "web_colors.h"

#define UI_MSG_BUF_LEN_MAX (256)

static struct nk_style nk_style_current;

#include "nuklear_sdl_renderer.h"
#include "nuklear_sdl_gl3.h"
#include "nuklear_sdl_gles2.h"
#ifdef _WIN32
#include "nuklear_d3d11.h"
#include "ui_compositor_csc.h"
#endif

#include <limits.h>

void nk_font_stash_begin(struct nk_font_atlas **atlas) {
    if (is_renderer_d3d11()) {
#ifndef USE_SDL_RENDERER_ONLY
#ifdef _WIN32
        nk_d3d11_font_stash_begin(atlas);
#endif
#endif
    } else if (is_renderer_ogl()) {
#ifndef USE_SDL_RENDERER_ONLY
        nk_sdl_gl3_font_stash_begin(atlas);
#endif
    } else if (is_renderer_gles()) {
#ifndef USE_SDL_RENDERER_ONLY
        nk_sdl_gles2_font_stash_begin(atlas);
#endif
    } else if (is_renderer_sdl_renderer()) {
        nk_sdl_renderer_font_stash_begin(atlas);
    }
}

void nk_font_stash_end(void) {
    if (is_renderer_d3d11()) {
#ifndef USE_SDL_RENDERER_ONLY
#ifdef _WIN32
        nk_d3d11_font_stash_end();
#endif
#endif
    } else if (is_renderer_ogl()) {
#ifndef USE_SDL_RENDERER_ONLY
        nk_sdl_gl3_font_stash_end();
#endif
    } else if (is_renderer_gles()) {
#ifndef USE_SDL_RENDERER_ONLY
        nk_sdl_gles2_font_stash_end();
#endif
    } else if (is_renderer_sdl_renderer()) {
        nk_sdl_renderer_font_stash_end();
    }
}

void nk_backend_font_init(void)
{
    /* Load Fonts: if none of these are loaded a default font will be used  */
    /* Load Cursor: if you uncomment cursor loading please hide the cursor */
    {
        struct nk_font_atlas *atlas;

        nk_font_stash_begin(&atlas);
        nk_font_stash_end();

        // nk_style_load_all_cursors(ui_nk_ctx, atlas->cursors);
        // nk_style_set_font(ui_nk_ctx, &roboto->handle);
    }

    // set_style(ui_nk_ctx, THEME_WHITE);
    // set_style(ui_nk_ctx, THEME_RED);
    // set_style(ui_nk_ctx, THEME_BLUE);
    set_style(ui_nk_ctx, THEME_DARK);

    web_colors_init(ui_nk_ctx);
    web_colors_add(ui_nk_ctx);

    nk_style_current = ui_nk_ctx->style;
}

atomic_bool ui_hide_nk_windows;
bool ui_upscaling_filters;

int ui_upscaling_selected;
const char **ui_upscaling_filter_options;
int ui_upscaling_filter_count;

static const char *nk_property_name = "#";
static enum NK_FOCUS {
    NK_FOCUS_NONE,
    NK_FOCUS_VIEW_MODE,
    NK_FOCUS_UPSCALING_FILTER,
    NK_FOCUS_IP_OCTET_0,
    NK_FOCUS_IP_OCTET_1,
    NK_FOCUS_IP_OCTET_2,
    NK_FOCUS_IP_OCTET_3,
    NK_FOCUS_IP_AUTO_DETECT,
    NK_FOCUS_IP_COMBO,
    NK_FOCUS_VIEWER_IP,
    NK_FOCUS_VIEWER_PORT,
    NK_FOCUS_PRIORITY_SCREEN,
    NK_FOCUS_PRIORITY_FACTOR,
    NK_FOCUS_QUALITY,
    NK_FOCUS_BANDWIDTH_LIMIT,
    NK_FOCUS_RELIABLE_STREAM,
    NK_FOCUS_DEFAULT,
    NK_FOCUS_CONNECT,
    NK_FOCUS_MIN = NK_FOCUS_VIEW_MODE,
    NK_FOCUS_MAX = NK_FOCUS_CONNECT,
} nk_focus_current;

static enum NK_NAV_FOCUS {
    NK_NAV_FOCUS_NONE,
    NK_NAV_FOCUS_NORMAL,
    NK_NAV_FOCUS_NAV,
} nk_nav_focus;

static const char *const remote_play_wnd = "Remote Play";
static const char *const debug_msg_wnd = "Debug";
static const char *const background_wnd = "Background";

static const char *connection_msg[CONNECTION_STATE_COUNT] = {
    "+",
    "...",
    "-",
    ".",
};

// HACK
// Try to get Nuklear to accept keyboard navigation

static nk_hash nk_hash_from_name_prev(const char *name, struct nk_window *win, int prev)
{
    // copied from nuklear.h since I don't want to edit that file
    if (name[0] == '#')
    {
        return nk_murmur_hash(name, (int)nk_strlen(name), win->property.seq - prev);
    }
    else
        return nk_murmur_hash(name, (int)nk_strlen(name), 42);
}

static nk_hash nk_hash_from_name(const char *name, struct nk_window *win)
{
    return nk_hash_from_name_prev(name, win, 0);
}

NK_LIB char *nk_itoa(char *s, long n);
static void focus_next_property(struct nk_context *ctx, const char *name, int val)
{
    struct nk_window *win = ctx->current;
    nk_hash hash = nk_hash_from_name(name, win);

    win->property.active = 1;
    nk_itoa(win->property.buffer, val);
    win->property.length = nk_strlen(win->property.buffer);
    win->property.cursor = 0;
    win->property.state = 1 /* NK_PROPERTY_EDIT */;
    win->property.name = hash;
    win->property.select_start = 0;
    win->property.select_end = win->property.length;
}

static void cancel_next_property(struct nk_context *ctx)
{
    struct nk_window *win = ctx->current;

    if (win->property.active && win->property.state == 0 /* NK_PROPERTY_DEFAULT */)
    {
        win->property.active = 0;
        win->property.buffer[0] = 0;
        win->property.length = 0;
        win->property.cursor = 0;
        win->property.state = 0;
        win->property.name = 0;
        win->property.select_start = 0;
        win->property.select_end = 0;
    }
}

static void confirm_next_property(struct nk_context *ctx)
{
    nk_input_key(ctx, NK_KEY_ENTER, nk_true);
}

static nk_bool check_next_property(struct nk_context *ctx, const char *name)
{
    struct nk_window *win = ctx->current;
    nk_hash hash = nk_hash_from_name(name, win);
    return win->property.active && win->property.name == hash;
}

static void do_nav_next(enum NK_FOCUS nk_focus)
{
    if (nk_focus == nk_focus_current)
    {
        switch (__atomic_load_n(&nk_nav_cmd, __ATOMIC_RELAXED))
        {
        case NK_NAV_PREVIOUS:
            if (nk_focus_current <= NK_FOCUS_MIN)
                nk_focus_current = NK_FOCUS_MAX;
            else
                --nk_focus_current;

            if (!ui_upscaling_filters && nk_focus_current == NK_FOCUS_UPSCALING_FILTER)
                --nk_focus_current;

            nk_nav_focus = NK_NAV_FOCUS_NAV;
            break;

        case NK_NAV_NEXT:
            if (nk_focus_current >= NK_FOCUS_MAX)
                nk_focus_current = NK_FOCUS_MIN;
            else
                ++nk_focus_current;

            if (!ui_upscaling_filters && nk_focus_current == NK_FOCUS_UPSCALING_FILTER)
                ++nk_focus_current;

            nk_nav_focus = NK_NAV_FOCUS_NAV;
            break;

        case NK_NAV_CANCEL:
            if (nk_nav_focus == NK_NAV_FOCUS_NONE)
                ui_hide_nk_windows = 1;
            else
                nk_nav_focus = NK_NAV_FOCUS_NONE;
            break;

        case NK_NAV_CONFIRM:
            if (nk_focus == NK_FOCUS_NONE)
            {
                nk_nav_focus = NK_NAV_FOCUS_NONE;
            }
            else
            {
                nk_nav_focus = nk_nav_focus == NK_NAV_FOCUS_NONE ? NK_NAV_FOCUS_NAV : NK_NAV_FOCUS_NONE;
            }
            break;

        default:
            break;
        }
        __atomic_store_n(&nk_nav_cmd, NK_NAV_NONE, __ATOMIC_RELAXED);
    }
}

// HACK always allow property text edit input in current window
static nk_flags nav_layout_rom;

static void do_nav_property_next(struct nk_context *ctx, const char *name, enum NK_FOCUS nk_focus, int val)
{
    if (check_next_property(ctx, name))
    {
        if (nk_nav_focus == NK_NAV_FOCUS_NAV)
        {
            cancel_next_property(ctx);
        }
        else
        {
            nk_focus_current = nk_focus;
            nk_nav_focus = NK_NAV_FOCUS_NORMAL;
        }
    }
    else if (nk_focus_current == nk_focus && nk_nav_focus != NK_NAV_FOCUS_NONE)
    {
        focus_next_property(ctx, name, val);
        nk_nav_focus = NK_NAV_FOCUS_NORMAL;
    }

    if (nk_focus_current == nk_focus && nk_nav_focus != NK_NAV_FOCUS_NONE)
    {
        nav_layout_rom = ctx->current->layout->flags & NK_WINDOW_ROM;
        if (nav_layout_rom)
        {
            ctx->current->layout->flags &= ~NK_WINDOW_ROM;
        }

        switch (__atomic_load_n(&nk_nav_cmd, __ATOMIC_RELAXED))
        {
        case NK_NAV_PREVIOUS:
        case NK_NAV_NEXT:
        case NK_NAV_CONFIRM:
            confirm_next_property(ctx);
            break;

        case NK_NAV_CANCEL:
            cancel_next_property(ctx);
            break;

        default:
            break;
        }
    }

    do_nav_next(nk_focus);
}

static void check_nav_property_prev(struct nk_context *ctx, const char *name, enum NK_FOCUS nk_focus)
{
    struct nk_window *win = ctx->current;
    if (win->property.active)
    {
        nk_hash hash = nk_hash_from_name_prev(name, win, 1);
        if (win->property.name == hash)
        {
            nk_focus_current = nk_focus;
            nk_nav_focus = NK_NAV_FOCUS_NORMAL;
        }
    }
    else if (nk_nav_focus != NK_NAV_FOCUS_NAV)
    {
        nk_nav_focus = NK_NAV_FOCUS_NONE;
    }
    ctx->input.keyboard.keys[NK_KEY_ENTER].clicked = 0;

    if (nav_layout_rom)
    {
        ctx->current->layout->flags |= NK_WINDOW_ROM;
        nav_layout_rom = 0;
    }
}

static void do_nav_combobox_next(struct nk_context *ctx, enum NK_FOCUS nk_focus, int *selected, int count)
{
    if (nk_focus_current == nk_focus && nk_nav_focus != NK_NAV_FOCUS_NONE)
    {
        ctx->style.combo.border_color = ctx->style.text.color;
        if (nk_input_is_key_pressed(&ctx->input, NK_KEY_DOWN))
        {
            ++*selected;
            if (*selected >= count)
            {
                *selected = 0;
            }
        }
        else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_UP))
        {
            --*selected;
            if (*selected < 0)
            {
                *selected = count - 1;
            }
        }
    }

    do_nav_next(nk_focus);
}

static void set_nav_combobox_prev(enum NK_FOCUS nk_focus)
{
    nk_nav_focus = NK_NAV_FOCUS_NAV;
    nk_focus_current = nk_focus;
}

static void check_nav_combobox_prev(struct nk_context *ctx)
{
    ctx->style.combo.border_color = nk_style_current.combo.border_color;
}

static bool do_nav_button_next(struct nk_context *ctx, enum NK_FOCUS nk_focus)
{
    bool ret = false;
    if (nk_focus_current == nk_focus && nk_nav_focus != NK_NAV_FOCUS_NONE)
    {
        ctx->style.button.border_color = ctx->style.text.color;
        if (__atomic_load_n(&nk_nav_cmd, __ATOMIC_RELAXED) == NK_NAV_CONFIRM)
        {
            ret = true;
        }
    }

    if (ret)
    {
        __atomic_store_n(&nk_nav_cmd, NK_NAV_NONE, __ATOMIC_RELAXED);
    }
    else
    {
        do_nav_next(nk_focus);
    }

    return ret;
}

static void set_nav_button_prev(enum NK_FOCUS nk_focus)
{
    nk_nav_focus = NK_NAV_FOCUS_NAV;
    nk_focus_current = nk_focus;
}

static void check_nav_button_prev(struct nk_context *ctx)
{
    ctx->style.button.border_color = nk_style_current.button.border_color;
}

static nk_bool nk_nav_checkbox_val_current;
static void do_nav_checkbox_next(struct nk_context *ctx, enum NK_FOCUS nk_focus, nk_bool *val)
{
    bool ret = false;
    if (nk_focus_current == nk_focus && nk_nav_focus != NK_NAV_FOCUS_NONE)
    {
        ctx->style.checkbox.cursor_hover.data.color = ctx->style.text.color;
        ctx->style.checkbox.cursor_normal.data.color = ctx->style.text.color;
        ctx->style.checkbox.border = 1.0f;
        ctx->style.checkbox.border_color = ctx->style.text.color;
        if (__atomic_load_n(&nk_nav_cmd, __ATOMIC_RELAXED) == NK_NAV_CONFIRM)
        {
            ret = true;
            *val = !*val;
        }
    }

    if (ret)
    {
        __atomic_store_n(&nk_nav_cmd, NK_NAV_NONE, __ATOMIC_RELAXED);
    }
    else
    {
        do_nav_next(nk_focus);
    }

    nk_nav_checkbox_val_current = *val;
}

static void check_nav_checkbox_prev(struct nk_context *ctx, enum NK_FOCUS nk_focus, nk_bool val)
{
    ctx->style.checkbox.border_color = nk_style_current.checkbox.border_color;
    ctx->style.checkbox.border = nk_style_current.checkbox.border;
    ctx->style.checkbox.cursor_normal.data.color = nk_style_current.checkbox.cursor_normal.data.color;
    ctx->style.checkbox.cursor_hover.data.color = nk_style_current.checkbox.cursor_hover.data.color;

    if (nk_nav_checkbox_val_current != val)
    {
        nk_nav_focus = NK_NAV_FOCUS_NAV;
        nk_focus_current = nk_focus;
    }
}

static int nk_nav_slider_val_current;
static void do_nav_slider_next(struct nk_context *ctx, enum NK_FOCUS nk_focus, int *val)
{
    if (nk_focus_current == nk_focus && nk_nav_focus != NK_NAV_FOCUS_NONE)
    {
        ctx->style.slider.border = 1.0f;
        ctx->style.slider.border_color = ctx->style.text.color;

        if (nk_input_is_key_pressed(&ctx->input, NK_KEY_RIGHT))
        {
            ++*val;
        }
        else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_LEFT))
        {
            --*val;
        }
        else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_DOWN))
        {
            *val += 5;
        }
        else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_UP))
        {
            *val -= 5;
        }
        else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_START))
        {
            *val = 0;
        }
        else if (nk_input_is_key_pressed(&ctx->input, NK_KEY_SCROLL_END))
        {
            *val = INT_MAX;
        }
    }

    do_nav_next(nk_focus);
    nk_nav_slider_val_current = *val;
}

static void check_nav_slider_prev(struct nk_context *ctx, enum NK_FOCUS nk_focus, int val)
{
    ctx->style.slider.border_color = nk_style_current.slider.border_color;
    ctx->style.slider.border = nk_style_current.slider.border;

    if (nk_nav_slider_val_current != val)
    {
        nk_nav_focus = NK_NAV_FOCUS_NAV;
        nk_focus_current = nk_focus;
    }
}

void ui_main_nk(void)
{
    struct nk_context *ctx = ui_nk_ctx;

    int focus_window = 0;
    ctx->style.window.fixed_background = nk_style_item_hide();
    if (nk_begin(ctx, background_wnd, nk_rect(0, 0, ui_win_width[SCREEN_TOP], ui_win_height[SCREEN_TOP]),
                 NK_WINDOW_BACKGROUND))
    {
        if (nk_window_is_hovered(ctx) && nk_window_is_active(ctx, background_wnd) &&
            nk_input_has_mouse_click(&ctx->input, NK_BUTTON_LEFT))
        {
            ui_hide_nk_windows = !ui_hide_nk_windows;
            if (!ui_hide_nk_windows)
                focus_window = 1;
        }
    }
    nk_end(ctx);
    ctx->style.window.fixed_background = nk_style_current.window.fixed_background;

    int nav_command = __atomic_load_n(&nk_nav_cmd, __ATOMIC_RELAXED);
    if (ui_hide_nk_windows && (nav_command == NK_NAV_CANCEL || nav_command == NK_NAV_CONFIRM))
    {
        ui_hide_nk_windows = 0;
        __atomic_store_n(&nk_nav_cmd, NK_NAV_NONE, __ATOMIC_RELAXED);
        focus_window = 1;
    }

    enum nk_show_states show_window = !ui_hide_nk_windows;

    char msg_buf[UI_MSG_BUF_LEN_MAX];

    if (nk_begin(ctx, remote_play_wnd, nk_rect(25, 10, 450, 505),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_SCALABLE | NK_WINDOW_TITLE) &&
        show_window)
    {
        do_nav_next(NK_FOCUS_NONE);

        nk_layout_row_dynamic(ctx, 30, 2);
        nk_label(ctx, "View Mode", NK_TEXT_CENTERED);
        int selected = ui_view_mode;
        struct nk_vec2 combo_size = {225, 200};
        const char *view_mode_options[] = {
            "Top and Bottom",
            "Separate Windows",
            "Top Only",
            "Bottom Only"};
        do_nav_combobox_next(ctx, NK_FOCUS_VIEW_MODE, &selected, sizeof(view_mode_options) / sizeof(*view_mode_options));
        nk_combobox(ctx, view_mode_options, sizeof(view_mode_options) / sizeof(*view_mode_options), &selected, 30, combo_size);
        check_nav_combobox_prev(ctx);
        if (selected != (int)ui_view_mode)
        {
            set_nav_combobox_prev(NK_FOCUS_VIEW_MODE);
            __atomic_store_n(&ui_view_mode, selected, __ATOMIC_RELAXED);
            ui_fullscreen = 0;
        }

        if (ui_upscaling_filters) {
            nk_draw_push_color_inline(ctx, NK_COLOR_INLINE_TAG);
            nk_layout_row_dynamic(ctx, 30, 2);
            nk_label(ctx, "Upscaling Filter", NK_TEXT_CENTERED);
            selected = ui_upscaling_selected;
            do_nav_combobox_next(ctx, NK_FOCUS_UPSCALING_FILTER, &selected, ui_upscaling_filter_count);
            nk_combobox(ctx, ui_upscaling_filter_options, ui_upscaling_filter_count, &selected, 30, combo_size);
            check_nav_combobox_prev(ctx);
            if (selected != ui_upscaling_selected) {
                set_nav_combobox_prev(NK_FOCUS_UPSCALING_FILTER);
                ui_upscaling_selected = selected;
            }
            nk_draw_pop_color_inline(ctx);
        }

        nk_layout_row_dynamic(ctx, 30, 5);
        nk_label(ctx, "3DS IP", NK_TEXT_CENTERED);

        for (int i = 0; i < 4; ++i)
        {
            int ip_octet = ntr_ip_octet[i];
            do_nav_property_next(ctx, nk_property_name, NK_FOCUS_IP_OCTET_0 + i, ip_octet);
            nk_property_int(ctx, nk_property_name, 0, &ip_octet, 255, 1, 1);
            check_nav_property_prev(ctx, nk_property_name, NK_FOCUS_IP_OCTET_0 + i);
            if (ip_octet != ntr_ip_octet[i])
            {
                ntr_ip_octet[i] = ip_octet;
                strcpy(ntr_auto_ip_list[0], "Manual");
                ntr_selected_ip = 0;
            }
        }

        nk_layout_row_dynamic(ctx, 30, 2);
        bool button_ret;
        button_ret = do_nav_button_next(ctx, NK_FOCUS_IP_AUTO_DETECT);
        if (nk_button_label(ctx, "Auto-Detect") || button_ret)
        {
            ntr_detect_3ds_ip();
            ntr_try_auto_select_adapter();
            set_nav_button_prev(NK_FOCUS_IP_AUTO_DETECT);
        }
        check_nav_button_prev(ctx);
        selected = ntr_selected_ip;
        do_nav_combobox_next(ctx, NK_FOCUS_IP_COMBO, &selected, ntr_auto_ip_count);
        const char *combo_items_null;
        if (ntr_auto_ip_list)
            nk_combobox(ctx, (const char **)ntr_auto_ip_list, ntr_auto_ip_count, &selected, 30, combo_size);
        else
            nk_combobox(ctx, &combo_items_null, 0, &selected, 30, combo_size);
        check_nav_combobox_prev(ctx);
        if (selected != ntr_selected_ip)
        {
            set_nav_combobox_prev(NK_FOCUS_IP_COMBO);
            ntr_selected_ip = selected;
            if (ntr_selected_ip)
            {
                memcpy(ntr_ip_octet, ntr_auto_ip_octet_list[ntr_selected_ip], 4);
                ntr_try_auto_select_adapter();
            }
        }

        nk_layout_row_dynamic(ctx, 30, 2);
        nk_label(ctx, "Viewer IP", NK_TEXT_CENTERED);
        selected = ntr_selected_adapter;
        do_nav_combobox_next(ctx, NK_FOCUS_VIEWER_IP, &selected, ntr_adapter_count);
        if (ntr_adapter_list)
            nk_combobox(ctx, (const char **)ntr_adapter_list, ntr_adapter_count, &selected, 30, combo_size);
        else
            nk_combobox(ctx, &combo_items_null, 0, &selected, 30, combo_size);
        check_nav_combobox_prev(ctx);
        if (selected != ntr_selected_adapter)
        {
            set_nav_combobox_prev(NK_FOCUS_VIEWER_IP);
            ntr_selected_adapter = selected;
            if (ntr_selected_adapter == ntr_adapter_count - NTR_ADAPTER_POST_COUNT + NTR_ADAPTER_POST_AUTO)
            {
                ntr_try_auto_select_adapter();
            }
            else if (ntr_selected_adapter == ntr_adapter_count - NTR_ADAPTER_POST_COUNT + NTR_ADAPTER_POST_REFRESH)
            {
                ntr_get_adapter_list();
            } else {
                ntr_rp_port_changed = 1;
                kcp_restart = 1;
            }
        }

        nk_layout_row_dynamic(ctx, 30, 2);
        nk_label(ctx, "Viewer Port", NK_TEXT_CENTERED);
        do_nav_property_next(ctx, nk_property_name, NK_FOCUS_VIEWER_PORT, ntr_rp_port);
        nk_property_int(ctx, nk_property_name, 1024, &ntr_rp_port, 65535, 1, 1);
        check_nav_property_prev(ctx, nk_property_name, NK_FOCUS_VIEWER_PORT);
        if (ntr_rp_port_bound != ntr_rp_port)
        {
            ntr_rp_port_bound = ntr_rp_port;
            ntr_rp_port_changed = 1;
            kcp_restart = 1;
        }

        nk_layout_row_dynamic(ctx, 30, 1);
        nk_label(ctx, "Press \"F\" to toggle fullscreen.", NK_TEXT_CENTERED);

        nk_layout_row_dynamic(ctx, 30, 2);
        nk_label(ctx, "Prioritize Top Screen", NK_TEXT_CENTERED);
        do_nav_checkbox_next(ctx, NK_FOCUS_PRIORITY_SCREEN, &ntr_rp_config.top_screen_priority);
        nk_checkbox_label(ctx, "", &ntr_rp_config.top_screen_priority);
        check_nav_checkbox_prev(ctx, NK_FOCUS_PRIORITY_SCREEN, ntr_rp_config.top_screen_priority);

        nk_layout_row_dynamic(ctx, 30, 2);
        nk_label(ctx, "Priority Screen Factor", NK_TEXT_CENTERED);
        do_nav_property_next(ctx, nk_property_name, NK_FOCUS_PRIORITY_FACTOR, ntr_rp_config.screen_priority_factor);
        nk_property_int(ctx, nk_property_name, 0, &ntr_rp_config.screen_priority_factor, 255, 1, 1);
        check_nav_property_prev(ctx, nk_property_name, NK_FOCUS_PRIORITY_FACTOR);

        nk_layout_row_dynamic(ctx, 30, 2);
        snprintf(msg_buf, sizeof(msg_buf), "JPEG Quality %d", ntr_rp_config.jpeg_quality);
        nk_label(ctx, msg_buf, NK_TEXT_CENTERED);
        do_nav_slider_next(ctx, NK_FOCUS_QUALITY, &ntr_rp_config.jpeg_quality);
        nk_slider_int(ctx, 10, &ntr_rp_config.jpeg_quality, 100, 1);
        check_nav_slider_prev(ctx, NK_FOCUS_QUALITY, ntr_rp_config.jpeg_quality);

        nk_layout_row_dynamic(ctx, 30, 2);
        snprintf(msg_buf, sizeof(msg_buf), "Bandwidth Limit %d Mbps", ntr_rp_config.bandwidth_limit);
        nk_label(ctx, msg_buf, NK_TEXT_CENTERED);
        do_nav_slider_next(ctx, NK_FOCUS_BANDWIDTH_LIMIT, &ntr_rp_config.bandwidth_limit);
        nk_slider_int(ctx, 4, &ntr_rp_config.bandwidth_limit, 20, 1);
        check_nav_slider_prev(ctx, NK_FOCUS_BANDWIDTH_LIMIT, ntr_rp_config.bandwidth_limit);

        nk_layout_row_dynamic(ctx, 30, 2);
        nk_label(ctx, "Reliable Stream", NK_TEXT_CENTERED);
        selected = ntr_rp_config.kcp_mode;
        combo_size = (struct nk_vec2){225, 100};
        const char *reliable_stream_options[] = {
            "Off",
            "On",
        };
        do_nav_combobox_next(ctx, NK_FOCUS_RELIABLE_STREAM, &selected, sizeof(reliable_stream_options) / sizeof(*reliable_stream_options));
        nk_combobox(ctx, reliable_stream_options, sizeof(reliable_stream_options) / sizeof(*reliable_stream_options), &selected, 30, combo_size);
        check_nav_combobox_prev(ctx);
        if (selected != (int)ntr_rp_config.kcp_mode)
        {
            set_nav_combobox_prev(NK_FOCUS_RELIABLE_STREAM);
            ntr_rp_config.kcp_mode = selected;
        }

        nk_layout_row_dynamic(ctx, 30, 2);
        button_ret = do_nav_button_next(ctx, NK_FOCUS_DEFAULT);
        if (nk_button_label(ctx, "Default") || button_ret)
        {
            ntr_config_set_default();
            set_nav_button_prev(NK_FOCUS_DEFAULT);
        }
        check_nav_button_prev(ctx);

        button_ret = do_nav_button_next(ctx, NK_FOCUS_CONNECT);
        if (nk_button_label(ctx, "Connect") || button_ret)
        {
            set_nav_button_prev(NK_FOCUS_CONNECT);
            menu_remote_play = 1;
            if (menu_work_state == CONNECTION_STATE_DISCONNECTED)
            {
                menu_work_state = CONNECTION_STATE_CONNECTING;
            }
            kcp_restart = 1;
        }
        check_nav_button_prev(ctx);
    }
    nk_end(ctx);
    nk_window_show(ctx, remote_play_wnd, show_window);

    if (focus_window)
        nk_window_set_focus(ctx, remote_play_wnd);

    if (nk_begin(ctx, debug_msg_wnd, nk_rect(475, 10, 150, 250),
                 NK_WINDOW_BORDER | NK_WINDOW_MOVABLE | NK_WINDOW_TITLE) &&
        show_window)
    {
        nk_layout_row_dynamic(ctx, 30, 2);
        nk_label(ctx, "Menu", NK_TEXT_CENTERED);
        menu_connection = menu_work_state;
        if (nk_button_label(ctx, connection_msg[menu_connection]))
        {
            if (menu_work_state == CONNECTION_STATE_DISCONNECTED)
            {
                menu_work_state = CONNECTION_STATE_CONNECTING;
            }
            else if (menu_work_state == CONNECTION_STATE_CONNECTED)
            {
                menu_work_state = CONNECTION_STATE_DISCONNECTING;
            }
        }

        nk_layout_row_dynamic(ctx, 30, 2);
        nk_label(ctx, "NWM", NK_TEXT_CENTERED);
        nwm_connection = nwm_work_state;
        if (nk_button_label(ctx, connection_msg[nwm_connection]))
        {
            if (nwm_work_state == CONNECTION_STATE_DISCONNECTED)
            {
                nwm_work_state = CONNECTION_STATE_CONNECTING;
            }
            else if (nwm_work_state == CONNECTION_STATE_CONNECTED)
            {
                nwm_work_state = CONNECTION_STATE_DISCONNECTING;
            }
        }
    }
    nk_end(ctx);
    nk_window_show(ctx, debug_msg_wnd, show_window);
}
