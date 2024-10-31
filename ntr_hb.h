#ifndef NTR_HB_H
#define NTR_HB_H

#include "const.h"
#include <stdatomic.h>

struct tcp_thread_arg {
    atomic_int *work_state;
    atomic_bool *remote_play;
    short port;
};

extern atomic_int menu_work_state;
extern atomic_int nwm_work_state;
extern atomic_bool menu_remote_play;

enum connection_state_t
{
    CONNECTION_STATE_DISCONNECTED,
    CONNECTION_STATE_CONNECTING,
    CONNECTION_STATE_CONNECTED,
    CONNECTION_STATE_DISCONNECTING,
    CONNECTION_STATE_COUNT,
};
extern enum connection_state_t menu_connection, nwm_connection;

thread_ret_t tcp_thread_func(void *arg);

#endif
