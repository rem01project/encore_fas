// SPDX-License-Identifier: BSD-2-Clause

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fas/fas.h"
#include "fas_internal.h"

static const char *const kDequeueSymbols[] = {
    "_ZN7android7Surface13dequeueBufferEPP19ANativeWindowBufferPi",
    "_ZN7android7Surface21dequeueBufferInternalEP13ANativeWindowPP19ANativeWindowBufferPi",
};

static const char *const kQueueSymbols[] = {
    "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferi",
    "_ZN7android7Surface11queueBufferEP19ANativeWindowBufferiPNS_24SurfaceQueueBufferOutputE",
    "_ZN7android7Surface19queueBufferInternalEP13ANativeWindowP19ANativeWindowBufferi",
};

static void copy_args(void *dst, size_t dst_size, const void *src) {
    uint32_t size = *(const uint32_t *)src;

    memset(dst, 0, dst_size);
    if (size == 0 || size > dst_size)
        size = (uint32_t)dst_size;
    memcpy(dst, src, size);
}

void fas_logf(struct fas *f, int level, const char *fmt, ...) {
    char msg[256];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    if (f->log)
        f->log(f->log_arg, level, msg);

    if (f->diag && f->diag_size && level <= FAS_LOG_WARN) {
        int n = snprintf(f->diag + f->diag_len, f->diag_size - f->diag_len, "%s\n", msg);

        if (n > 0)
            f->diag_len += (size_t)n < f->diag_size - f->diag_len ? (size_t)n : f->diag_size - f->diag_len - 1;
    }
}

const char *fas_backend_name(uint32_t backend) {
    switch (backend) {
        case FAS_BACKEND_AUTO:
            return "auto";
        case FAS_BACKEND_LKM:
            return "lkm";
        case FAS_BACKEND_BPF:
            return "bpf";
        case FAS_BACKEND_REPLAY:
            return "replay";
        default:
            return "unknown";
    }
}

