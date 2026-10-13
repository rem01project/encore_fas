// SPDX-License-Identifier: BSD-2-Clause

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fas_internal.h"

struct replay_be {
    int fd;
};

static int replay_open(struct fas *f, const struct fas_options *opts) {
    struct fas_replay_header hdr;
    struct replay_be *b;
    int fd;

    if (!opts->replay_path) {
        fas_logf(f, FAS_LOG_WARN, "replay: replay_path is not set");
        return -ENOENT;
    }

    fd = open(opts->replay_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        int err = errno;

        fas_logf(f, FAS_LOG_WARN, "replay: cannot open %s: %s", opts->replay_path, strerror(err));
        return -err;
    }
    if (read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) || memcmp(hdr.magic, FAS_REPLAY_MAGIC, 4) != 0) {
        fas_logf(f, FAS_LOG_WARN, "replay: %s is not a replay file", opts->replay_path);
        close(fd);
        return -EPROTO;
    }
    if (hdr.abi != FAS_ABI_VERSION || hdr.rec_size != sizeof(struct fas_event)) {
        fas_logf(f, FAS_LOG_WARN, "replay: ABI mismatch. File ABI %u, record size %u. Library ABI %u, record size %zu.", hdr.abi, hdr.rec_size, FAS_ABI_VERSION, sizeof(struct fas_event));
        close(fd);
        return -EPROTO;
    }

    b = malloc(sizeof(*b));
    if (!b) {
        close(fd);
        return -ENOMEM;
    }
    b->fd = fd;
    f->be = b;
    f->info.backend = FAS_BACKEND_REPLAY;
    f->info.abi = hdr.abi;
    f->info.caps = hdr.caps;
    f->info.rec_size = hdr.rec_size;
    return 0;
}

static int replay_fd(const struct fas *f) {
    return ((const struct replay_be *)f->be)->fd;
}

static void replay_close(struct fas *f) {
    close(replay_fd(f));
    free(f->be);
    f->be = NULL;
}

static int replay_attach(struct fas *f, int pid, const char *lib_path, uint64_t dequeue_offset, uint64_t queue_offset) {
    return 0;
}

static int replay_detach(struct fas *f) {
    return 0;
}

static int replay_read(struct fas *f, struct fas_event *out, size_t max) {
    ssize_t bytes = read(replay_fd(f), out, max * sizeof(*out));

    if (bytes < 0)
        return errno == EINTR ? 0 : -errno;
    if (bytes == 0)
        return -ENODATA;
    return (int)((size_t)bytes / sizeof(*out));
}

const struct fas_backend_ops fas_ops_replay = {
    .id = FAS_BACKEND_REPLAY,
    .open = replay_open,
    .close = replay_close,
    .attach = replay_attach,
    .detach = replay_detach,
    .fd = replay_fd,
    .read = replay_read,
};
