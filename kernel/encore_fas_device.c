// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>

#include "fas_lkm_backend.h"

#ifndef FAS_VERSION
    #define FAS_VERSION 0
#endif

#if LINUX_VERSION_CODE < KERNEL_VERSION(4, 16, 0)
typedef unsigned int fas_poll_t;
    #define FAS_POLLIN (POLLIN | POLLRDNORM)
    #define FAS_POLLHUP EPOLLHUP
#else
typedef __poll_t fas_poll_t;
    #define FAS_POLLIN ((__force __poll_t)(EPOLLIN | EPOLLRDNORM))
    #define FAS_POLLHUP ((__force __poll_t)EPOLLHUP)
#endif

static bool fas_is_root(void) {
    return uid_eq(current_euid(), GLOBAL_ROOT_UID);
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
    fas_session_init(ctx);
    atomic_set(&ctx->lost, 0);
    file->private_data = ctx;
    return nonseekable_open(inode, file);
}

static int fas_release(struct inode *inode, struct file *file) {
    struct fas_ctx *ctx = file->private_data;

    fas_session_release(ctx);
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
            return fas_session_register(ctx, uarg);
        case FAS_IOC_GET_STATUS:
            return fas_session_status(ctx, uarg);
        case FAS_IOC_UNREGISTER:
            return fas_session_unregister(ctx);
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
        if (READ_ONCE(ctx->state) != FAS_STATE_ACTIVE) {
            ret = 0;
            goto out;
        }
        if (file->f_flags & O_NONBLOCK) {
            ret = -EAGAIN;
            goto out;
        }
        if (wait_event_interruptible(ctx->wq,
                                     fas_ring_head_ready(ring) ||
                                         READ_ONCE(ctx->state) != FAS_STATE_ACTIVE)) {
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
    fas_poll_t mask = 0;

    poll_wait(file, &ctx->wq, wait);
    fas_session_refresh_lifecycle(ctx);
    if (fas_ring_head_ready(&ctx->ring))
        mask |= FAS_POLLIN;
    if (READ_ONCE(ctx->state) != FAS_STATE_ACTIVE)
        mask |= FAS_POLLHUP;
    return mask;
}

const struct file_operations fas_device_fops = {
    .owner = THIS_MODULE,
    .open = fas_open,
    .release = fas_release,
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 12, 0)
    .llseek = no_llseek,
#endif
    .read = fas_read,
    .poll = fas_poll,
    .unlocked_ioctl = fas_ioctl,
};
