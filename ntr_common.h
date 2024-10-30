#ifndef NTR_COMMON_H
#define NTR_COMMON_H

#include "nuklear/nuklear.h"

#include <stdatomic.h>

int sock_startup(void);
int sock_cleanup(void);

extern atomic_uint_fast8_t ntr_ip_octet[4];

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
