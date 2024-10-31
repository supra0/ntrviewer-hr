#ifndef NTR_COMMON_H
#define NTR_COMMON_H

#define SOCKET_POLL_INTERVAL_MS 250
#define SOCKET_RESET_INTERVAL_MS 2000

#include "nuklear/nuklear.h"

#include "main.h"
#include <stdatomic.h>

int socket_startup(void);
int socket_shutdown(void);

UNUSED static bool socket_set_nonblock(SOCKET s, bool nb)
{
#ifdef _WIN32
    u_long opt = nb;
    if (ioctlsocket(s, FIONBIO, &opt)) {
        return false;
    }
#else
    int flags = fcntl(s, F_GETFL, 0);
    if (flags == -1) {
        return false;
    }
    flags = nb ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
    if (fcntl(s, F_SETFL, flags) != 0) {
        return false;
    }
#endif
    return true;
}

UNUSED static bool socket_poll(SOCKET s)
{
    while (program_running) {
        WSAPOLLFD pollfd = {
            .fd = s,
            .events = POLLIN,
            .revents = 0,
        };
        int res = WSAPoll(&pollfd, 1, SOCKET_POLL_INTERVAL_MS);
        if (res < 0) {
            return false;
        }
        else if (res > 0) {
            if (pollfd.revents & POLLIN) {
                return true;
            }
        }
    }
    return false;
}

extern atomic_uint_fast8_t ntr_ip_octet[4];

extern atomic_int ntr_rp_port;
extern atomic_int ntr_rp_port_bound;
extern atomic_bool ntr_rp_port_changed;

struct ntr_rp_config_t {
    nk_bool top_screen_priority;
    int screen_priority_factor;
    int jpeg_quality;
    int bandwidth_limit;
    int kcp_mode;
};

extern struct ntr_rp_config_t ntr_rp_config;
void ntr_config_set_default(void);

extern char **ntr_auto_ip_list;
extern uint8_t **ntr_auto_ip_octet_list;
extern int ntr_auto_ip_count;

extern int ntr_selected_ip;
extern int ntr_selected_adapter;

extern char **ntr_adaptor_list;
extern uint8_t **ntr_adaptor_octet_list;
extern int ntr_adaptor_count;

enum {
    NTR_ADAPTOR_PRE_ANY,
    NTR_ADAPTOR_PRE_COUNT,
};

enum {
    NTR_ADAPTOR_POST_AUTO,
    NTR_ADAPTOR_POST_REFRESH,
    NTR_ADAPTOR_POST_COUNT,
};

#define NTR_ADAPTOR_EXTRA_COUNT (NTR_ADAPTOR_PRE_COUNT + NTR_ADAPTOR_POST_COUNT)

void ntr_try_auto_select_adaptor(void);
void ntr_detect_3ds_ip(void);
void ntr_get_adapter_list(void);

#endif
