// SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note

#ifndef _UAPI_ENCORE_FAS_H
#define _UAPI_ENCORE_FAS_H

#include <linux/types.h>
#include <linux/ioctl.h>

#define FAS_ABI_VERSION 6

#define FAS_MAX_PATH_LEN 256
#define FAS_MAX_PROBES 4

#define FAS_ASSERT_SIZE(type, size)                                                 \
    typedef char fas_assert_##type##_size[(sizeof(struct type) == (size)) ? 1 : -1] \
        __attribute__((unused))

#define FAS_CAP_SCHED_COUNTERS (1u << 0)
#define FAS_CAP_BACKEND_LKM (1u << 16)

#define FAS_EVF_NO_SCHED (1u << 0)

enum fas_event_type {
    FAS_EVENT_NONE = 0,
    FAS_EVENT_DEQUEUE_ENTER = 1,
    FAS_EVENT_DEQUEUE_RET = 2,
    FAS_EVENT_QUEUE_ENTER = 3,
    FAS_EVENT_LOST = 4,
};

enum fas_probe_kind {
    FAS_PROBE_NONE = 0,
    FAS_PROBE_DEQUEUE = 1,
    FAS_PROBE_QUEUE = 2,
};

enum fas_state {
    FAS_STATE_UNREGISTERED = 0,
    FAS_STATE_ACTIVE = 1,
    FAS_STATE_TARGET_EXITED = 2,
};

struct fas_event {
    __u64 ts_ns;
    __u64 surface;
    __u64 buffer;
    __u64 exec_ns;
    __u64 delay_ns;
    __s32 tid;
    __s32 arg;
    __u16 ctx_vol;
    __u16 ctx_invol;
    __u32 reserved0;
    __u16 type;
    __u8 cpu;
    __u8 flags;
    __u32 reserved1;
};
FAS_ASSERT_SIZE(fas_event, 64);

struct fas_version {
    __u32 version;
    __u32 abi;
    __u32 caps;
    __u32 rec_size;
    __u32 queue_records;
    __u32 reserved[3];
};
FAS_ASSERT_SIZE(fas_version, 32);

struct fas_status {
    __u32 state;
    __u32 reserved[3];
};
FAS_ASSERT_SIZE(fas_status, 16);

struct fas_probe_spec {
    __u32 kind;
    __u32 reserved;
    __u64 offset;
};
FAS_ASSERT_SIZE(fas_probe_spec, 16);

struct fas_register_args {
    __s32 pid;
    __u32 probe_count;
    __u32 reserved[2];
    struct fas_probe_spec probes[FAS_MAX_PROBES];
    char path[FAS_MAX_PATH_LEN];
};
FAS_ASSERT_SIZE(fas_register_args, 336);

#define FAS_IOC_MAGIC 'F'

#define FAS_IOC_GET_VERSION _IOR(FAS_IOC_MAGIC, 0, struct fas_version)
#define FAS_IOC_REGISTER _IOW(FAS_IOC_MAGIC, 1, struct fas_register_args)
#define FAS_IOC_GET_STATUS _IOR(FAS_IOC_MAGIC, 2, struct fas_status)
#define FAS_IOC_UNREGISTER _IO(FAS_IOC_MAGIC, 3)

#define FAS_IOC_MAXNR 3

#endif
