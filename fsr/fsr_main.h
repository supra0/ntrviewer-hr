#ifndef FSR_MAIN_H
#define FSR_MAIN_H

#include <glad/glad.h>

#ifdef __cplusplus
extern "C" {
#endif

#include "../const.h"

GLuint fsr_main(int tb, int top_bot, GLuint inputTexture, uint32_t in_w, uint32_t in_h, uint32_t out_w, uint32_t out_h, float rcasAtt);

#ifdef __cplusplus
}
#endif

#endif
