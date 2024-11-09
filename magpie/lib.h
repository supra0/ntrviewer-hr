#ifndef MAGPIE_LIB_H
#define MAGPIE_LIB_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <d3d11.h>

struct magpie_t;
struct magpie_t *magpie_load(void);
void magpie_unload(struct magpie_t *magpie);

size_t magpie_mode_count(struct magpie_t *magpie);
const char *magpie_mode_name(struct magpie_t *magpie, size_t index);

struct magpie_render_t;
struct magpie_render_t *magpie_render_init(struct magpie_t *magpie, size_t index, ID3D11Device* dev, ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const SIZE* outSize);
void magpie_render_close(struct magpie_render_t *render);

void magpie_render_run(struct magpie_render_t *render);
ID3D11Texture2D* magpie_render_output(struct magpie_render_t *render);

#ifdef __cplusplus
}
#endif

#endif
