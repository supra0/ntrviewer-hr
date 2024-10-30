#ifndef CONST_H
#define CONST_H

#include <stdio.h>
#define err_log(f, ...) fprintf(stderr, "%s:%d:%s " f, __FILE__, __LINE__, __func__, ## __VA_ARGS__)

enum screen_t {
  SCREEN_TOP,
  SCREEN_BOT,
  SCREEN_COUNT,
};

enum frame_buffer_index_t
{
  FBI_DECODE,
  FBI_READY_DISPLAY,
  FBI_READY_DISPLAY_2,
  FBI_DISPLAY,
  FBI_DISPLAY_2,
  FBI_COUNT,
};

#define UNUSED __attribute__((unused))

#include <SDL2/SDL.h>
#define SDL_WIN_FLAGS_DEFAULT (SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN)
#define WIN_TITLE "NTRViewer HR"

#define WIN_WIDTH_DEFAULT (SCREEN_HEIGHT0 * 2)
#define WIN_HEIGHT_DEFAULT (SCREEN_WIDTH * 2 * 2)
#define WIN_WIDTH2_DEFAULT (SCREEN_HEIGHT1 * 2)
#define WIN_HEIGHT12_DEFAULT (SCREEN_WIDTH * 2)

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT0 400
#define SCREEN_HEIGHT1 320
#define SDL_FORMAT SDL_PIXELFORMAT_RGBA32

#define ui_font_scale_step_factor (32.0f)
#define ui_font_scale_epsilon (1.0f / ui_font_scale_step_factor)

#endif
