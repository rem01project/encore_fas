// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/namei.h>
#include <linux/pid.h>
#include <linux/poll.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "fas_lkm_be.h"

#ifndef FAS_VERSION
    #define FAS_VERSION 0
#endif
#ifndef FAS_BRANCH
    #define FAS_BRANCH "-"
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
typedef unsigned int fas_poll_t;
    #define FAS_POLLIN (POLLIN | POLLRDNORM)
#else
typedef __poll_t fas_poll_t;
    #define FAS_POLLIN ((__force __poll_t)(EPOLLIN | EPOLLRDNORM))
#endif

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

static bool fas_filter(struct uprobe_consumer *uc, enum uprobe_filter_ctx ctx, struct mm_struct *mm) {
    return mm == container_of(uc, struct fas_probe, uc)->ctx->mm;
}

static int fas_h_dq_enter(struct uprobe_consumer *uc, struct pt_regs *regs) {
    struct fas_ctx *ctx = container_of(uc, struct fas_probe, uc)->ctx;
    u64 t0 = fas_t0();

    fas_on_dequeue_enter(ctx, regs->regs[0], regs->regs[1]);
    fas_t1(t0);
    return 0;
}

static int fas_h_dq_ret(struct uprobe_consumer *uc, unsigned long func, struct pt_regs *regs) {
    struct fas_ctx *ctx = container_of(uc, struct fas_probe, uc)->ctx;
    u64 t0 = fas_t0();

    fas_on_dequeue_ret(ctx, (s32)regs->regs[0]);
    fas_t1(t0);
    return 0;
}

static int fas_h_q_enter(struct uprobe_consumer *uc, struct pt_regs *regs) {
    struct fas_ctx *ctx = container_of(uc, struct fas_probe, uc)->ctx;
    u64 t0 = fas_t0();

    fas_on_queue_enter(ctx, regs->regs[0], regs->regs[1]);
    fas_t1(t0);
    return 0;
}

static bool fas_is_root(void) {
    return uid_eq(current_euid(), GLOBAL_ROOT_UID);
}

static int fas_register_check(const struct fas_register_args *args) {
    u32 i;

    if (args->pid <= 0 || args->reserved[0] || args->reserved[1] ||
        !args->probe_count || args->probe_count > FAS_MAX_PROBES)
        return -EINVAL;

    for (i = 0; i < args->probe_count; i++) {
        const struct fas_probe_spec *probe = &args->probes[i];

        if (probe->reserved || (probe->offset & 3) ||
            (probe->kind != FAS_PROBE_DEQUEUE &&
             probe->kind != FAS_PROBE_QUEUE))
            return -EINVAL;
    }
    return 0;
}

static struct mm_struct *fas_grab_mm(struct task_struct *task) {
    struct mm_struct *mm;

    task_lock(task);
    mm = task->mm;
    if (mm)
        mmgrab(mm);
    task_unlock(task);
    return mm;
}

static void fas_unregister(struct fas_ctx *ctx) {
    u32 i;

    for (i = 0; i < ctx->nprobes; i++)
        uprobe_unregister(d_inode(ctx->path.dentry), ctx->probe[i].offset, &ctx->probe[i].uc);
    ctx->nprobes = 0;
    path_put(&ctx->path);
    mmdrop(ctx->mm);
    ctx->registered = false;
}

