// SPDX-License-Identifier: BSD-2-Clause

#include <linux/bpf.h>
#include <linux/types.h>
#include <bpf/bpf_helpers.h>
#include <stdbool.h>

struct pt_regs {
    __u64 regs[31];
    __u64 sp;
    __u64 pc;
    __u64 pstate;
};

#include "uapi/encore_fas_uapi.h"

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, FAS_BPF_RINGBUF_BYTES);
} fas_events SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct fas_bpf_cfg);
} fas_cfg SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_LRU_HASH);
    __uint(max_entries, FAS_BPF_STASH_ENTRIES);
    __type(key, __u32);
    __type(value, struct fas_bpf_stash);
} fas_stash SEC(".maps");

struct {
    __uint(type, BPF_MAP_TYPE_PERCPU_ARRAY);
    __uint(max_entries, 1);
    __type(key, __u32);
    __type(value, struct fas_bpf_pcpu);
} fas_pcpu SEC(".maps");

#define FAS_UNTAG(p) ((p) & 0x00ffffffffffffffULL)

struct fas_bpf_ctx {
    const struct fas_bpf_cfg *bc;
    struct fas_bpf_pcpu *pc;
};

typedef struct {
    __u32 cpu;
} fas_guard_t;

#define FAS_CTX struct fas_bpf_ctx
#define FAS_GUARD fas_guard_t
#define FAS_INLINE static __always_inline

FAS_INLINE __u64 fas_be_now(void) {
    return bpf_ktime_get_ns();
}

FAS_INLINE __s32 fas_be_tid(void) {
    return (__s32)bpf_get_current_pid_tgid();
}

FAS_INLINE bool fas_be_sched(FAS_CTX *ctx, __u64 *exec_ns, __u64 *delay_ns) {
    __u32 off_exec = ctx->bc->off_sum_exec_runtime;
    __u32 off_delay = ctx->bc->off_run_delay;
    __u64 task_ptr;

    if (!off_exec || !off_delay)
        return false;

    task_ptr = bpf_get_current_task();
    if (bpf_probe_read_kernel(exec_ns, sizeof(*exec_ns), (const void *)(task_ptr + off_exec)) ||
        bpf_probe_read_kernel(delay_ns, sizeof(*delay_ns), (const void *)(task_ptr + off_delay)))
        return false;

    return true;
}

FAS_INLINE int fas_be_user_u64(__u64 uptr, __u64 *out) {
    return bpf_probe_read_user(out, sizeof(*out), (const void *)FAS_UNTAG(uptr));
}

FAS_INLINE void fas_be_stash_put(FAS_CTX *ctx, __s32 tid, const struct fas_bpf_stash *stash) {
    __u32 key = (__u32)tid;

    bpf_map_update_elem(&fas_stash, &key, stash, BPF_ANY);
}

FAS_INLINE bool fas_be_stash_take(FAS_CTX *ctx, __s32 tid, struct fas_bpf_stash *out) {
    __u32 key = (__u32)tid;
    struct fas_bpf_stash *entry = bpf_map_lookup_elem(&fas_stash, &key);

    if (!entry)
        return false;
    *out = *entry;
    bpf_map_delete_elem(&fas_stash, &key);
    return true;
}

FAS_INLINE struct fas_event *fas_be_reserve(FAS_CTX *ctx, fas_guard_t *guard, bool count_lost) {
    struct fas_event *ev = bpf_ringbuf_reserve(&fas_events, sizeof(*ev), 0);

    if (!ev) {
        if (count_lost)
            ctx->pc->pending_lost++;
        return NULL;
    }
    guard->cpu = bpf_get_smp_processor_id();
    return ev;
}

FAS_INLINE void fas_be_commit(FAS_CTX *ctx, fas_guard_t *guard, struct fas_event *ev, __u16 type, __u8 flags, bool wake) {
    ev->type = type;
    ev->cpu = (__u8)guard->cpu;
    ev->flags = flags;
    ev->reserved1 = 0;
    bpf_ringbuf_submit(ev, wake ? BPF_RB_FORCE_WAKEUP : BPF_RB_NO_WAKEUP);
}

FAS_INLINE __u32 fas_be_lost_take(FAS_CTX *ctx) {
    __u32 count = ctx->pc->pending_lost;

    ctx->pc->pending_lost = 0;
    return count;
}

FAS_INLINE void fas_be_lost_put(FAS_CTX *ctx, __u32 count) {
    ctx->pc->pending_lost += count;
}

#include "fas_core.h"

#define FAS_PROLOGUE()                                                       \
    __u32 map_zero = 0;                                                      \
    const struct fas_bpf_cfg *bc = bpf_map_lookup_elem(&fas_cfg, &map_zero); \
    struct fas_bpf_pcpu *pc = bpf_map_lookup_elem(&fas_pcpu, &map_zero);     \
    FAS_CTX c;                                                               \
    if (!bc || !pc || bc->abi != FAS_ABI_VERSION)                            \
        return 0;                                                            \
    c.bc = bc;                                                               \
    c.pc = pc

SEC("uprobe")
int fas_dequeue_enter(struct pt_regs *ctx) {
    FAS_PROLOGUE();

    fas_on_dequeue_enter(&c, ctx->regs[0], ctx->regs[1]);
    return 0;
}

SEC("uretprobe")
int fas_dequeue_ret(struct pt_regs *ctx) {
    FAS_PROLOGUE();

    fas_on_dequeue_ret(&c, (__s32)ctx->regs[0]);
    return 0;
}

SEC("uprobe")
int fas_queue_enter(struct pt_regs *ctx) {
    FAS_PROLOGUE();

    fas_on_queue_enter(&c, ctx->regs[0], ctx->regs[1]);
    return 0;
}

char LICENSE[] SEC("license") = "Dual BSD/GPL";
