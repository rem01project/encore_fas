// SPDX-License-Identifier: BSD-2-Clause

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "fas_internal.h"

struct lkm_be {
    int dev_fd;
    int ep_fd;
    char path[128];
};

static int lkm_open_dev(struct fas *f, struct lkm_be *b) {
    struct fas_version ver;
    int fd = open(b->path, O_RDWR | O_CLOEXEC | O_NONBLOCK);

    if (fd < 0) {
        int err = errno;

        fas_logf(f, FAS_LOG_WARN, "lkm: cannot open %s: %s", b->path, strerror(err));
        return -err;
    }

    memset(&ver, 0, sizeof(ver));
    if (ioctl(fd, FAS_IOC_GET_VERSION, &ver) != 0) {
        int err = errno;

        fas_logf(f, FAS_LOG_WARN, "lkm: version request failed: %s", strerror(err));
        close(fd);
        return -err;
    }
    if (ver.abi != FAS_ABI_VERSION || ver.rec_size != sizeof(struct fas_event)) {
        fas_logf(f, FAS_LOG_WARN, "lkm: ABI mismatch. Module ABI %u, record size %u. Library ABI %u, record size %zu. Build the module and the library from the same source.", ver.abi, ver.rec_size, FAS_ABI_VERSION, sizeof(struct fas_event));
        close(fd);
        return -EPROTO;
    }

    f->info.backend = FAS_BACKEND_LKM;
    f->info.abi = ver.abi;
    f->info.version = ver.version;
    f->info.caps = ver.caps;
    f->info.rec_size = ver.rec_size;
    f->info.ring_bytes = ver.queue_records * ver.rec_size;
    b->dev_fd = fd;
    return 0;
}

static int lkm_ep_add(struct lkm_be *b) {
    struct epoll_event ev = {.events = EPOLLIN};

    return epoll_ctl(b->ep_fd, EPOLL_CTL_ADD, b->dev_fd, &ev) ? -errno : 0;
}

static int lkm_open(struct fas *f, const struct fas_options *opts) {
    const char *path = opts->lkm_path ? opts->lkm_path : FAS_DEFAULT_LKM_PATH;
    struct lkm_be *b = calloc(1, sizeof(*b));
    int rc;

    if (!b)
        return -ENOMEM;
    b->dev_fd = -1;
    if (strlen(path) >= sizeof(b->path)) {
        free(b);
        return -ENAMETOOLONG;
    }
    strcpy(b->path, path);

    b->ep_fd = epoll_create1(EPOLL_CLOEXEC);
    if (b->ep_fd < 0) {
        rc = -errno;
        free(b);
        return rc;
    }

    rc = lkm_open_dev(f, b);
    if (rc == 0)
        rc = lkm_ep_add(b);
    if (rc) {
        if (b->dev_fd >= 0)
            close(b->dev_fd);
        close(b->ep_fd);
        free(b);
        return rc;
    }
    f->be = b;
    return 0;
}

static void lkm_close(struct fas *f) {
    struct lkm_be *b = f->be;

    close(b->dev_fd);
    close(b->ep_fd);
    free(b);
    f->be = NULL;
}

static int lkm_attach(struct fas *f, int pid, const char *lib_path, uint64_t dequeue_offset, uint64_t queue_offset) {
    struct lkm_be *b = f->be;
    struct fas_register_args args;

    if (strlen(lib_path) >= FAS_MAX_PATH_LEN)
        return -ENAMETOOLONG;

    memset(&args, 0, sizeof(args));
    args.pid = pid;
    if (dequeue_offset) {
        args.probes[args.probe_count].kind = FAS_PROBE_DEQUEUE;
        args.probes[args.probe_count++].offset = dequeue_offset;
    }
    args.probes[args.probe_count].kind = FAS_PROBE_QUEUE;
    args.probes[args.probe_count++].offset = queue_offset;
    strcpy(args.path, lib_path);

    if (ioctl(b->dev_fd, FAS_IOC_REGISTER, &args) != 0) {
        int err = errno;

        fas_logf(f, FAS_LOG_ERROR, "lkm: register failed for pid %d: %s", pid, strerror(err));
        return -err;
    }
    return 0;
}

static int lkm_detach(struct fas *f) {
    struct lkm_be *b = f->be;
    int old_fd = b->dev_fd;
    int rc;

    epoll_ctl(b->ep_fd, EPOLL_CTL_DEL, old_fd, NULL);
    close(old_fd);
    b->dev_fd = -1;

    rc = lkm_open_dev(f, b);
    if (rc == 0)
        rc = lkm_ep_add(b);
    if (rc && b->dev_fd >= 0) {
        close(b->dev_fd);
        b->dev_fd = -1;
    }
    return rc;
}

static int lkm_fd(const struct fas *f) {
    return ((const struct lkm_be *)f->be)->ep_fd;
}

static int lkm_read(struct fas *f, struct fas_event *out, size_t max) {
    struct lkm_be *b = f->be;
    ssize_t bytes;

    if (b->dev_fd < 0)
        return -ENODEV;

    bytes = read(b->dev_fd, out, max * sizeof(*out));
    if (bytes < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return 0;
        return -errno;
    }
    if (bytes % sizeof(*out))
        return -EPROTO;
    return (int)((size_t)bytes / sizeof(*out));
}

const struct fas_backend_ops fas_ops_lkm = {
    .id = FAS_BACKEND_LKM,
    .open = lkm_open,
    .close = lkm_close,
    .attach = lkm_attach,
    .detach = lkm_detach,
    .fd = lkm_fd,
    .read = lkm_read,
};
