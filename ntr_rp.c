#include "ntr_rp.h"
#include "ntr_common.h"
#include "main.h"
#include "ui_common_sdl.h"
#include "rp_syn.h"

#include "ikcp.h"

static SOCKET s;
static struct sockaddr_in remote_addr;
static bool remote_received;

static void socket_error_pause(void) {
  Sleep(SOCKET_RESET_INTERVAL_MS);
}

#define BUF_SIZE 2000
static uint8_t buf[BUF_SIZE];
static ikcpcb *kcp;
static int kcp_cid;
static bool kcp_restart;
static bool kcp_active;
static int kcp_cid_reset = (IUINT16)-1 & ((1 << CID_NBITS) - 1);

static int kcp_udp_output(const char *buf, int len, ikcpcb *, void *)
{
    if (!remote_received)
        return 0;
    return sendto(s, buf, len, 0, (struct sockaddr *)&remote_addr, sizeof(remote_addr));
}

#define RP_PACKET_SIZE 1448
#define RP_DATA_HDR_SIZE (4)
#define RP_PACKET_DATA_SIZE (RP_PACKET_SIZE - RP_DATA_HDR_SIZE)
#define RP_DATA_HDR_ID_SIZE (2)

#define RP_MAX_PACKET_COUNT (128)

#define RP_WORK_COUNT (3)
static uint8_t recv_buf[RP_WORK_COUNT][RP_PACKET_SIZE * RP_MAX_PACKET_COUNT];
static uint8_t recv_track[RP_WORK_COUNT][RP_MAX_PACKET_COUNT];
static uint8_t recv_hdr[RP_WORK_COUNT][RP_DATA_HDR_ID_SIZE];
static uint8_t recv_end[RP_WORK_COUNT];
static uint8_t recv_end_packet[RP_WORK_COUNT];
static uint8_t recv_end_incomp[RP_WORK_COUNT];
static uint32_t recv_end_size[RP_WORK_COUNT];
static uint32_t recv_delay_between_packets[RP_WORK_COUNT];
static uint32_t recv_last_packet_time[RP_WORK_COUNT];
static uint8_t recv_work;
static bool recv_has_last_frame_id[SCREEN_COUNT];
static uint8_t recv_last_frame_id[SCREEN_COUNT];
static uint8_t recv_last_packet_id[SCREEN_COUNT];

#define RP_CORE_COUNT_MAX (3)

#define RP_KCP_WORK_COUNT (2)
#define RP_KCP_HDR_W_NBITS (1)
#define RP_KCP_HDR_T_NBITS (2)
#define RP_KCP_HDR_QUALITY_NBITS (7)
#define RP_KCP_HDR_CHROMASS_NBITS (2)
#define RP_KCP_HDR_SIZE_NBITS (11)
#define RP_KCP_HDR_RC_NBITS (5)

static u8 kcp_recv_w[RP_KCP_WORK_COUNT];

static struct kcp_recv_t {
    u8 buf[RP_MAX_PACKET_COUNT][RP_PACKET_SIZE - sizeof(IUINT16) - sizeof(u16)];
    u8 count; // packet count including term
    u16 term_size; // term packet size
} kcp_recv[RP_KCP_WORK_COUNT][RP_WORK_COUNT][RP_CORE_COUNT_MAX];

static struct kcp_recv_info_t {
    bool is_top;
    u16 jpeg_quality;
    u8 chroma_ss;
    u8 core_count;
    u8 v_adjusted;
    u8 v_last_adjusted;
    u16 term_sizes[RP_CORE_COUNT_MAX];
    u8 term_count; // term count

    u8 last_term; // term being saved
    u16 last_term_size; // size saved so far
} kcp_recv_info[RP_KCP_WORK_COUNT][RP_WORK_COUNT];

static void kcp_init(ikcpcb *kcp) {
    kcp->output = kcp_udp_output;
    ikcp_setmtu(kcp, RP_PACKET_SIZE);

    kcp_active = 0;
    kcp_restart = 0;

    memset(kcp_recv, 0, sizeof(kcp_recv));
    memset(kcp_recv_info, 0, sizeof(kcp_recv_info));
}

struct rp_buffer_ctx_t rp_buffer_ctx[SCREEN_COUNT];
event_t decode_updated_event;

void rp_buffer_init(void) {
  for (int i = 0; i < SCREEN_COUNT; ++i) {
        struct rp_buffer_ctx_t *ctx = &rp_buffer_ctx[i];
        rp_lock_init(ctx->status_lock);
        ctx->index_display_2 = FBI_DISPLAY_2;
        ctx->index_display = FBI_DISPLAY;
        ctx->index_ready_display_2 = FBI_READY_DISPLAY_2;
        ctx->index_ready_display = FBI_READY_DISPLAY;
        ctx->index_decode = FBI_DECODE;
        event_init(&ctx->decode_updated_event);
    }

    event_init(&decode_updated_event);
}

void rp_buffer_destroy(void) {
    event_close(&decode_updated_event);

    for (int i = 0; i < SCREEN_COUNT; ++i) {
        struct rp_buffer_ctx_t *ctx = &rp_buffer_ctx[i];
        event_close(&ctx->decode_updated_event);
        rp_lock_close(ctx->status_lock);
    }
}

