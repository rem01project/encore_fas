// SPDX-License-Identifier: GPL-2.0-only

#ifndef FAS_LKM_BE_H
#define FAS_LKM_BE_H

#include <linux/atomic.h>
#include <linux/compiler.h>
#include <linux/irqflags.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/path.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/smp.h>
#include <linux/timekeeping.h>
#include <linux/uaccess.h>
#include <linux/uprobes.h>
#include <linux/version.h>
#include <linux/wait.h>

#include "fas_ring.h"
#include "uapi/encore_fas_uapi.h"

#ifndef CONFIG_ARM64
    #error "encore_fas supports arm64 architecture only."
#endif

#ifndef CONFIG_UPROBES
    #error "encore_fas requires kernel CONFIG_UPROBES support."
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 14, 0)
    #error "encore_fas requires Linux Kernel 4.14 or later."
#endif

/*
 * uprobe consumer API differences:
 *
 *   < 6.12   uprobe_register(inode, off, uc) returns int, uprobe_unregister(inode, off, uc)
 *            is synchronous, filter() takes an extra enum uprobe_filter_ctx.
 *   >= 6.12  uprobe_register(inode, off, ref_ctr_off, uc) returns struct uprobe * or ERR_PTR,
 *            teardown is uprobe_unregister_nosync() + one uprobe_unregister_sync(),
 *            filter() has no ctx argument.
 *   >= 6.13  handler() and ret_handler() take a trailing __u64 *data.
 */
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 12, 0)
    #define FAS_UPROBE_OBJ 1
    #define FAS_UPROBE_FILTER_ARGS struct uprobe_consumer *uc, struct mm_struct *mm
#else
    #define FAS_UPROBE_FILTER_ARGS struct uprobe_consumer *uc, enum uprobe_filter_ctx ctx, struct mm_struct *mm
#endif

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 13, 0)
    #define FAS_UPROBE_HANDLER_ARGS struct uprobe_consumer *uc, struct pt_regs *regs, __u64 *data
    #define FAS_UPROBE_RET_HANDLER_ARGS struct uprobe_consumer *uc, unsigned long func, struct pt_regs *regs, __u64 *data
#else
    #define FAS_UPROBE_HANDLER_ARGS struct uprobe_consumer *uc, struct pt_regs *regs
    #define FAS_UPROBE_RET_HANDLER_ARGS struct uprobe_consumer *uc, unsigned long func, struct pt_regs *regs
#endif

struct fas_ctx;

struct fas_probe {
    struct uprobe_consumer uc;
    struct fas_ctx *ctx;
    loff_t offset;
#ifdef FAS_UPROBE_OBJ
    struct uprobe *up;
#endif
};

struct fas_ctx {
    struct fas_ring ring;
    struct fas_stash_slot *stash;
    struct mm_struct *mm;
    atomic_t lost ____cacheline_aligned_in_smp;
    wait_queue_head_t wq;
    struct mutex ctl_lock;
    struct mutex read_lock;
    struct fas_probe probe[FAS_MAX_PROBES];
    u32 nprobes;
    struct path path;
    bool registered;
};

#define FAS_CTX struct fas_ctx

typedef struct {
    unsigned long irq;
    u8 cpu;
} fas_guard_t;

#define FAS_GUARD fas_guard_t

static __always_inline u64 fas_be_now(void) {
    return ktime_get_mono_fast_ns();
}

static __always_inline s32 fas_be_tid(void) {
    return current->pid;
}

static __always_inline bool fas_be_sched(struct fas_ctx *ctx, u64 *exec_ns, u64 *delay_ns) {
#ifdef CONFIG_SCHED_INFO
    *exec_ns = READ_ONCE(current->se.sum_exec_runtime);
    *delay_ns = READ_ONCE(current->sched_info.run_delay);
    return true;
#else
    return false;
#endif
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 0, 0)
    #define FAS_ACCESS_OK(p, n) access_ok((p), (n))
#else
    #define FAS_ACCESS_OK(p, n) access_ok(VERIFY_READ, (p), (n))
#endif

#ifndef FAS_UNTAG
    #define FAS_UNTAG(p) ((p) & 0x00ffffffffffffffULL)
#endif

static __always_inline int fas_be_user_u64(u64 uptr, u64 *out) {
    u64 __user *p = (u64 __user *)(unsigned long)FAS_UNTAG(uptr);
    int ret;

    if (!FAS_ACCESS_OK(p, sizeof(*p)))
        return -EFAULT;
    pagefault_disable();
    ret = __get_user(*out, p);
    pagefault_enable();
    return ret;
}

static __always_inline void fas_be_stash_put(struct fas_ctx *ctx, s32 tid, const struct fas_bpf_stash *s) {
    fas_stash_put(ctx->stash, (u32)tid, s);
}

static __always_inline bool fas_be_stash_take(struct fas_ctx *ctx, s32 tid, struct fas_bpf_stash *s) {
    return fas_stash_take(ctx->stash, (u32)tid, s);
}

static __always_inline struct fas_event *
fas_be_reserve(struct fas_ctx *ctx, fas_guard_t *guard, bool count_lost) {
    struct fas_event *ev;

    local_irq_save(guard->irq);
    ev = fas_ring_claim(&ctx->ring);
    if (unlikely(!ev)) {
        if (count_lost)
            atomic_inc(&ctx->lost);
        local_irq_restore(guard->irq);
        return NULL;
    }
    guard->cpu = (u8)smp_processor_id();
    return ev;
}

static __always_inline void fas_be_commit(struct fas_ctx *ctx, fas_guard_t *guard, struct fas_event *ev, u16 type, u8 flags, bool wake) {
    fas_ring_publish(ev, fas_tail_pack(type, guard->cpu, flags));
    local_irq_restore(guard->irq);

    if (wake) {
        smp_mb();
        if (waitqueue_active(&ctx->wq))
            wake_up_interruptible(&ctx->wq);
    }
}

static __always_inline u32 fas_be_lost_take(struct fas_ctx *ctx) {
    if (likely(!atomic_read(&ctx->lost)))
        return 0;
    return (u32)atomic_xchg(&ctx->lost, 0);
}

static __always_inline void fas_be_lost_put(struct fas_ctx *ctx, u32 n) {
    atomic_add((int)n, &ctx->lost);
}

#include "fas_core.h"

#endif
