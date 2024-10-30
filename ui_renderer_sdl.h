#ifndef UI_RENDERER_SDL_H
#define UI_RENDERER_SDL_H

extern SDL_Renderer *sdl_renderer[SCREEN_COUNT];

int ui_renderer_sdl_init(void);
void ui_renderer_sdl_destroy(void);

#endif