atomic_int frame_rate_decoded_tracker[SCREEN_COUNT];
atomic_int frame_rate_displayed_tracker[SCREEN_COUNT];
atomic_int frame_size_tracker[SCREEN_COUNT];
atomic_int delay_between_packet_tracker[SCREEN_COUNT];

static bool jpeg_decode_sem_inited;
static bool jpeg_decode_queue_inited;
static rp_sem_t jpeg_decode_sem;
static struct rp_syn_comp_func_t jpeg_decode_queue;

struct jpeg_decode_info_t {
    int top_bot;
    uint32_t in_size;
    uint32_t in_delay;

    union {
        struct {
            uint8_t *in;
            bool not_queued;
            uint8_t frame_id;
        };

        struct {
            int kcp_w;
            int kcp_queue_w;
        };
    };

    bool is_kcp;
};
static struct jpeg_decode_info_t jpeg_decode_info[RP_WORK_COUNT];
static struct jpeg_decode_info_t *jpeg_decode_ptr[RP_WORK_COUNT];

static int queue_decode(int work) {
    struct jpeg_decode_info_t *ptr = &jpeg_decode_info[work];
    if (rp_syn_rel(&jpeg_decode_queue, ptr) != 0) {
        program_running = 0;
        return -1;
    }
    return 0;
}

static int acquire_decode() {
    int ret;
    if ((ret = acquire_sem(&jpeg_decode_sem)) != 0) {
        if (program_running) {
            program_running = 0;
            err_log("jpeg_decode_sem wait error\n");
        }
    }
    return ret;
}

static int queue_decode_kcp(int w, int queue_w) {
    if (acquire_decode() != 0) {
        return -1;
    }

    // err_log("recv_work %d\n", recv_work);

    struct jpeg_decode_info_t *ptr = &jpeg_decode_info[recv_work];
    int top_bot = kcp_recv_info[w][queue_w].is_top ? 0 : 1;
    *ptr = (struct jpeg_decode_info_t){
        .top_bot = top_bot,
        .kcp_w = w,
        .kcp_queue_w = queue_w,
        .is_kcp = true,
    };
    if (rp_syn_rel(&jpeg_decode_queue, ptr) != 0)
    {
        program_running = 0;
        return -1;
    }

    recv_work = (recv_work + 1) % RP_WORK_COUNT;

    return 0;
}

static int frame_fully_received_tracker;
static int frame_lost_tracker;
static uint8_t last_decoded_frame_id[SCREEN_COUNT];

#include <turbojpeg.h>
static int handle_decode(uint8_t *out, uint8_t *in, int size, int w, int h) {
    tjhandle tjInstance = NULL;
    if ((tjInstance = tj3Init(TJINIT_DECOMPRESS)) == NULL)
    {
        err_log("create turbo jpeg decompressor failed\n");
        return -1;
    }

    int ret = -1;

    if (tj3Set(tjInstance, TJPARAM_STOPONWARNING, 1) != 0)
    {
        goto final;
    }

    if (tj3DecompressHeader(tjInstance, in, size) != 0)
    {
        err_log("jpeg header error\n");
        goto final;
    }

    if (
        h != tj3Get(tjInstance, TJPARAM_JPEGWIDTH) ||
        w != tj3Get(tjInstance, TJPARAM_JPEGHEIGHT))
    {
        err_log("jpeg unexpected dimensions\n");
        goto final;
    }

    if (tj3Decompress8(tjInstance, in, size, out, h * GL_CHANNELS_N, TJ_FORMAT) != 0)
    {
        err_log("jpeg decompression error\n");
        goto final;
    }

    ret = 0;

final:
    tj3Destroy(tjInstance);
    return ret;
}


