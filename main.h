#ifndef MAIN_H
#define MAIN_H

#include "const.h"
#include <stdatomic.h>

#ifdef _WIN32
#include <combaseapi.h>
#define RO_INIT() CoInitializeEx(NULL, COINIT_MULTITHREADED)
#define RO_UNINIT() CoUninitialize()
#else
#define RO_INIT()
#define RO_UNINIT()
#endif

extern atomic_bool program_running;

void itimeofday(int64_t *sec, int64_t *usec);

UNUSED static inline int64_t iclock64(void)
{
    int64_t s, u;
    int64_t value;
    itimeofday(&s, &u);
    value = ((int64_t)s) * 1000000 + u;
    return value;
}

UNUSED static inline uint32_t iclock()
{
    return (uint32_t)(iclock64() & 0xfffffffful);
}

#include "rp_syn.h"
UNUSED static int acquire_sem(rp_sem_t *sem) {
    while (1) {
        if (!program_running)
            return -1;
        int res = rp_sem_timedwait(*sem, NWM_THREAD_WAIT_NS, NULL);
        if (res == 0)
            return 0;
        if (res != ETIMEDOUT) {
            return -1;
        }
    }
}

UNUSED static void cond_mutex_flag_signal(event_t *event) {
  event_rel(event);
}

UNUSED static bool cond_mutex_flag_lock(event_t *event) {
    while (1) {
        if (!program_running)
            return false;
        int res = event_wait(event, NWM_THREAD_WAIT_NS);
        if (res == ETIMEDOUT) {
            continue;
        } else if (res) {
            return false;
        } else {
            break;
        }
    }
    return true;
}

#endif
