#ifndef REALCUGAN_LIB_H
#define REALCUGAN_LIB_H

#include "glad/glad.h"
#include "stdbool.h"

#include "../fsr/fsr_main.h"

#ifdef _WIN32
#include <d3d11.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define REALCUGAN_SCALE SCREEN_UPSCALE_FACTOR
#define D3D11_MUTEX_TIMEOUT (2000)

#ifdef _WIN32
extern int realcugan_d3d11_create(ID3D11Device *device[SCREEN_COUNT], ID3D11DeviceContext *context[SCREEN_COUNT], IDXGIAdapter1 *adapter);
extern int realcugan_d3d11_reset(ID3D11Device *device[SCREEN_COUNT], ID3D11DeviceContext *context[SCREEN_COUNT], IDXGIAdapter1 *adapter);
extern ID3D11Resource *realcugan_d3d11_run(int ctx_top_bot, int screen_top_bot, int index, int w, int h, int c, const unsigned char *indata, unsigned char *outdata, IDXGIKeyedMutex **mutex, ID3D11ShaderResourceView **srv, bool *dim3, bool *success);
#endif

extern int realcugan_ogl_create(void);
extern GLuint realcugan_ogl_run(int ctx_top_bot, int screen_top_bot, int index, int w, int h, int c, const unsigned char *indata, unsigned char *outdata, GLuint *gl_sem, GLuint *gl_sem_next, bool *dim3, bool *success);

extern void realcugan_next(int ctx_top_bot, int screen_top_bot, int index);
extern void realcugan_destroy(void);

extern int opt_testing_no_ext_mem, opt_testing_no_shared_sem, opt_testing_no_fp16;

#ifdef __cplusplus
}
#endif

#endif