static long fas_ioctl_register(struct fas_ctx *ctx, void __user *uarg) {
    struct fas_register_args req;
    struct task_struct *task;
    struct mm_struct *mm;
    struct inode *inode;
    struct pid *pid;
    u32 i;
    long ret;

    if (copy_from_user(&req, uarg, sizeof(req)))
        return -EFAULT;

    ret = fas_register_check(&req);
    if (ret)
        return ret;
    req.path[FAS_MAX_PATH_LEN - 1] = '\0';

    pid = find_get_pid(req.pid);
    if (!pid)
        return -ESRCH;

    task = get_pid_task(pid, PIDTYPE_PID);
    put_pid(pid);
    if (!task)
        return -ESRCH;

    mm = (task->flags & PF_KTHREAD) ? NULL : fas_grab_mm(task);
    put_task_struct(task);
    if (!mm)
        return -ESRCH;

    mutex_lock(&ctx->ctl_lock);
    if (ctx->registered) {
        ret = -EBUSY;
        goto err_mm;
    }

    ret = kern_path(req.path, LOOKUP_FOLLOW, &ctx->path);
    if (ret)
        goto err_mm;

    inode = d_inode(ctx->path.dentry);
    if (!S_ISREG(inode->i_mode)) {
        ret = -EINVAL;
        goto err_path;
    }

    for (i = 0; i < req.probe_count; i++) {
        if (req.probes[i].offset >= (u64)i_size_read(inode)) {
            ret = -EINVAL;
            goto err_path;
        }
    }

    ctx->mm = mm;
    for (i = 0; i < req.probe_count; i++) {
        struct fas_probe *probe = &ctx->probe[i];

        probe->ctx = ctx;
        probe->offset = (loff_t)req.probes[i].offset;
        memset(&probe->uc, 0, sizeof(probe->uc));

        if (req.probes[i].kind == FAS_PROBE_DEQUEUE) {
            probe->uc.handler = fas_h_dq_enter;
            probe->uc.ret_handler = fas_h_dq_ret;
        } else {
            probe->uc.handler = fas_h_q_enter;
        }
        probe->uc.filter = fas_filter;

        ret = uprobe_register(inode, probe->offset, &probe->uc);
        if (ret)
            goto err_probes;
        ctx->nprobes = i + 1;
    }

    ctx->registered = true;
    mutex_unlock(&ctx->ctl_lock);
    return 0;

err_probes:
    for (i = 0; i < ctx->nprobes; i++)
        uprobe_unregister(inode, ctx->probe[i].offset, &ctx->probe[i].uc);
    ctx->nprobes = 0;
err_path:
    path_put(&ctx->path);
err_mm:
    mmdrop(mm);
    mutex_unlock(&ctx->ctl_lock);
    return ret;
}

static int fas_open(struct inode *inode, struct file *file) {
    struct fas_ctx *ctx;

    if (!fas_is_root())
        return -EPERM;

    ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
    if (!ctx)
        return -ENOMEM;

    ctx->ring.slot = vzalloc(FAS_RING_RECORDS * sizeof(struct fas_event));
    ctx->stash = vzalloc(FAS_STASH_SLOTS * sizeof(struct fas_stash_slot));

    if (!ctx->ring.slot || !ctx->stash) {
        vfree(ctx->stash);
        vfree(ctx->ring.slot);
        kfree(ctx);
        return -ENOMEM;
    }

    init_waitqueue_head(&ctx->wq);
    mutex_init(&ctx->ctl_lock);
    mutex_init(&ctx->read_lock);
    atomic_set(&ctx->lost, 0);
    file->private_data = ctx;
    return nonseekable_open(inode, file);
}

static int fas_release(struct inode *inode, struct file *file) {
    struct fas_ctx *ctx = file->private_data;

    mutex_lock(&ctx->ctl_lock);
    if (ctx->registered)
        fas_unregister(ctx);
    mutex_unlock(&ctx->ctl_lock);

    vfree(ctx->stash);
    vfree(ctx->ring.slot);
    kfree(ctx);
    return 0;
}

