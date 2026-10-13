// SPDX-License-Identifier: GPL-2.0-only

#ifndef FAS_RING_H
#define FAS_RING_H

#include "uapi/encore_fas_uapi.h"

#ifdef __AARCH64EB__
    #error "encore_fas requires a little-endian CPU architecture."
#endif

#define FAS_RING_RECORDS 4096u
#define FAS_RING_MASK (FAS_RING_RECORDS - 1u)
#define FAS_RING_MAX_READ 1024u

#define FAS_STASH_SLOTS 256u
#define FAS_STASH_WINDOW 4u
#define FAS_STASH_SHIFT 8u

struct fas_ring {
    struct fas_event *slot;
    u32 prod ____cacheline_aligned_in_smp;
    u32 cons ____cacheline_aligned_in_smp;
};

static __always_inline u64 *fas_tail(struct fas_event *ev) {
    return (u64 *)&ev->type;
}

static __always_inline u64 fas_tail_pack(u16 type, u8 cpu, u8 flags) {
    return (u64)type | ((u64)cpu << 16) | ((u64)flags << 24);
}

static __always_inline struct fas_event *fas_ring_claim(struct fas_ring *ring) {
    u32 prod = READ_ONCE(ring->prod);

    for (;;) {
        u32 cons = smp_load_acquire(&ring->cons);
        u32 old_prod;

        if (unlikely(prod - cons >= FAS_RING_RECORDS)) {
            u32 fresh_prod = READ_ONCE(ring->prod);

            if (fresh_prod == prod)
                return NULL;
            prod = fresh_prod;
            continue;
        }
        old_prod = cmpxchg_relaxed(&ring->prod, prod, prod + 1u);
        if (likely(old_prod == prod))
            return &ring->slot[prod & FAS_RING_MASK];
        prod = old_prod;
    }
}

static __always_inline void fas_ring_publish(struct fas_event *ev, u64 tail) {
    smp_store_release(fas_tail(ev), tail);
}

static __always_inline bool fas_ring_head_ready(struct fas_ring *ring) {
    u64 tail_word = smp_load_acquire(
        fas_tail(&ring->slot[READ_ONCE(ring->cons) & FAS_RING_MASK]));

    return (u16)tail_word != 0;
}

static __always_inline u32 fas_ring_ready(struct fas_ring *ring, u32 max) {
    u32 cons = READ_ONCE(ring->cons);
    u32 ready_count = 0;

    if (max > FAS_RING_RECORDS)
        max = FAS_RING_RECORDS;

    while (ready_count < max) {
        u64 tail_word = smp_load_acquire(
            fas_tail(&ring->slot[(cons + ready_count) & FAS_RING_MASK]));

        if (!(u16)tail_word)
            break;
        ready_count++;
    }
    return ready_count;
}

static __always_inline void fas_ring_consume(struct fas_ring *ring, u32 count) {
    u32 cons = READ_ONCE(ring->cons);
    u32 i;

    for (i = 0; i < count; i++)
        WRITE_ONCE(*fas_tail(&ring->slot[(cons + i) & FAS_RING_MASK]), 0);
    smp_store_release(&ring->cons, cons + count);
}

struct fas_stash_slot {
    u32 tid;
    u32 pad;
    struct fas_bpf_stash v;
} __aligned(32);

static __always_inline u32 fas_stash_hash(u32 tid) {
    return (tid * 0x9E3779B1u) >> (32u - FAS_STASH_SHIFT);
}

static __always_inline void fas_stash_put(struct fas_stash_slot *table, u32 tid, const struct fas_bpf_stash *src) {
    struct fas_stash_slot *target = NULL;
    u32 hash_idx = fas_stash_hash(tid);
    u32 i;

    for (i = 0; i < FAS_STASH_WINDOW; i++) {
        struct fas_stash_slot *curr = &table[(hash_idx + i) & (FAS_STASH_SLOTS - 1u)];

        if (READ_ONCE(curr->tid) == tid) {
            target = curr;
            break;
        }
    }
    for (i = 0; !target && i < FAS_STASH_WINDOW; i++) {
        struct fas_stash_slot *curr = &table[(hash_idx + i) & (FAS_STASH_SLOTS - 1u)];

        if (READ_ONCE(curr->tid) == 0 &&
            cmpxchg_relaxed(&curr->tid, 0u, tid) == 0u)
            target = curr;
    }
    if (!target) {
        target = &table[hash_idx & (FAS_STASH_SLOTS - 1u)];
        WRITE_ONCE(target->tid, tid);
    }

    target->v.surface = src->surface;
    target->v.out_buffer_ptr = src->out_buffer_ptr;
}

static __always_inline bool fas_stash_take(struct fas_stash_slot *table, u32 tid, struct fas_bpf_stash *dst) {
    u32 hash_idx = fas_stash_hash(tid);
    u32 i;

    for (i = 0; i < FAS_STASH_WINDOW; i++) {
        struct fas_stash_slot *slot = &table[(hash_idx + i) & (FAS_STASH_SLOTS - 1u)];

        if (READ_ONCE(slot->tid) != tid)
            continue;
        dst->surface = slot->v.surface;
        dst->out_buffer_ptr = slot->v.out_buffer_ptr;
        smp_store_release(&slot->tid, 0u);
        return true;
    }
    return false;
}

#endif
