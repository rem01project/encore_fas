// SPDX-License-Identifier: BSD-2-Clause

#ifndef FAS_INTERNAL_H
#define FAS_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fas/fas.h"

#define FAS_INTERNAL __attribute__((visibility("hidden")))

struct fas;

struct fas_backend_ops {
    uint32_t id;
    int (*open)(struct fas *f, const struct fas_options *opts);
    void (*close)(struct fas *f);
    int (*attach)(struct fas *f, int pid, const char *lib_path, uint64_t dequeue_offset, uint64_t queue_offset);
    int (*detach)(struct fas *f);
    int (*fd)(const struct fas *f);
    int (*read)(struct fas *f, struct fas_event *out, size_t max);
};

struct fas {
    const struct fas_backend_ops *ops;
    void *be;
    struct fas_info info;
    struct fas_stats stats;
    bool attached;
    int record_fd;
    fas_log_fn log;
    void *log_arg;
    char *diag;
    size_t diag_size;
    size_t diag_len;
};

extern const struct fas_backend_ops fas_ops_lkm;
extern const struct fas_backend_ops fas_ops_bpf;
extern const struct fas_backend_ops fas_ops_replay;

struct fas_replay_header {
    char magic[4];
    uint32_t abi;
    uint32_t rec_size;
    uint32_t caps;
};

#define FAS_REPLAY_MAGIC "FASR"

#ifdef FAS_WITH_BPF
extern const unsigned char fas_bpf_obj_start[] FAS_INTERNAL;
extern const unsigned char fas_bpf_obj_end[] FAS_INTERNAL;
#endif

FAS_INTERNAL void fas_logf(struct fas *f, int level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#endif