static unsigned char jpeg_header_top_buffer_kcp[SCREEN_HEIGHT0 * SCREEN_WIDTH * RGB_CHANNELS_N * 2 + 2048];
static unsigned char jpeg_header_bot_buffer_kcp[SCREEN_HEIGHT1 * SCREEN_WIDTH * RGB_CHANNELS_N * 2 + 2048];
static unsigned char jpeg_header_empty_src_kcp[SCREEN_HEIGHT0 * SCREEN_WIDTH * RGB_CHANNELS_N];
static u16 jpeg_header_top_quality_kcp;
static u16 jpeg_header_bot_quality_kcp;
static u16 jpeg_header_top_chroma_ss_kcp;
static u16 jpeg_header_bot_chroma_ss_kcp;
static int set_decode_quality_kcp(bool is_top, int quality, int chroma_ss, int rc)
{
  u16 *hdr_quality = is_top ? &jpeg_header_top_quality_kcp : &jpeg_header_bot_quality_kcp;
  u16 *hdr_chroma_ss = is_top ? &jpeg_header_top_chroma_ss_kcp : &jpeg_header_bot_chroma_ss_kcp;

  // No need to check for rc as we change restart interval manually later
  if (*hdr_quality != quality || *hdr_chroma_ss != chroma_ss) {
    tjhandle tjInst = tj3Init(TJINIT_COMPRESS);
    if (!tjInst) {
      return -1;
    }
    int ret = 0;

    ret = tj3Set(tjInst, TJPARAM_NOREALLOC, 1);
    if (ret < 0) {
      ret = ret * 0x10 - 2;
      goto final;
    }

    ret = tj3Set(tjInst, TJPARAM_RESTARTROWS, rc);
    if (ret < 0) {
      ret = ret * 0x10 - 5;
      goto final;
    }

    ret = tj3Set(tjInst, TJPARAM_QUALITY, quality);
    if (ret < 0) {
      ret = ret * 0x10 - 6;
      goto final;
    }

    enum TJSAMP tjsamp = chroma_ss == 2 ? TJSAMP_444 : chroma_ss == 1 ? TJSAMP_422 : TJSAMP_420;
    ret = tj3Set(tjInst, TJPARAM_SUBSAMP, tjsamp);
    if (ret < 0) {
      ret = ret * 0x10 - 7;
      goto final;
    }

    size_t size = is_top ? sizeof(jpeg_header_top_buffer_kcp) : sizeof(jpeg_header_bot_buffer_kcp);
    size_t buf_size = tj3JPEGBufSize(SCREEN_WIDTH, is_top ? SCREEN_HEIGHT0 : SCREEN_HEIGHT1, tjsamp);
    if (size < buf_size) {
      err_log("buf size %d size %d\n", (int)buf_size, (int)size);
      ret = -3;
      goto final;
    }

    unsigned char *jpeg_buf = is_top ? jpeg_header_top_buffer_kcp : jpeg_header_bot_buffer_kcp;

    ret = tj3Compress8(tjInst, jpeg_header_empty_src_kcp, SCREEN_WIDTH, 0, is_top ? SCREEN_HEIGHT0 : SCREEN_HEIGHT1, TJPF_RGB,
      &jpeg_buf,
      &size);

    if (ret < 0) {
      err_log("tj3Compress8 error (%d): %s\n", tj3GetErrorCode(tjInst), tj3GetErrorStr(tjInst));
      ret = ret * 0x10 - 4;
      goto final;
    }

    ret = 0;
    *hdr_quality = quality;
    *hdr_chroma_ss = chroma_ss;

final:
    tj3Destroy(tjInst);
    return ret;
  }

  return 0;
}

static uint8_t *copy_with_escape(uint8_t *out, const uint8_t *in, int size)
{
    while (size)
    {
        if (*in == 0xff)
        {
            *out = 0xff;
            ++out;
            *out = 0;
            ++out;
            ++in;
        }
        else
        {
            *out = *in;
            ++out;
            ++in;
        }
        --size;
    }
    return out;
}

static unsigned char jpeg_buffer_kcp[SCREEN_HEIGHT0 * SCREEN_WIDTH * RGB_CHANNELS_N + 2048];
static int handle_decode_kcp(uint8_t *out, int w, int queue_w)
{
    struct kcp_recv_t *recvs = kcp_recv[w][queue_w];
    struct kcp_recv_info_t *info = &kcp_recv_info[w][queue_w];

    int ret;
    if ((ret = set_decode_quality_kcp(info->is_top, info->jpeg_quality, info->chroma_ss, info->v_adjusted)) < 0)
    {
        return ret * 0x100 - 1;
    }

    unsigned char *jpeg_header = info->is_top ? jpeg_header_top_buffer_kcp : jpeg_header_bot_buffer_kcp;
    size_t jpeg_header_size_max = info->is_top ? sizeof(jpeg_header_top_buffer_kcp) : sizeof(jpeg_header_bot_buffer_kcp);
    size_t jpeg_header_size = 0;
    for (size_t i = 0; i < jpeg_header_size_max; ++i)
    {
        if (jpeg_header[i] == 0xff)
        {
            if (i + 1 < jpeg_header_size_max)
            {
                if (jpeg_header[i + 1] == 0xdd)
                {
                    if (i + 6 >= jpeg_header_size_max)
                    {
                        return -6;
                    }
                    *(u16 *)&jpeg_header[i + 4] = htons(info->v_adjusted * (SCREEN_WIDTH / (JPEG_DCTSIZE * (info->chroma_ss == 2 ? 1 : 2))));
                }
                else if (jpeg_header[i + 1] == 0xda)
                {
                    jpeg_header_size = i + 2;
                    if (jpeg_header_size + 2 >= jpeg_header_size_max)
                    {
                        return -4;
                    }
                    jpeg_header_size += ntohs(*(u16 *)&jpeg_header[jpeg_header_size]);
                    if (jpeg_header_size >= jpeg_header_size_max)
                    {
                        return -5;
                    }
                    break;
                }
            }
        }
    }
    if (jpeg_header_size == 0)
    {
        return -2;
    }

    memcpy(jpeg_buffer_kcp, jpeg_header, jpeg_header_size);
    unsigned char *ptr = jpeg_buffer_kcp + jpeg_header_size;
    for (int t = 0; t < info->core_count; ++t)
    {
        struct kcp_recv_t *recv = &recvs[t];
        for (int i = 0; i < recv->count; ++i)
        {
            if (i == recv->count - 1)
            {
                ptr = copy_with_escape(ptr, recv->buf[i], recv->term_size);
            }
            else
            {
                ptr = copy_with_escape(ptr, recv->buf[i], RP_PACKET_SIZE - sizeof(IUINT16) - sizeof(u16));
            }
        }
        *ptr = 0xff;
        ++ptr;
        if (t == info->core_count - 1)
        {
            *ptr = 0xd9;
        }
        else
        {
            *ptr = 0xd0 + t;
        }
        ++ptr;
    }

    if (handle_decode(out, jpeg_buffer_kcp, ptr - jpeg_buffer_kcp, info->is_top ? SCREEN_HEIGHT0 : SCREEN_HEIGHT1, SCREEN_WIDTH) != 0)
    {
        return -3;
    }

    memset(recvs, 0, sizeof(struct kcp_recv_t) * RP_CORE_COUNT_MAX);
    memset(info, 0, sizeof(struct kcp_recv_info_t));

    return 0;
}

