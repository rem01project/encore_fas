// SPDX-License-Identifier: GPL-2.0-only

#include <linux/jiffies.h>
#include <linux/namei.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>

#include "fas_lkm_backend.h"

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

static bool fas_tgid_alive(struct pid *pid) {
    struct task_struct *task = get_pid_task(pid, PIDTYPE_TGID);
    bool alive;

    if (!task)
        return false;
    alive = atomic_read(&task->signal->live) > 0;
    put_task_struct(task);
    return alive;
}

static void fas_unregister(struct fas_ctx *ctx) {
    u32 i;

    for (i = 0; i < ctx->nprobes; i++)
        fas_uprobe_unregister(d_inode(ctx->path.dentry), &ctx->probe[i]);
    fas_uprobe_sync();
    ctx->nprobes = 0;
    path_put(&ctx->path);
    mmdrop(ctx->mm);
    put_pid(ctx->target_pid);
    ctx->target_pid = NULL;
    ctx->registered = false;
}

static void fas_life_work(struct work_struct *work) {
    struct fas_ctx *ctx = container_of(to_delayed_work(work), struct fas_ctx, life_work);

    mutex_lock(&ctx->ctl_lock);
    if (ctx->registered) {
        if (fas_tgid_alive(ctx->target_pid)) {
            mod_delayed_work(system_wq, &ctx->life_work, HZ);
        } else {
            fas_unregister(ctx);
            WRITE_ONCE(ctx->state, FAS_STATE_TARGET_EXITED);
            wake_up_interruptible(&ctx->wq);
        }
    }
    mutex_unlock(&ctx->ctl_lock);
}

void fas_session_init(struct fas_ctx *ctx) {
    INIT_DELAYED_WORK(&ctx->life_work, fas_life_work);
}

void fas_session_refresh_lifecycle(struct fas_ctx *ctx) {
    mutex_lock(&ctx->ctl_lock);
    if (ctx->registered && !fas_tgid_alive(ctx->target_pid)) {
        fas_unregister(ctx);
        WRITE_ONCE(ctx->state, FAS_STATE_TARGET_EXITED);
        wake_up_interruptible(&ctx->wq);
    }
    mutex_unlock(&ctx->ctl_lock);
}

long fas_session_register(struct fas_ctx *ctx, void __user *uarg) {
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

    pid = get_task_pid(task, PIDTYPE_TGID);
    mm = (task->flags & PF_KTHREAD) ? NULL : fas_grab_mm(task);
    put_task_struct(task);
    if (!mm) {
        put_pid(pid);
        return -ESRCH;
    }

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

        ret = fas_uprobe_register(inode, probe, req.probes[i].kind);
        if (ret)
            goto err_probes;
        ctx->nprobes = i + 1;
    }

    if (!fas_tgid_alive(pid)) {
        ret = -ESRCH;
        goto err_probes;
    }

    ctx->target_pid = pid;
    pid = NULL;
    ctx->registered = true;
    WRITE_ONCE(ctx->state, FAS_STATE_ACTIVE);
    mod_delayed_work(system_wq, &ctx->life_work, HZ);
    mutex_unlock(&ctx->ctl_lock);
    return 0;

err_probes:
    for (i = 0; i < ctx->nprobes; i++)
        fas_uprobe_unregister(inode, &ctx->probe[i]);
    fas_uprobe_sync();
    ctx->nprobes = 0;
err_path:
    path_put(&ctx->path);
err_mm:
    mmdrop(mm);
    put_pid(pid);
    mutex_unlock(&ctx->ctl_lock);
    return ret;
}

long fas_session_status(struct fas_ctx *ctx, void __user *uarg) {
    struct fas_status status = {};

    fas_session_refresh_lifecycle(ctx);
    status.state = READ_ONCE(ctx->state);
    return copy_to_user(uarg, &status, sizeof(status)) ? -EFAULT : 0;
}

long fas_session_unregister(struct fas_ctx *ctx) {
    mutex_lock(&ctx->ctl_lock);
    if (ctx->registered)
        fas_unregister(ctx);
    WRITE_ONCE(ctx->state, FAS_STATE_UNREGISTERED);
    wake_up_interruptible(&ctx->wq);
    mutex_unlock(&ctx->ctl_lock);
    return 0;
}

void fas_session_release(struct fas_ctx *ctx) {
    cancel_delayed_work_sync(&ctx->life_work);
    mutex_lock(&ctx->ctl_lock);
    if (ctx->registered)
        fas_unregister(ctx);
    mutex_unlock(&ctx->ctl_lock);
}
