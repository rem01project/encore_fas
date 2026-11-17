// SPDX-License-Identifier: GPL-2.0-only

#include <linux/module.h>

#include "fas_lkm_backend.h"

static bool fas_measure;
module_param_named(measure, fas_measure, bool, 0644);
MODULE_PARM_DESC(measure, "Time the handlers. See handler_* in sysfs parameters/.");

static atomic64_t fas_dbg_calls = ATOMIC64_INIT(0);
static atomic64_t fas_dbg_ns_sum = ATOMIC64_INIT(0);
static atomic64_t fas_dbg_ns_max = ATOMIC64_INIT(0);

static int fas_dbg_get(char *buf, const struct kernel_param *kp) {
    return scnprintf(buf, PAGE_SIZE, "%lld\n", (long long)atomic64_read((atomic64_t *)kp->arg));
}

static const struct kernel_param_ops fas_dbg_ops = {.get = fas_dbg_get};
module_param_cb(handler_calls, &fas_dbg_ops, &fas_dbg_calls, 0444);
module_param_cb(handler_ns_sum, &fas_dbg_ops, &fas_dbg_ns_sum, 0444);
module_param_cb(handler_ns_max, &fas_dbg_ops, &fas_dbg_ns_max, 0444);

static __always_inline u64 fas_t0(void) {
    return unlikely(READ_ONCE(fas_measure)) ? ktime_get_mono_fast_ns() : 0;
}

static __always_inline void fas_t1(u64 t0) {
    u64 dt;

    if (likely(!t0))
        return;
    dt = ktime_get_mono_fast_ns() - t0;
    atomic64_inc(&fas_dbg_calls);
    atomic64_add(dt, &fas_dbg_ns_sum);
    if (dt > (u64)atomic64_read(&fas_dbg_ns_max))
        atomic64_set(&fas_dbg_ns_max, dt);
}

static bool fas_filter(FAS_UPROBE_FILTER_ARGS) {
    return mm == container_of(uc, struct fas_probe, uc)->ctx->mm;
}

static int fas_h_dq_enter(FAS_UPROBE_HANDLER_ARGS) {
    struct fas_ctx *ctx = container_of(uc, struct fas_probe, uc)->ctx;
    u64 t0 = fas_t0();

    fas_on_dequeue_enter(ctx, regs->regs[0], regs->regs[1]);
    fas_t1(t0);
    return 0;
}

static int fas_h_dq_ret(FAS_UPROBE_RET_HANDLER_ARGS) {
    struct fas_ctx *ctx = container_of(uc, struct fas_probe, uc)->ctx;
    u64 t0 = fas_t0();

    fas_on_dequeue_ret(ctx, (s32)regs->regs[0]);
    fas_t1(t0);
    return 0;
}

static int fas_h_q_enter(FAS_UPROBE_HANDLER_ARGS) {
    struct fas_ctx *ctx = container_of(uc, struct fas_probe, uc)->ctx;
    u64 t0 = fas_t0();

    fas_on_queue_enter(ctx, regs->regs[0], regs->regs[1]);
    fas_t1(t0);
    return 0;
}

int fas_uprobe_register(struct inode *inode, struct fas_probe *probe, u32 kind) {
#ifdef FAS_UPROBE_OBJ
    struct uprobe *up;
#endif

    if (kind == FAS_PROBE_DEQUEUE) {
        probe->uc.handler = fas_h_dq_enter;
        probe->uc.ret_handler = fas_h_dq_ret;
    } else {
        probe->uc.handler = fas_h_q_enter;
    }
    probe->uc.filter = fas_filter;

#ifdef FAS_UPROBE_OBJ
    up = uprobe_register(inode, probe->offset, 0, &probe->uc);

    if (IS_ERR(up))
        return PTR_ERR(up);
    probe->up = up;
    return 0;
#else
    return uprobe_register(inode, probe->offset, &probe->uc);
#endif
}

/* Handlers may still be running on other CPUs until fas_uprobe_sync() returns. */
void fas_uprobe_unregister(struct inode *inode, struct fas_probe *probe) {
#ifdef FAS_UPROBE_OBJ
    uprobe_unregister_nosync(probe->up, &probe->uc);
    probe->up = NULL;
#else
    uprobe_unregister(inode, probe->offset, &probe->uc);
#endif
}

void fas_uprobe_sync(void) {
#ifdef FAS_UPROBE_OBJ
    uprobe_unregister_sync();
#endif
}