static void handle_decode_frame_screen(struct rp_buffer_ctx_t *ctx, int top_bot, int frame_size, int delay_between_packet, struct rp_buffer_ctx_t *sync_ctx)
{
    __atomic_add_fetch(&frame_rate_decoded_tracker[top_bot], 1, __ATOMIC_RELAXED);
    if (__atomic_load_n(&frame_size_tracker[top_bot], __ATOMIC_RELAXED) < frame_size) {
        __atomic_store_n(&frame_size_tracker[top_bot], frame_size, __ATOMIC_RELAXED);
    }
    if (__atomic_load_n(&delay_between_packet_tracker[top_bot], __ATOMIC_RELAXED) < delay_between_packet) {
        __atomic_store_n(&delay_between_packet_tracker[top_bot], delay_between_packet, __ATOMIC_RELAXED);
    }

    rp_lock_wait(ctx->status_lock);
    // ctx_sync is set when view mode is top and bot in one window.
    // we can use this check to enable "triple buffering" only when the update rate is likely to exceed monitor refresh rate.
    if (/* ctx_sync && */ ctx->status >= FBS_UPDATED) {
        int index = ctx->index_ready_display_2;
        ctx->index_ready_display_2 = ctx->index_ready_display;
        ctx->index_ready_display = ctx->index_decode;
        ctx->index_decode = index;
        ctx->status = FBS_UPDATED_2;
    } else {
        int index = ctx->index_ready_display;
        ctx->index_ready_display = ctx->index_decode;
        ctx->index_decode = index;
        ctx->status = FBS_UPDATED;
    }
    rp_lock_rel(ctx->status_lock);

    if (renderer_single_thread) {
        cond_mutex_flag_signal(&decode_updated_event);
    } else {
        if (sync_ctx)
            cond_mutex_flag_signal(&sync_ctx->decode_updated_event);
        else
            cond_mutex_flag_signal(&ctx->decode_updated_event);
    }
}

static thread_ret_t jpeg_decode_thread_func(void *e)
{
    while (program_running && !kcp_restart) {
        struct jpeg_decode_info_t *ptr;
        while (1)
        {
            if (!(program_running && !kcp_restart))
                return 0;
            thread_set_cancel_state(true);
            int res = rp_syn_acq(&jpeg_decode_queue, NWM_THREAD_WAIT_NS, (void **)&ptr, e);
            thread_set_cancel_state(false);
            if (res == 0)
                break;
            if (res != ETIMEDOUT)
            {
                err_log("rp_syn_acq failed\n");
                program_running = 0;
                return 0;
            }
        }

        int top_bot = ptr->top_bot;
        struct rp_buffer_ctx_t *ctx = &rp_buffer_ctx[top_bot];
        int index = ctx->index_decode;
        uint8_t *out = ctx->screen_decoded[index];

        view_mode_t view_mode = __atomic_load_n(&ui_view_mode, __ATOMIC_RELAXED);
        struct rp_buffer_ctx_t *sync_ctx = view_mode == VIEW_MODE_TOP_BOT ? &rp_buffer_ctx[SCREEN_TOP] : NULL;
        // TODO csc

        int ret;
        if (ptr->is_kcp)
        {
            // err_log("%d %d\n", ptr->kcp_w, ptr->kcp_queue_w);
            if ((ret = handle_decode_kcp(out, ptr->kcp_w, ptr->kcp_queue_w)) != 0)
            {
                err_log("kcp recv decode error: %d\n", ret);
                kcp_restart = 1;

                // ikcp_reset(kcp, kcp->cid);
                kcp_cid_reset = kcp->cid;
                kcp_cid = (kcp->cid + 1) & ((1 << CID_NBITS) - 1);
            }
            else
            {
                // err_log("%d\n", kcp_recv_info[ptr->kcp_w][ptr->kcp_queue_w].term_count);
                handle_decode_frame_screen(ctx, top_bot, ptr->in_size, ptr->in_delay, sync_ctx);
            }
        }
        else
        {
            if (ptr->in)
            {
                if (handle_decode(out, ptr->in, ptr->in_size, top_bot == 0 ? SCREEN_HEIGHT0 : SCREEN_HEIGHT1, SCREEN_WIDTH) != 0)
                {
                    err_log("recv decode error\n");
                    __atomic_add_fetch(&frame_lost_tracker, 1, __ATOMIC_RELAXED);
                }
                else
                {
                    handle_decode_frame_screen(ctx, top_bot, ptr->in_size, ptr->in_delay, sync_ctx);
                    __atomic_add_fetch(&frame_fully_received_tracker, 1, __ATOMIC_RELAXED);
                }
            }
            else
            {
                __atomic_add_fetch(&frame_lost_tracker, (uint8_t)(ptr->frame_id - last_decoded_frame_id[top_bot]), __ATOMIC_RELAXED);
            }
            last_decoded_frame_id[top_bot] = ptr->frame_id;
        }

        rp_sem_rel(jpeg_decode_sem);
    }

    return 0;
}

