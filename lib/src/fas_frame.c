// SPDX-License-Identifier: BSD-2-Clause

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "fas/fas_frame.h"

#define MAX_ENTERS 4
#define MAX_INFLIGHT 8
#define MAX_SURFACES 4

struct enter_slot {
    bool valid;
    int32_t tid;
    uint64_t ts_ns;
};

struct inflight_slot {
    bool valid;
    bool has_enter;
    uint8_t flags;
    int32_t tid;
    uint64_t surface;
    uint64_t buffer;
    uint64_t enter_ts_ns;
    uint64_t ret_ts_ns;
    uint64_t exec_ns;
    uint64_t delay_ns;
};

struct surface_slot {
    bool valid;
    uint64_t surface;
    uint64_t last_queue_ns;
};

struct fas_frame_ctx {
    struct enter_slot enters[MAX_ENTERS];
    struct inflight_slot inflight[MAX_INFLIGHT];
    struct surface_slot surfaces[MAX_SURFACES];
    uint32_t pending_lost;
    bool resync;
    bool q2q_only;
    struct fas_frame_stats stats;
} __attribute__((aligned(64)));

fas_frame_ctx_t *fas_frame_create(void) {
    void *mem = NULL;

    if (posix_memalign(&mem, 64, sizeof(struct fas_frame_ctx)) != 0)
        return NULL;
    memset(mem, 0, sizeof(struct fas_frame_ctx));
    return mem;
}

void fas_frame_destroy(fas_frame_ctx_t *ctx) {
    free(ctx);
}

void fas_frame_reset(fas_frame_ctx_t *ctx) {
    bool q2q_only;

    if (!ctx)
        return;
    q2q_only = ctx->q2q_only;
    memset(ctx, 0, sizeof(*ctx));
    ctx->q2q_only = q2q_only;
}

void fas_frame_set_mode(fas_frame_ctx_t *ctx, uint32_t mode) {
    if (ctx)
        ctx->q2q_only = mode == FAS_MODE_Q2Q;
}

void fas_frame_get_stats(const fas_frame_ctx_t *ctx, struct fas_frame_stats *stats) {
    if (ctx && stats)
        *stats = ctx->stats;
}

static struct enter_slot *enter_pick(struct fas_frame_ctx *ctx, int32_t tid) {
    struct enter_slot *best = &ctx->enters[0];

    for (int i = 0; i < MAX_ENTERS; i++) {
        struct enter_slot *s = &ctx->enters[i];

        if (s->valid && s->tid == tid)
            return s;
        if (!s->valid)
            best = s;
        else if (best->valid && s->ts_ns < best->ts_ns)
            best = s;
    }
    return best;
}

static bool enter_take(struct fas_frame_ctx *ctx, int32_t tid, uint64_t *ts_ns) {
    for (int i = 0; i < MAX_ENTERS; i++) {
        struct enter_slot *s = &ctx->enters[i];

        if (s->valid && s->tid == tid) {
            *ts_ns = s->ts_ns;
            s->valid = false;
            return true;
        }
    }
    return false;
}

static struct inflight_slot *inflight_pick(struct fas_frame_ctx *ctx, uint64_t buffer) {
    struct inflight_slot *best = &ctx->inflight[0];

    for (int i = 0; i < MAX_INFLIGHT; i++) {
        struct inflight_slot *s = &ctx->inflight[i];

        if (s->valid && s->buffer == buffer)
            return s;
        if (!s->valid)
            best = s;
        else if (best->valid && s->ret_ts_ns < best->ret_ts_ns)
            best = s;
    }
    return best;
}

static struct surface_slot *surface_pick(struct fas_frame_ctx *ctx, uint64_t surface) {
    struct surface_slot *best = &ctx->surfaces[0];

    for (int i = 0; i < MAX_SURFACES; i++) {
        struct surface_slot *s = &ctx->surfaces[i];

        if (s->valid && s->surface == surface)
            return s;
        if (!s->valid)
            best = s;
        else if (best->valid && s->last_queue_ns < best->last_queue_ns)
            best = s;
    }
    return best;
}

