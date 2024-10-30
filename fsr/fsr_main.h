#ifndef FSR_MAIN_H
#define FSR_MAIN_H

#ifndef USE_D3D11
#include <glad/glad.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#include "../const.h"

#define FrameBufferCount (FBI_COUNT)

#ifdef USE_D3D11
#else
GLuint fsr_main(int tb, int top_bot, GLuint inputTexture, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, float rcasAtt);
#endif

#ifdef __cplusplus
}
#endif

#endif