static int handle_recv(uint8_t *buf, int size)
{
    if (size < RP_DATA_HDR_SIZE)
    {
        err_log("recv header too small\n");
        return 0;
    }
    uint8_t *hdr = buf;
    buf += RP_DATA_HDR_SIZE;
    size -= RP_DATA_HDR_SIZE;

    // err_log("%d %d %d %d (%d)\n", hdr[0], hdr[1], hdr[2], hdr[3], size);

    if (hdr[2] != 2)
    {
        err_log("recv invalid header\n");
        return 0;
    }

    uint8_t end = 0;
    if (hdr[1] & 0x10)
    {
        end = 1;
    }
    else if (size != RP_PACKET_DATA_SIZE)
    {
        err_log("recv incorrect size: %d\n", size);
        return 0;
    }
    hdr[1] &= 0x1;
    uint8_t work = recv_work;

    int work_next = 0;
    if (memcmp(recv_hdr[work], hdr, RP_DATA_HDR_ID_SIZE) != 0)
    {
        // If no decode_info is set at this point, it means network receive has skipped frame.
        // Queue empty info to keep in sync.
        if (jpeg_decode_info[work].not_queued)
        {
            if (queue_decode(work) != 0)
            {
                return -1;
            }
        }

        work = (work + 1) % RP_WORK_COUNT;
        work_next = 1;
    }

    if (work_next)
    {
        if (acquire_decode() != 0)
        {
            return -1;
        }

        jpeg_decode_info[work] = (struct jpeg_decode_info_t){0};
        jpeg_decode_info[work].not_queued = true;

        memcpy(recv_hdr[work], hdr, RP_DATA_HDR_ID_SIZE);
        jpeg_decode_info[work].frame_id = recv_hdr[work][0];
        jpeg_decode_info[work].top_bot = !recv_hdr[work][1];

        recv_delay_between_packets[work] = 0;
        recv_last_packet_time[work] = iclock();

        memset(recv_track[work], 0, RP_MAX_PACKET_COUNT);
        if (recv_end[work] != 2)
        {
            err_log("recv incomplete skipping frame\n");
        }
        recv_end[work] = 0;
        recv_end_incomp[work] = 0;

        recv_work = work;
    }

    uint8_t packet = hdr[3];
    if (packet >= RP_MAX_PACKET_COUNT)
    {
        err_log("recv packet number too high\n");
        return 0;
    }

    {
        uint32_t packet_time = iclock();
        uint32_t delay_from_last_packet = packet_time - recv_last_packet_time[work];
        if (delay_from_last_packet > recv_delay_between_packets[work])
        {
            recv_delay_between_packets[work] = delay_from_last_packet;
        }
        recv_last_packet_time[work] = packet_time;
    }

    // err_log("%d %d %d %d (%d %d)\n", hdr[0], hdr[1], hdr[2], hdr[3], size, end);

    memcpy(&recv_buf[work][RP_PACKET_DATA_SIZE * packet], buf, size);
    recv_track[work][packet] = 1;
    if (end)
    {
        recv_end[work] = 1;
        recv_end_packet[work] = packet;
        recv_end_size[work] = RP_PACKET_DATA_SIZE * packet + size;
        // err_log("size %d\n", recv_end_size[work]);
    }

    if (recv_end[work] == 1)
    {
        for (int i = 0; i < recv_end_packet[work]; ++i)
        {
            if (!recv_track[work][i])
            {
                if (!recv_end_incomp[work])
                {
                    recv_end_incomp[work] = 1;
                    err_log("recv end packet incomplete\n");
                }
                return 0;
            }
        }

        recv_end[work] = 2;
        int top_bot = !recv_hdr[work][1];

        jpeg_decode_info[work] = (struct jpeg_decode_info_t){
            .top_bot = top_bot,
            .in_delay = recv_delay_between_packets[work],
            .in = recv_buf[work],
            .frame_id = recv_hdr[work][0],
            .in_size = recv_end_size[work],
        };
        if (queue_decode(work) != 0)
            return -1;
    }

    return 0;
}