static void on_lost(struct fas_frame_ctx *ctx, const struct fas_event *ev) {
    uint32_t lost = ev->arg > 0 ? (uint32_t)ev->arg : 0;

    memset(ctx->enters, 0, sizeof(ctx->enters));
    memset(ctx->inflight, 0, sizeof(ctx->inflight));
    memset(ctx->surfaces, 0, sizeof(ctx->surfaces));
    ctx->pending_lost += lost;
    ctx->stats.lost_events += lost;
    ctx->stats.resyncs++;
    ctx->resync = true;
}

static void on_dequeue_ret(struct fas_frame_ctx *ctx, const struct fas_event *ev) {
    struct inflight_slot *s;
    uint64_t enter_ts = 0;
    bool has_enter = enter_take(ctx, ev->tid, &enter_ts);

    if (ev->arg < 0 || ev->buffer == 0)
        return;

    s = inflight_pick(ctx, ev->buffer);
    s->valid = true;
    s->has_enter = has_enter && ev->ts_ns >= enter_ts;
    s->flags = ev->flags;
    s->tid = ev->tid;
    s->surface = ev->surface;
    s->buffer = ev->buffer;
    s->enter_ts_ns = enter_ts;
    s->ret_ts_ns = ev->ts_ns;
    s->exec_ns = ev->exec_ns;
    s->delay_ns = ev->delay_ns;
}

static void on_queue(struct fas_frame_ctx *ctx, const struct fas_event *ev, struct fas_frame *out) {
    struct surface_slot *surf = surface_pick(ctx, ev->surface);
    struct inflight_slot *s = NULL;

    memset(out, 0, sizeof(*out));
    out->ts_ns = ev->ts_ns;
    out->surface = ev->surface;
    out->buffer = ev->buffer;
    out->tid = ev->tid;

    if (surf->valid && surf->surface == ev->surface && ev->ts_ns >= surf->last_queue_ns) {
        out->q2q_ns = ev->ts_ns - surf->last_queue_ns;
        out->flags |= FAS_FRAME_HAS_Q2Q;
    }
    surf->valid = true;
    surf->surface = ev->surface;
    surf->last_queue_ns = ev->ts_ns;

    for (int i = 0; i < MAX_INFLIGHT; i++) {
        struct inflight_slot *c = &ctx->inflight[i];

        if (c->valid && c->buffer == ev->buffer && c->surface == ev->surface) {
            s = c;
            break;
        }
    }

    if (s && ev->ts_ns >= s->ret_ts_ns) {
        if (s->has_enter) {
            out->dequeue_wait_ns = s->ret_ts_ns - s->enter_ts_ns;
            out->hold_ns = ev->ts_ns - s->ret_ts_ns;
            out->flags |= FAS_FRAME_HAS_DEQUEUE;
        }
        if (s->tid == ev->tid && !((s->flags | ev->flags) & FAS_EVF_NO_SCHED) && ev->exec_ns >= s->exec_ns && ev->delay_ns >= s->delay_ns) {
            out->exec_delta_ns = ev->exec_ns - s->exec_ns;
            out->delay_delta_ns = ev->delay_ns - s->delay_ns;
            out->flags |= FAS_FRAME_HAS_SCHED;
        }
    }
    if (s)
        s->valid = false;

    if (!ctx->q2q_only && !(out->flags & FAS_FRAME_HAS_DEQUEUE))
        ctx->stats.unmatched++;

    if (ctx->resync) {
        out->flags |= FAS_FRAME_RESYNC;
        out->lost_events = ctx->pending_lost;
        ctx->pending_lost = 0;
        ctx->resync = false;
    }
    ctx->stats.frames++;
}

int fas_frame_feed(fas_frame_ctx_t *ctx, const struct fas_event *ev, struct fas_frame *out) {
    if (!ctx || !ev || !out)
        return 0;

    switch (ev->type) {
        case FAS_EVENT_LOST:
            on_lost(ctx, ev);
            return 0;
        case FAS_EVENT_DEQUEUE_ENTER: {
            struct enter_slot *s = enter_pick(ctx, ev->tid);

            s->valid = true;
            s->tid = ev->tid;
            s->ts_ns = ev->ts_ns;
            return 0;
        }
        case FAS_EVENT_DEQUEUE_RET:
            on_dequeue_ret(ctx, ev);
            return 0;
        case FAS_EVENT_QUEUE_ENTER:
            on_queue(ctx, ev, out);
            return 1;
        default:
            return 0;
    }
}