static long fas_ioctl(struct file *file, unsigned int cmd, unsigned long arg) {
    struct fas_ctx *ctx = file->private_data;
    void __user *uarg = (void __user *)arg;

    if (!fas_is_root())
        return -EPERM;
    if (_IOC_TYPE(cmd) != FAS_IOC_MAGIC || _IOC_NR(cmd) > FAS_IOC_MAXNR)
        return -ENOTTY;

    switch (cmd) {
        case FAS_IOC_GET_VERSION: {
            struct fas_version ver = {
                .version = FAS_VERSION,
                .abi = FAS_ABI_VERSION,
                .caps = FAS_CAP_BACKEND_LKM |
#ifdef CONFIG_SCHED_INFO
                        FAS_CAP_SCHED_COUNTERS |
#endif
                        0,
                .rec_size = sizeof(struct fas_event),
                .queue_records = FAS_RING_RECORDS,
            };

            return copy_to_user(uarg, &ver, sizeof(ver)) ? -EFAULT : 0;
        }
        case FAS_IOC_REGISTER:
            return fas_ioctl_register(ctx, uarg);
        default:
            return -ENOTTY;
    }
}

static ssize_t fas_read(struct file *file, char __user *ubuf, size_t len, loff_t *ppos) {
    struct fas_ctx *ctx = file->private_data;
    struct fas_ring *ring = &ctx->ring;
    size_t want = min_t(size_t, len / sizeof(struct fas_event), FAS_RING_MAX_READ);
    u32 ready_records, cons, first_chunk;
    ssize_t ret;

    if (!want)
        return -EINVAL;
    if (mutex_lock_interruptible(&ctx->read_lock))
        return -ERESTARTSYS;

    for (;;) {
        ready_records = fas_ring_ready(ring, (u32)want);
        if (ready_records)
            break;
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            goto out;
        }
        if (wait_event_interruptible(ctx->wq, fas_ring_head_ready(ring))) {
            ret = -ERESTARTSYS;
            goto out;
        }
    }

    cons = ring->cons;
    first_chunk = min_t(u32, ready_records, FAS_RING_RECORDS - (cons & FAS_RING_MASK));

    if (copy_to_user(ubuf, &ring->slot[cons & FAS_RING_MASK], first_chunk * sizeof(struct fas_event)) ||
        (ready_records > first_chunk &&
         copy_to_user(ubuf + first_chunk * sizeof(struct fas_event),
                      ring->slot,
                      (ready_records - first_chunk) * sizeof(struct fas_event)))) {
        ret = -EFAULT;
        goto out;
    }

    fas_ring_consume(ring, ready_records);
    ret = (ssize_t)ready_records * sizeof(struct fas_event);
out:
    mutex_unlock(&ctx->read_lock);
    return ret;
}

static fas_poll_t fas_poll(struct file *file, poll_table *wait) {
    struct fas_ctx *ctx = file->private_data;

    poll_wait(file, &ctx->wq, wait);
    return fas_ring_head_ready(&ctx->ring) ? FAS_POLLIN : 0;
}

static const struct file_operations fas_fops = {
    .owner = THIS_MODULE,
    .open = fas_open,
    .release = fas_release,
    .llseek = no_llseek,
    .read = fas_read,
    .poll = fas_poll,
    .unlocked_ioctl = fas_ioctl,
};

static struct miscdevice fas_miscdev = {
    .minor = MISC_DYNAMIC_MINOR,
    .name = "encore_fas",
    .fops = &fas_fops,
    .mode = 0600,
};

static int __init fas_init(void) {
    int ret;

    BUILD_BUG_ON(sizeof(struct fas_event) != 64);
    BUILD_BUG_ON(offsetof(struct fas_event, type) != 56);
    BUILD_BUG_ON(sizeof(struct fas_stash_slot) != 32);
    BUILD_BUG_ON(FAS_RING_RECORDS & FAS_RING_MASK);
    BUILD_BUG_ON((1u << FAS_STASH_SHIFT) != FAS_STASH_SLOTS);

    ret = misc_register(&fas_miscdev);
    if (ret)
        return ret;

    pr_info("encore_fas: version %u, branch %s, ABI %u, ring %u records\n",
            FAS_VERSION,
            FAS_BRANCH,
            FAS_ABI_VERSION,
            FAS_RING_RECORDS);
    return 0;
}

static void __exit fas_exit(void) {
    misc_deregister(&fas_miscdev);
}

module_init(fas_init);
module_exit(fas_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Encore Frame Aware Scheduling module");
