#ifndef UI_RENDERER_D3D11_H
#define UI_RENDERER_D3D11_H

int ui_renderer_d3d11_init(void);
void ui_renderer_d3d11_destroy(void);

#include "ui_common_sdl.h"
void ui_renderer_d3d11_main(int screen_top_bot, int ctx_top_bot, view_mode_t view_mode, bool win_shared, float bg[4]);
void ui_renderer_d3d11_draw(struct rp_buffer_ctx_t *ctx, uint8_t *data, int width, int height, int screen_top_bot, int ctx_top_bot, int index, view_mode_t view_mode, int win_shared);
void ui_renderer_d3d11_present(int screen_top_bot, int ctx_top_bot, bool win_shared);

int d3d11_ui_init(void);
void d3d11_ui_close(void);

#endif
