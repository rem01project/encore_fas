/* SPDX-License-Identifier: (GPL-2.0-only WITH Linux-syscall-note) OR BSD-2-Clause */

#ifndef _UAPI_ENCORE_FAS_H
#define _UAPI_ENCORE_FAS_H

#include <linux/types.h>
#ifndef __bpf__
    #include <linux/ioctl.h>
#endif

#define FAS_ABI_VERSION 5

#define FAS_MAX_PATH_LEN 256
#define FAS_MAX_PROBES 4

#define FAS_ASSERT_SIZE(type, size)                                                 \
    typedef char fas_assert_##type##_size[(sizeof(struct type) == (size)) ? 1 : -1] \
        __attribute__((unused))

#define FAS_CAP_SCHED_COUNTERS (1u << 0)
#define FAS_CAP_BACKEND_LKM (1u << 16)
#define FAS_CAP_BACKEND_BPF (1u << 17)

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

struct fas_event {
    __u64 ts_ns;
    __u64 surface;
    __u64 buffer;
    __u64 exec_ns;
    __u64 delay_ns;
    __s32 tid;
    __s32 arg;
    __u64 reserved0;
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

#ifndef __bpf__

    #define FAS_IOC_MAGIC 'F'

    #define FAS_IOC_GET_VERSION _IOR(FAS_IOC_MAGIC, 0, struct fas_version)
    #define FAS_IOC_REGISTER _IOW(FAS_IOC_MAGIC, 1, struct fas_register_args)

    #define FAS_IOC_MAXNR 1

#endif

#define FAS_BPF_MAP_EVENTS "fas_events"
#define FAS_BPF_MAP_CFG "fas_cfg"
#define FAS_BPF_MAP_STASH "fas_stash"
#define FAS_BPF_MAP_PCPU "fas_pcpu"

#define FAS_BPF_PROG_DEQUEUE_ENTER "fas_dequeue_enter"
#define FAS_BPF_PROG_DEQUEUE_RET "fas_dequeue_ret"
#define FAS_BPF_PROG_QUEUE_ENTER "fas_queue_enter"

#define FAS_BPF_RINGBUF_BYTES (256u * 1024u)
#define FAS_BPF_STASH_ENTRIES 1024u

struct fas_bpf_cfg {
    __u32 abi;
    __u32 off_sum_exec_runtime;
    __u32 off_run_delay;
    __u32 reserved;
};
FAS_ASSERT_SIZE(fas_bpf_cfg, 16);

struct fas_bpf_stash {
    __u64 surface;
    __u64 out_buffer_ptr;
};
FAS_ASSERT_SIZE(fas_bpf_stash, 16);

struct fas_bpf_pcpu {
    __u32 pending_lost;
    __u32 reserved;
};
FAS_ASSERT_SIZE(fas_bpf_pcpu, 8);

#endif
