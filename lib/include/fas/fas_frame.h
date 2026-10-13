// SPDX-License-Identifier: BSD-2-Clause

#ifndef FAS_FAS_FRAME_H
#define FAS_FAS_FRAME_H

#include <stdint.h>

#include "fas/fas.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAS_FRAME_HAS_Q2Q (1u << 0)
#define FAS_FRAME_HAS_DEQUEUE (1u << 1)
#define FAS_FRAME_HAS_SCHED (1u << 2)
#define FAS_FRAME_RESYNC (1u << 3)

struct fas_frame {
    uint64_t ts_ns;
    uint64_t surface;
    uint64_t buffer;
    uint64_t q2q_ns;
    uint64_t dequeue_wait_ns;
    uint64_t hold_ns;
    uint64_t exec_delta_ns;
    uint64_t delay_delta_ns;
    int32_t tid;
    uint32_t flags;
    uint32_t lost_events;
    uint32_t reserved;
};

struct fas_frame_stats {
    uint64_t frames;
    uint64_t unmatched;
    uint64_t resyncs;
    uint64_t lost_events;
};

typedef struct fas_frame_ctx fas_frame_ctx_t;

FAS_API fas_frame_ctx_t *fas_frame_create(void);

FAS_API void fas_frame_destroy(fas_frame_ctx_t *ctx);

FAS_API void fas_frame_reset(fas_frame_ctx_t *ctx);

FAS_API void fas_frame_set_mode(fas_frame_ctx_t *ctx, uint32_t mode);

FAS_API int fas_frame_feed(fas_frame_ctx_t *ctx, const struct fas_event *ev, struct fas_frame *out);

FAS_API void fas_frame_get_stats(const fas_frame_ctx_t *ctx, struct fas_frame_stats *stats);

#ifdef __cplusplus
}
#endif

#endif