static int record_open(struct fas *f, const char *path) {
    struct fas_replay_header hdr = {
        .magic = {'F', 'A', 'S', 'R'},
        .abi = f->info.abi,
        .rec_size = f->info.rec_size,
        .caps = f->info.caps,
    };

    f->record_fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (f->record_fd < 0)
        return -errno;
    if (write(f->record_fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) {
        int err = errno ? errno : EIO;

        close(f->record_fd);
        f->record_fd = -1;
        return -err;
    }
    return 0;
}

int fas_open(const struct fas_options *opts, fas_t **out) {
    const struct fas_backend_ops *order[3];
    struct fas_options o;
    struct fas *f;
    size_t count = 0;
    int rc = -ENODEV;

    if (!out)
        return -EINVAL;
    *out = NULL;

    if (opts)
        copy_args(&o, sizeof(o), opts);
    else
        memset(&o, 0, sizeof(o));

    if (o.diag && o.diag_size)
        o.diag[0] = '\0';

    f = calloc(1, sizeof(*f));
    if (!f)
        return -ENOMEM;
    f->record_fd = -1;
    f->log = o.log;
    f->log_arg = o.log_arg;
    f->diag = o.diag;
    f->diag_size = o.diag_size;

    switch (o.backend) {
        case FAS_BACKEND_AUTO:
            if (o.replay_path)
                order[count++] = &fas_ops_replay;
            order[count++] = &fas_ops_bpf;
            order[count++] = &fas_ops_lkm;
            break;
        case FAS_BACKEND_LKM:
            order[count++] = &fas_ops_lkm;
            break;
        case FAS_BACKEND_BPF:
            order[count++] = &fas_ops_bpf;
            break;
        case FAS_BACKEND_REPLAY:
            order[count++] = &fas_ops_replay;
            break;
        default:
            free(f);
            return -EINVAL;
    }

    for (size_t i = 0; i < count; i++) {
        rc = order[i]->open(f, &o);
        if (rc == 0) {
            f->ops = order[i];
            break;
        }
        fas_logf(f, FAS_LOG_WARN, "%s backend failed: %s", fas_backend_name(order[i]->id), strerror(-rc));
        memset(&f->info, 0, sizeof(f->info));
        f->be = NULL;
    }

    if (!f->ops) {
        free(f);
        return o.backend == FAS_BACKEND_AUTO ? -ENODEV : rc;
    }

    if (o.record_path) {
        rc = record_open(f, o.record_path);
        if (rc) {
            fas_logf(f, FAS_LOG_ERROR, "cannot open record file %s: %s", o.record_path, strerror(-rc));
            f->ops->close(f);
            free(f);
            return rc;
        }
    }

    f->diag = NULL;
    f->diag_size = 0;
    *out = f;
    return 0;
}

void fas_close(fas_t *f) {
    if (!f)
        return;
    f->ops->close(f);
    if (f->record_fd >= 0)
        close(f->record_fd);
    free(f);
}

int fas_attach(fas_t *f, const struct fas_target *target) {
    struct fas_target t;
    uint64_t dq_off, q_off;
    int rc;

    if (!f || !target)
        return -EINVAL;
    copy_args(&t, sizeof(t), target);
    if (f->attached)
        return -EBUSY;
    if (f->info.backend != FAS_BACKEND_REPLAY && t.pid <= 0)
        return -EINVAL;
    if (t.mode > FAS_MODE_Q2Q)
        return -EINVAL;

    if (!t.lib_path)
        t.lib_path = FAS_DEFAULT_LIB_PATH;
    dq_off = t.dequeue_offset;
    q_off = t.queue_offset;

    if (f->info.backend != FAS_BACKEND_REPLAY) {
        if (!q_off) {
            rc = fas_resolve_offset(t.lib_path, kQueueSymbols, sizeof(kQueueSymbols) / sizeof(kQueueSymbols[0]), &q_off);
            if (rc) {
                fas_logf(f, FAS_LOG_ERROR, "queueBuffer not found in %s", t.lib_path);
                return rc;
            }
        }
        if (t.mode == FAS_MODE_Q2Q) {
            dq_off = 0;
        } else if (!dq_off) {
            rc = fas_resolve_offset(t.lib_path, kDequeueSymbols, sizeof(kDequeueSymbols) / sizeof(kDequeueSymbols[0]), &dq_off);
            if (rc && t.mode == FAS_MODE_AUTO) {
                fas_logf(f, FAS_LOG_WARN, "dequeueBuffer not found in %s. Using Q2Q mode.", t.lib_path);
                dq_off = 0;
            } else if (rc) {
                fas_logf(f, FAS_LOG_ERROR, "dequeueBuffer not found in %s", t.lib_path);
                return rc;
            }
        }
    }

    rc = f->ops->attach(f, t.pid, t.lib_path, dq_off, q_off);
    if (rc == 0) {
        f->attached = true;
        if (f->info.backend == FAS_BACKEND_REPLAY)
            f->info.mode = t.mode;
        else
            f->info.mode = dq_off ? FAS_MODE_FULL : FAS_MODE_Q2Q;
    }
    return rc;
}

int fas_detach(fas_t *f) {
    int rc;

    if (!f)
        return -EINVAL;
    if (!f->attached)
        return 0;
    rc = f->ops->detach(f);
    if (rc == 0) {
        f->attached = false;
        f->info.mode = 0;
    }
    return rc;
}

int fas_fd(const fas_t *f) {
    return f ? f->ops->fd(f) : -EINVAL;
}

int fas_wait(fas_t *f, int timeout_ms) {
    struct pollfd pfd;
    int rc;

    if (!f)
        return -EINVAL;
    pfd.fd = f->ops->fd(f);
    pfd.events = POLLIN;
    pfd.revents = 0;
    rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0)
        return -errno;
    return rc > 0 ? 1 : 0;
}

int fas_read(fas_t *f, struct fas_event *out, size_t max) {
    int n;

    if (!f || (!out && max))
        return -EINVAL;
    if (max == 0)
        return 0;

    n = f->ops->read(f, out, max);
    if (n <= 0)
        return n;

    f->stats.events += (uint64_t)n;
    for (int i = 0; i < n; i++) {
        if (out[i].type == FAS_EVENT_LOST) {
            f->stats.lost_records++;
            f->stats.lost_events += (uint32_t)out[i].arg;
        }
    }

    if (f->record_fd >= 0) {
        size_t bytes = (size_t)n * sizeof(out[0]);

        if (write(f->record_fd, out, bytes) != (ssize_t)bytes) {
            fas_logf(f, FAS_LOG_ERROR, "record write failed: %s. Recording stops.", strerror(errno));
            close(f->record_fd);
            f->record_fd = -1;
        }
    }
    return n;
}

void fas_get_info(const fas_t *f, struct fas_info *info) {
    if (f && info)
        *info = f->info;
}

void fas_get_stats(const fas_t *f, struct fas_stats *stats) {
    if (f && stats)
        *stats = f->stats;
}