static int handle_recv_kcp(uint8_t *buf, int size)
{
    if (size < (int)sizeof(u16)) {
        return -1;
    }
    u16 hdr = *(u16 *)buf;
    buf += sizeof(u16);
    size -= sizeof(u16);

    u16 w = (hdr >> (PID_NBITS + CID_NBITS)) & ((1 << RP_KCP_HDR_W_NBITS) - 1);
    u16 queue_w = kcp_recv_w[w];

    u16 t = (hdr >> (PID_NBITS + CID_NBITS + RP_KCP_HDR_W_NBITS)) & ((1 << RP_KCP_HDR_T_NBITS) - 1);

    if (t < RP_CORE_COUNT_MAX) {
        if (kcp_recv_info[w][queue_w].term_count != 0) {
            err_log(
                "%d %d %d %d %d %d\n", w, queue_w,
                (int)kcp_recv_info[w][queue_w].term_count,
                (int)kcp_recv_info[w][queue_w].last_term,
                (int)kcp_recv_info[w][queue_w].last_term_size,
                (int)kcp_recv_info[w][queue_w].term_sizes[kcp_recv_info[w][queue_w].last_term]);
            return -9;
        }
        if (size != RP_PACKET_SIZE - sizeof(IUINT16) - sizeof(u16)) {
            return -2;
        }
        struct kcp_recv_t *recv = &kcp_recv[w][queue_w][t];
        if (recv->count < RP_MAX_PACKET_COUNT) {
            memcpy(recv->buf[recv->count], buf, size);
            ++recv->count;
        } else {
            return -4;
        }
    } else { // t == rp_core_count_max
        // err_log("%d %d\n", w, queue_w);

        struct kcp_recv_info_t *info = &kcp_recv_info[w][queue_w];
        if (info->term_count == 0) {
            if (size < (int)sizeof(u16)) {
                return -3;
            }
            hdr = *(u16 *)buf;
            buf += sizeof(u16);
            size -= sizeof(u16);

            u16 jpeg_quality = hdr & ((1 << RP_KCP_HDR_QUALITY_NBITS) - 1);
            u16 core_count = (hdr >> RP_KCP_HDR_QUALITY_NBITS) & ((1 << RP_KCP_HDR_T_NBITS) - 1);
            bool top_bot = (hdr >> (RP_KCP_HDR_QUALITY_NBITS + RP_KCP_HDR_T_NBITS)) & ((1 << 1) - 1);
            u16 chroma_ss = (hdr >> (RP_KCP_HDR_QUALITY_NBITS + RP_KCP_HDR_T_NBITS + 1)) & ((1 << RP_KCP_HDR_CHROMASS_NBITS) - 1);

            // err_log("w %d quality %d cores %d top %d\n", (int)w, (int)jpeg_quality, (int)core_count, (int)is_top);

            if (core_count == 0) {
                // ignore core_count == 0 for future extension
                return 0;
            }

            info->jpeg_quality = jpeg_quality;
            info->core_count = core_count;
            info->is_top = top_bot == 0;
            info->chroma_ss = chroma_ss;

            for (int t = 0; t < core_count; ++t) {
                if (size < (int)sizeof(u16)) {
                    return -6;
                }
                hdr = *(u16 *)buf;
                buf += sizeof(u16);
                size -= sizeof(u16);

                u16 v_adjusted = (hdr >> RP_KCP_HDR_SIZE_NBITS) & ((1 << RP_KCP_HDR_RC_NBITS) - 1);
                u16 term_size = hdr & ((1 << RP_KCP_HDR_SIZE_NBITS) - 1);

                // err_log("t %d rc %d size %d\n", (int)t, (int)v_adjusted, (int)term_size);

                info->term_sizes[t] = term_size;
                if (t == core_count - 1) {
                    if (core_count > 1 && v_adjusted > info->v_adjusted) {
                        return -8;
                    }
                    info->v_last_adjusted = v_adjusted;
                } else if (t == 0) {
                    info->v_adjusted = v_adjusted;
                } else if (info->v_adjusted != v_adjusted) {
                    return -7;
                }
            }
        }

        while (1) {
            struct kcp_recv_t *recv = &kcp_recv[w][queue_w][info->last_term];
            u16 left_size = info->term_sizes[info->last_term] - info->last_term_size;
            // err_log(
            //     "left size %d size %d last term %d last term size %d\n",
            //     (int)left_size, (int)size, (int)info->last_term, (int)info->last_term_size);
            if (left_size == 0) {
                ++recv->count;
                recv->term_size = info->last_term_size;
                // err_log("%d\n", (int)recv->term_size);

                ++info->last_term;
                info->last_term_size = 0;

                if (info->last_term == info->core_count) {
                    int ret = queue_decode_kcp(w, queue_w);
                    if (ret < 0)
                        return ret * 0x100 - 10;
                    kcp_recv_w[w] = (kcp_recv_w[w] + 1) % RP_WORK_COUNT;
                    return 0;
                }

                continue;
            }

            if (!size)
                break;

            left_size = left_size <= size ? left_size : size;
            memcpy(recv->buf[recv->count] + info->last_term_size, buf, left_size);
            buf += left_size;
            size -= left_size;
            info->last_term_size += left_size;
        }

        ++info->term_count;
    }

    return 0;
}

static uint32_t reply_time;
static void socket_reply(void)
{
    if (kcp_active) {
        if (!kcp->session_just_established) {
            int ret;
            while ((ret = ikcp_recv(kcp, (char *)buf, sizeof(buf))) > 0) {
                // err_log("ikcp_recv: %d\n", ret);
                if ((ret = handle_recv_kcp(buf, ret)) != 0) {
                    err_log("handle_recv_kcp failed: %d\n", ret);
                    kcp_restart = 1;
                    ikcp_reset(kcp, kcp->cid);
                    kcp_cid_reset = kcp->cid;
                    kcp_cid = (kcp->cid + 1) & ((1 << CID_NBITS) - 1);
                    return;
                }
            }
            if (ret < 0) {
                err_log("ikcp_recv failed: %d\n", ret);
                kcp_restart = 1;
                return;
            }
            bool reply = false;
            uint32_t current_time = iclock();
            if (kcp->recv_pid == kcp->input_pid) {
                // Send ack every 1/8 second or 125 ms; do not spam as that slows thing down considerably
                if (current_time - reply_time >= 125000) {
                    reply = true;
                }
            } else {
                // Likewise nack every 1/80 second or 12.5 ms
                if (current_time - reply_time >= 12500) {
                    reply = true;
                }
            }
            if (reply) {
                if ((ret = ikcp_reply(kcp)) < 0) {
                    if (ret < -0x100) {
                        if (socket_errno() == WSAEWOULDBLOCK) {
                            return;
                        }
                    }
                    err_log("ikcp_reply failed: %d\n", ret);
                    kcp_restart = 1;
                    return;
                } else {
                    reply_time = current_time;
                }
            }
        }
    }
}

