// SPDX-License-Identifier: BSD-2-Clause

#ifndef FAS_CORE_H
#define FAS_CORE_H

#include "uapi/encore_fas_uapi.h"

#ifndef FAS_INLINE
    #define FAS_INLINE static inline __attribute__((always_inline))
#endif

#define FAS_LIKELY(x) __builtin_expect(!!(x), 1)
#define FAS_UNLIKELY(x) __builtin_expect(!!(x), 0)

#ifndef FAS_UNTAG
    #define FAS_UNTAG(p) ((p) & 0x00ffffffffffffffULL)
#endif

struct fas_stamp {
    __u64 ts_ns;
    __u64 exec_ns;
    __u64 delay_ns;
    __s32 tid;
    __u8 flags;
};

FAS_INLINE void fas_stamp_get(FAS_CTX *ctx, struct fas_stamp *stamp) {
    stamp->ts_ns = fas_be_now();
    if (fas_be_sched(ctx, &stamp->exec_ns, &stamp->delay_ns)) {
        stamp->flags = 0;
    } else {
        stamp->exec_ns = 0;
        stamp->delay_ns = 0;
        stamp->flags = FAS_EVF_NO_SCHED;
    }
    stamp->tid = fas_be_tid();
}

FAS_INLINE void fas_put(FAS_CTX *ctx, const struct fas_stamp *stamp, __u16 type, __u64 surface, __u64 buffer, __s32 arg) {
    struct fas_event *ev;
    FAS_GUARD guard;
    __u32 lost_count = fas_be_lost_take(ctx);

    if (FAS_UNLIKELY(lost_count)) {
        ev = fas_be_reserve(ctx, &guard, false);
        if (ev) {
            ev->ts_ns = stamp->ts_ns;
            ev->surface = 0;
            ev->buffer = 0;
            ev->exec_ns = 0;
            ev->delay_ns = 0;
            ev->tid = stamp->tid;
            ev->arg = (__s32)lost_count;
            ev->reserved0 = 0;
            fas_be_commit(ctx, &guard, ev, FAS_EVENT_LOST, FAS_EVF_NO_SCHED, false);
        } else {
            fas_be_lost_put(ctx, lost_count);
        }
    }

    ev = fas_be_reserve(ctx, &guard, true);
    if (FAS_UNLIKELY(!ev))
        return;

    ev->ts_ns = stamp->ts_ns;
    ev->surface = surface;
    ev->buffer = buffer;
    ev->exec_ns = stamp->exec_ns;
    ev->delay_ns = stamp->delay_ns;
    ev->tid = stamp->tid;
    ev->arg = arg;
    ev->reserved0 = 0;

    fas_be_commit(ctx, &guard, ev, type, stamp->flags, type == FAS_EVENT_QUEUE_ENTER);
}

FAS_INLINE void fas_on_dequeue_enter(FAS_CTX *ctx, __u64 surface, __u64 out_buf) {
    struct fas_bpf_stash stash_data = {
        .surface = surface,
        .out_buffer_ptr = out_buf,
    };
    struct fas_stamp stamp;

    fas_stamp_get(ctx, &stamp);
    fas_be_stash_put(ctx, stamp.tid, &stash_data);
    fas_put(ctx, &stamp, FAS_EVENT_DEQUEUE_ENTER, surface, 0, 0);
}

FAS_INLINE void fas_on_dequeue_ret(FAS_CTX *ctx, __s32 status) {
    struct fas_bpf_stash stash_data;
    struct fas_stamp stamp;
    __u64 buffer = 0;

    fas_stamp_get(ctx, &stamp);
    if (!fas_be_stash_take(ctx, stamp.tid, &stash_data))
        return;

    if (status >= 0) {
        __u64 read_buffer;

        if (!fas_be_user_u64(stash_data.out_buffer_ptr, &read_buffer))
            buffer = read_buffer;
    }
    fas_put(ctx, &stamp, FAS_EVENT_DEQUEUE_RET, stash_data.surface, buffer, status);
}

FAS_INLINE void fas_on_queue_enter(FAS_CTX *ctx, __u64 surface, __u64 buffer) {
    struct fas_stamp stamp;

    fas_stamp_get(ctx, &stamp);
    fas_put(ctx, &stamp, FAS_EVENT_QUEUE_ENTER, surface, buffer, 0);
}

#endif
