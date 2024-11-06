#ifndef NTR_RP_H
#define NTR_RP_H

#include "const.h"
thread_ret_t udp_recv_thread_func(void *);

void rp_buffer_init(void);
void rp_buffer_destroy(void);

#include <stdatomic.h>
extern atomic_int frame_rate_decoded_tracker[SCREEN_COUNT];
extern atomic_int frame_rate_displayed_tracker[SCREEN_COUNT];
extern atomic_int frame_size_tracker[SCREEN_COUNT];
extern atomic_int delay_between_packet_tracker[SCREEN_COUNT];
extern atomic_int frame_fully_received_tracker;
extern atomic_int frame_lost_tracker;
extern atomic_bool kcp_active;
extern atomic_bool kcp_restart;

#include "ui_common_sdl.h"
#include "rp_syn.h"
#define SCREEN_UPSCALE_FACTOR 2

#define sr_create(...) (0)
#define sr_run(...) (0)
#define sr_next(...) ((void)0)
#define sr_destroy(...) ((void)0)
#define sr_reset() ((void)0)

#ifdef _WIN32
#include <d3d11.h>
#endif

struct rp_buffer_ctx_t {
#ifdef _WIN32
    ID3D11Texture2D *d3d_tex[SCREEN_COUNT];
    ID3D11ShaderResourceView *d3d_srv[SCREEN_COUNT];
#endif

    uint8_t screen_decoded[FBI_COUNT][SCREEN_HEIGHT0 * SCREEN_WIDTH * GL_CHANNELS_N];
    uint8_t screen_upscaled[SCREEN_HEIGHT0 * SCREEN_WIDTH * GL_CHANNELS_N * SCREEN_UPSCALE_FACTOR * SCREEN_UPSCALE_FACTOR];

    rp_lock_t status_lock;
    enum frame_buffer_status_t status;
    int index_display_2;
    int index_display;
    int index_ready_display_2;
    int index_ready_display;
    int index_decode;

    uint8_t *data_prev;
    int win_width_prev, win_height_prev;
    view_mode_t view_mode_prev;

    event_t decode_updated_event;
};
extern struct rp_buffer_ctx_t rp_buffer_ctx[SCREEN_COUNT];
extern event_t decode_updated_event;

#endif