static int test_kcp_magic(int magic)
{
    return !((magic & (~0x00001100 & 0x0000ff00)) == 0 && (magic & 0x00ff0000) == 0x00020000);
}

static void socket_action(int ret)
{
    int ntr_is_kcp_test = 0;
    if (ret == (int)sizeof(uint16_t)) {
        ntr_is_kcp_test = 1;
    } else {
        if (ret < (int)sizeof(uint32_t)) {
            return;
        }
        int magic = *(uint32_t *)buf;
        // err_log("magic: 0x%x\n", magic);
        ntr_is_kcp_test = test_kcp_magic(magic);
    }
    // err_log("recvfrom: %d\n", ret);
    if (ntr_is_kcp_test) {
        kcp_active = 1;
    }

    if (kcp_active) {
        if ((ret = ikcp_input(kcp, (const char *)buf, ret)) != 0) {
            kcp_restart = 1;
            if (kcp->input_cid == kcp_cid_reset) {
                ikcp_reset(kcp, kcp_cid_reset);
            } else if (kcp->should_reset) {
                err_log("ikcp_reset: %d\n", kcp->cid);
                ikcp_reset(kcp, kcp->cid);
                kcp_cid_reset = kcp->cid;
                kcp_cid = kcp->input_cid;
            } else {
                if (ret < 0) {
                    err_log("ikcp_input failed: %d\n", ret);
                }
                ikcp_reset(kcp, kcp->cid);
                kcp_cid_reset = kcp->cid;
                kcp_cid = (kcp->cid + 1) & ((1 << CID_NBITS) - 1);
            }
            return;
        }
        // Sleep(1);
        if (kcp->session_just_established) {
            kcp->session_just_established = false;
            if (!kcp->session_established) {
                err_log("kcp session_established\n");
                kcp->session_established = true;
            }
        }
    } else if (handle_recv(buf, ret) < 0) {
        return;
    }
}

static void receive_from_socket()
{
    while (program_running && !kcp_restart)
    {
        socklen_t addr_len = sizeof(remote_addr);

        int ret = recvfrom(s, (char *)buf, sizeof(buf), 0, (struct sockaddr *)&remote_addr, &addr_len);
        if (
            ret == 0
            // || (rand() & 0xf) == 0
        )
        {
            continue;
        }
        else if (ret < 0)
        {
            int err = socket_errno();
            if (err != WSAETIMEDOUT && err != WSAEWOULDBLOCK)
            {
                // err_log("recvfrom failed: %d\n", err);
                Sleep(SOCKET_RESET_INTERVAL_MS);
                return;
            }
            else if (err == WSAEWOULDBLOCK)
            {
                socket_reply();
                if (!socket_poll(s))
                {
                    if (program_running)
                        err_log("socket poll failed: %d\n", socket_errno());
                    return;
                }
            }
            continue;
        }

#ifdef _WIN32
        if (ntr_ip_octet[0] == 0 &&
            ntr_ip_octet[1] == 0 &&
            ntr_ip_octet[2] == 0 &&
            ntr_ip_octet[3] == 0)
        {
            ntr_ip_octet[0] = remote_addr.sin_addr.S_un.S_un_b.s_b1;
            ntr_ip_octet[1] = remote_addr.sin_addr.S_un.S_un_b.s_b2;
            ntr_ip_octet[2] = remote_addr.sin_addr.S_un.S_un_b.s_b3;
            ntr_ip_octet[3] = remote_addr.sin_addr.S_un.S_un_b.s_b4;
        }
#endif

        remote_received = 1;

        socket_action(ret);
    }
}

