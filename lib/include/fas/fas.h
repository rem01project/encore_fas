// SPDX-License-Identifier: BSD-2-Clause

#ifndef FAS_FAS_H
#define FAS_FAS_H

#include <stddef.h>
#include <stdint.h>

#include "uapi/encore_fas_uapi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAS_API __attribute__((visibility("default")))

#define FAS_LIB_VERSION_MAJOR 0
#define FAS_LIB_VERSION_MINOR 1

#define FAS_DEFAULT_LKM_PATH "/dev/encore_fas"
#define FAS_DEFAULT_LIB_PATH "/system/lib64/libgui.so"

typedef struct fas fas_t;

enum fas_backend {
    FAS_BACKEND_AUTO = 0,
    FAS_BACKEND_LKM = 1,
    FAS_BACKEND_BPF = 2,
    FAS_BACKEND_REPLAY = 3,
};

enum fas_mode {
    FAS_MODE_AUTO = 0,
    FAS_MODE_FULL = 1,
    FAS_MODE_Q2Q = 2,
};

enum fas_log_level {
    FAS_LOG_ERROR = 0,
    FAS_LOG_WARN = 1,
    FAS_LOG_INFO = 2,
    FAS_LOG_DEBUG = 3,
};

typedef void (*fas_log_fn)(void *arg, int level, const char *msg);

struct fas_options {
    uint32_t size;
    uint32_t backend;
    const char *lkm_path;
    const char *bpf_object_path;
    const char *replay_path;
    const char *record_path;
    uint32_t sched_off_exec;
    uint32_t sched_off_delay;
    fas_log_fn log;
    void *log_arg;
    char *diag;
    size_t diag_size;
};

struct fas_target {
    uint32_t size;
    int32_t pid;
    const char *lib_path;
    uint64_t dequeue_offset;
    uint64_t queue_offset;
    uint32_t mode;
    uint32_t reserved;
};

struct fas_info {
    uint32_t backend;
    uint32_t abi;
    uint32_t version;
    uint32_t caps;
    uint32_t rec_size;
    uint32_t ring_bytes;
    uint32_t mode;
};

struct fas_stats {
    uint64_t events;
    uint64_t lost_events;
    uint64_t lost_records;
};

FAS_API int fas_open(const struct fas_options *opts, fas_t **out);

FAS_API void fas_close(fas_t *f);

FAS_API int fas_attach(fas_t *f, const struct fas_target *target);

FAS_API int fas_detach(fas_t *f);

FAS_API int fas_fd(const fas_t *f);

FAS_API int fas_wait(fas_t *f, int timeout_ms);

FAS_API int fas_read(fas_t *f, struct fas_event *out, size_t max);

FAS_API void fas_get_info(const fas_t *f, struct fas_info *info);

FAS_API void fas_get_stats(const fas_t *f, struct fas_stats *stats);

FAS_API int fas_resolve_offset(const char *lib_path, const char *const *symbols, size_t count, uint64_t *out_offset);

FAS_API const char *fas_backend_name(uint32_t backend);

#ifdef __cplusplus
}
#endif

#endif
