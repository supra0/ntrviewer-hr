#ifndef PLACEBO_H
#define PLACEBO_H

#ifdef __cplusplus
extern "C" {
#endif

#include <libplacebo/log.h>

#include <stdint.h>

pl_log placebo_log_create(void);
void placebo_log_destroy(pl_log *log);

struct placebo_t;
struct placebo_t *placebo_load(const char *filename);
void placebo_unload(struct placebo_t *placebo);

size_t placebo_mode_count(struct placebo_t *placebo);
const char *placebo_mode_name(struct placebo_t *placebo, size_t index, const char *prefix);

#ifdef __cplusplus
}
#endif

#endif