static void receive_from_socket_loop(void) {
    while (program_running && !ntr_rp_port_changed)
    {
        kcp = ikcp_create(kcp_cid, 0);
        if (!kcp)
        {
            err_log("ikcp_create failed\n");
            Sleep(SOCKET_RESET_INTERVAL_MS);
            continue;
        }
        kcp_init(kcp);

        remote_received = 0;

        for (int i = 0; i < SCREEN_COUNT; ++i)
        {
            recv_has_last_frame_id[i] = 0;
            recv_last_frame_id[i] = 0;
            recv_last_packet_id[i] = 0;
        }

        // err_log("new connection\n");
        for (int i = 0; i < SCREEN_COUNT; ++i)
        {
            rp_lock_wait(rp_buffer_ctx[i].status_lock);
            rp_buffer_ctx[i].status = FBS_NOT_AVAIL;
            rp_lock_rel(rp_buffer_ctx[i].status_lock);
        }
        for (int i = 0; i < RP_WORK_COUNT; ++i)
        {
            recv_end[i] = 2;
        }
        memset(recv_hdr, 0, sizeof(recv_hdr));
        // recv_work = 0;

        memset(frame_rate_decoded_tracker, 0, sizeof(frame_rate_decoded_tracker));
        memset(frame_rate_displayed_tracker, 0, sizeof(frame_rate_displayed_tracker));
        memset(frame_size_tracker, 0, sizeof(frame_size_tracker));
        memset(delay_between_packet_tracker, 0, sizeof(delay_between_packet_tracker));

        if (jpeg_decode_sem_inited)
        {
            if (rp_sem_close(jpeg_decode_sem) != 0)
            {
                err_log("jpeg_decode_sem close failed\n");
                break;
            }
            jpeg_decode_sem_inited = 0;
        }
        if (rp_sem_create(jpeg_decode_sem, RP_WORK_COUNT, RP_WORK_COUNT) != 0)
        {
            err_log("jpeg_decode_sem init failed\n");
            break;
        }
        jpeg_decode_sem_inited = 1;

        if (jpeg_decode_queue_inited)
        {
            if (rp_syn_close1(&jpeg_decode_queue))
            {
                err_log("jpeg_decode_queue close failed\n");
                break;
            }
            jpeg_decode_queue_inited = 0;
        }
        if (rp_syn_init1(&jpeg_decode_queue, 0, 0, 0, RP_WORK_COUNT, (void **)jpeg_decode_ptr) != 0)
        {
            err_log("jpeg_decode_queue init failed\n");
            break;
        }
        jpeg_decode_queue_inited = 1;

        memset(jpeg_decode_ptr, 0, sizeof(jpeg_decode_ptr));
        memset(jpeg_decode_info, 0, sizeof(jpeg_decode_info));

        thread_t jpeg_decode_thread;
        int ret;
#ifdef _WIN32
        HANDLE jpeg_decode_thread_e = CreateEventA(NULL, FALSE, FALSE, NULL);
#else
        void *jpeg_decode_thread_e = NULL;
#endif
        if ((ret = thread_create(jpeg_decode_thread, jpeg_decode_thread_func, jpeg_decode_thread_e)))
        {
            err_log("jpeg_decode_thread create failed\n");
            break;
        }

        receive_from_socket();
        // Sleep(SOCKET_RESET_INTERVAL_MS);

#ifdef _WIN32
        thread_set_cancel(jpeg_decode_thread_e);
        thread_join(jpeg_decode_thread);
        CloseHandle(jpeg_decode_thread_e);
#else
        thread_cancel(jpeg_decode_thread);
        thread_join(jpeg_decode_thread);
#endif

        if (kcp)
        {
            ikcp_release(kcp);
            kcp = 0;
        }
    }
}

thread_ret_t udp_recv_thread_func(void *) {
    while (program_running)
    {
        s = 0;
        int ret;
        if (!socket_valid(s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)))
        {
            err_log("socket creation failed\n");
            socket_error_pause();
            continue;
        }

        ntr_rp_port_bound = ntr_rp_port;
        struct sockaddr_in si_other;
        si_other.sin_family = AF_INET;
        si_other.sin_port = htons(ntr_rp_port_bound);
        si_other.sin_addr.s_addr = ntr_adaptor_octet_list ? *(uint32_t *)ntr_adaptor_octet_list[ntr_selected_adapter] : 0;

        if (bind(s, (struct sockaddr *)&si_other, sizeof(si_other)) == SOCKET_ERROR)
        {
            err_log("socket bind failed for port %d\n", ntr_rp_port_bound);
            socket_error_pause();
            goto socket_final;
        }
        uint8_t octets_null[] = {0, 0, 0, 0};
        uint8_t *octets = ntr_adaptor_octet_list ? ntr_adaptor_octet_list[ntr_selected_adapter] : octets_null;
        err_log("port bound at %d.%d.%d.%d:%d\n", (int)octets[0], (int)octets[1], (int)octets[2], (int)octets[3], ntr_rp_port_bound);
        ntr_rp_port_changed = 0;
        ntr_rp_port = ntr_rp_port_bound;

        int buff_size = 6 * 1024 * 1024;
        socklen_t tmp = sizeof(buff_size);

        ret = setsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)(&buff_size), sizeof(buff_size));
        buff_size = 0;
        ret = getsockopt(s, SOL_SOCKET, SO_RCVBUF, (char *)(&buff_size), &tmp);
        if (ret)
        {
            err_log("setsockopt buf size failed\n");
            socket_error_pause();
            goto socket_final;
        }

#ifdef _WIN32
        DWORD timeout = SOCKET_RESET_INTERVAL_MS;
#else
        struct timeval timeout;
        timeout.tv_sec = SOCKET_RESET_INTERVAL_MS / 1000;
        timeout.tv_usec = (SOCKET_RESET_INTERVAL_MS % 1000) * 1000;
#endif
        ret = setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&timeout, sizeof(timeout));
        if (ret)
        {
            err_log("setsockopt timeout failed\n");
            socket_error_pause();
            goto socket_final;
        }

        if (!socket_set_nonblock(s, 1))
        {
            err_log("socket_set_nonblock failed, %d\n", socket_errno());
            socket_error_pause();
            goto socket_final;
        }

        receive_from_socket_loop();

socket_final:
        closesocket(s);
    }
    return 0;
}
