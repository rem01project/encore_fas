// SPDX-License-Identifier: BSD-2-Clause

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fas_internal.h"

#ifdef FAS_WITH_BPF

    #include <bpf/bpf.h>
    #include <bpf/btf.h>
    #include <bpf/libbpf.h>
    #include <stdarg.h>
    #include <stdio.h>

    #include "ringbuf.h"

    #define PROBE_COUNT 3
    #define PROBE_QUEUE 2

struct bpf_be {
    struct bpf_object *obj;
    struct bpf_program *progs[PROBE_COUNT];
    struct bpf_link *links[PROBE_COUNT];
    int events_fd;
    struct fas_ringbuf rb;
};

static __thread struct fas *tls_owner;

static int print_cb(enum libbpf_print_level level, const char *fmt, va_list ap) {
    char msg[512];
    size_t len;

    if (!tls_owner)
        return 0;
    vsnprintf(msg, sizeof(msg), fmt, ap);
    len = strlen(msg);
    while (len && msg[len - 1] == '\n')
        msg[--len] = '\0';
    fas_logf(tls_owner, level == LIBBPF_WARN ? FAS_LOG_INFO : FAS_LOG_DEBUG, "libbpf: %s", msg);
    return 0;
}

static int member_off(const struct btf *btf, int type_id, const char *name, uint32_t base, uint32_t *off, int *type) {
    const struct btf_type *t;
    const struct btf_member *m;

    type_id = btf__resolve_type(btf, (__u32)type_id);
    if (type_id < 0)
        return type_id;
    t = btf__type_by_id(btf, (__u32)type_id);
    if (!t || !(btf_is_struct(t) || btf_is_union(t)))
        return -EINVAL;

    m = btf_members(t);
    for (int i = 0; i < btf_vlen(t); i++, m++) {
        const char *mname = btf__name_by_offset(btf, m->name_off);
        uint32_t bits = btf_member_bit_offset(t, (__u32)i);

        if (mname && *mname) {
            if (strcmp(mname, name) != 0)
                continue;
            if (bits % 8)
                return -EINVAL;
            *off = base + bits / 8;
            *type = (int)m->type;
            return 0;
        }
        if (bits % 8 == 0 && member_off(btf, (int)m->type, name, base + bits / 8, off, type) == 0)
            return 0;
    }
    return -ENOENT;
}

static int field_off(const struct btf *btf, const char *outer, const char *mid, const char *leaf, uint32_t *out) {
    uint32_t mid_off, leaf_off;
    int outer_id = btf__find_by_name_kind(btf, outer, BTF_KIND_STRUCT);
    int mid_type, leaf_type, rc;

    if (outer_id < 0)
        return outer_id;
    rc = member_off(btf, outer_id, mid, 0, &mid_off, &mid_type);
    if (rc == 0)
        rc = member_off(btf, mid_type, leaf, 0, &leaf_off, &leaf_type);
    if (rc)
        return rc;
    if (btf__resolve_size(btf, (__u32)leaf_type) != 8)
        return -EINVAL;
    *out = mid_off + leaf_off;
    return 0;
}

static void find_sched_offsets(struct fas *f, const struct fas_options *opts, struct fas_bpf_cfg *cfg) {
    struct btf *btf;
    int rc;

    if (opts->sched_off_exec && opts->sched_off_delay) {
        cfg->off_sum_exec_runtime = opts->sched_off_exec;
        cfg->off_run_delay = opts->sched_off_delay;
        return;
    }

    btf = btf__load_vmlinux_btf();
    rc = (int)libbpf_get_error(btf);
    if (rc) {
        fas_logf(f, FAS_LOG_INFO, "bpf: no kernel BTF (%s). Events have no scheduler counters.", strerror(-rc));
        return;
    }

    if (field_off(btf, "task_struct", "se", "sum_exec_runtime", &cfg->off_sum_exec_runtime) || field_off(btf, "task_struct", "sched_info", "run_delay", &cfg->off_run_delay)) {
        fas_logf(f, FAS_LOG_INFO, "bpf: BTF has no scheduler counters. Events have no scheduler counters.");
        cfg->off_sum_exec_runtime = 0;
        cfg->off_run_delay = 0;
    }
    btf__free(btf);
}

static int load_object(struct fas *f, struct bpf_be *b, const struct fas_options *opts) {
    static const char *const prog_names[PROBE_COUNT] = {FAS_BPF_PROG_DEQUEUE_ENTER, FAS_BPF_PROG_DEQUEUE_RET, FAS_BPF_PROG_QUEUE_ENTER};
    struct fas_bpf_cfg cfg;
    struct bpf_map *cfg_map, *events_map;
    uint32_t key = 0;
    int rc;

    b->obj = opts->bpf_object_path ? bpf_object__open_file(opts->bpf_object_path, NULL) : bpf_object__open_mem(fas_bpf_obj_start, (size_t)(fas_bpf_obj_end - fas_bpf_obj_start), NULL);
    rc = (int)libbpf_get_error(b->obj);
    if (rc) {
        b->obj = NULL;
        fas_logf(f, FAS_LOG_WARN, "bpf: cannot open object: %s", strerror(-rc));
        return rc;
    }

    rc = bpf_object__load(b->obj);
    if (rc) {
        fas_logf(f, FAS_LOG_WARN, "bpf: cannot load object: %s. The kernel needs BPF, ring buffer and uprobe support. The process needs root.", strerror(-rc));
        return rc;
    }

    cfg_map = bpf_object__find_map_by_name(b->obj, FAS_BPF_MAP_CFG);
    events_map = bpf_object__find_map_by_name(b->obj, FAS_BPF_MAP_EVENTS);
    if (!cfg_map || !events_map) {
        fas_logf(f, FAS_LOG_WARN, "bpf: object has no map %s or %s", FAS_BPF_MAP_CFG, FAS_BPF_MAP_EVENTS);
        return -EPROTO;
    }
    for (int i = 0; i < PROBE_COUNT; i++) {
        b->progs[i] = bpf_object__find_program_by_name(b->obj, prog_names[i]);
        if (!b->progs[i]) {
            fas_logf(f, FAS_LOG_WARN, "bpf: object has no program %s", prog_names[i]);
            return -EPROTO;
        }
    }

    memset(&cfg, 0, sizeof(cfg));
    cfg.abi = FAS_ABI_VERSION;
    find_sched_offsets(f, opts, &cfg);
    rc = bpf_map_update_elem(bpf_map__fd(cfg_map), &key, &cfg, 0);
    if (rc) {
        rc = -errno;
        fas_logf(f, FAS_LOG_WARN, "bpf: cannot write the config map: %s", strerror(-rc));
        return rc;
    }

    b->events_fd = bpf_map__fd(events_map);
    rc = fas_ringbuf_map(&b->rb, b->events_fd, bpf_map__max_entries(events_map));
    if (rc) {
        fas_logf(f, FAS_LOG_WARN, "bpf: cannot map the ring buffer: %s", strerror(-rc));
        return rc;
    }

    f->info.backend = FAS_BACKEND_BPF;
    f->info.abi = FAS_ABI_VERSION;
    f->info.caps = FAS_CAP_BACKEND_BPF | (cfg.off_sum_exec_runtime ? FAS_CAP_SCHED_COUNTERS : 0);
    f->info.rec_size = sizeof(struct fas_event);
    f->info.ring_bytes = bpf_map__max_entries(events_map);
    return 0;
}

static void bpf_unlink(struct bpf_be *b) {
    for (int i = 0; i < PROBE_COUNT; i++) {
        if (b->links[i])
            bpf_link__destroy(b->links[i]);
        b->links[i] = NULL;
    }
}

static void bpf_free(struct bpf_be *b) {
    bpf_unlink(b);
    fas_ringbuf_unmap(&b->rb);
    if (b->obj)
        bpf_object__close(b->obj);
    free(b);
}

static int bpf_open(struct fas *f, const struct fas_options *opts) {
    libbpf_print_fn_t prev;
    struct bpf_be *b;
    int rc;

    b = calloc(1, sizeof(*b));
    if (!b)
        return -ENOMEM;
    b->events_fd = -1;

    tls_owner = f;
    prev = libbpf_set_print(print_cb);
    rc = load_object(f, b, opts);
    libbpf_set_print(prev);
    tls_owner = NULL;

    if (rc) {
        bpf_free(b);
        return rc;
    }
    f->be = b;
    return 0;
}

static void bpf_close(struct fas *f) {
    bpf_free(f->be);
    f->be = NULL;
}

static int bpf_attach(struct fas *f, int pid, const char *lib_path, uint64_t dequeue_offset, uint64_t queue_offset) {
    static const bool is_ret[PROBE_COUNT] = {false, true, false};
    const uint64_t offsets[PROBE_COUNT] = {dequeue_offset, dequeue_offset, queue_offset};
    struct bpf_be *b = f->be;
    libbpf_print_fn_t prev;
    int rc = 0;

    tls_owner = f;
    prev = libbpf_set_print(print_cb);
    for (int i = 0; i < PROBE_COUNT && rc == 0; i++) {
        if (i != PROBE_QUEUE && !dequeue_offset)
            continue;
        LIBBPF_OPTS(bpf_uprobe_opts, uo, .retprobe = is_ret[i]);
        struct bpf_link *link = bpf_program__attach_uprobe_opts(b->progs[i], pid, lib_path, offsets[i], &uo);

        rc = (int)libbpf_get_error(link);
        if (rc == 0)
            b->links[i] = link;
        else
            fas_logf(f, FAS_LOG_ERROR, "bpf: cannot attach probe %d to pid %d: %s", i, pid, strerror(-rc));
    }
    libbpf_set_print(prev);
    tls_owner = NULL;

    if (rc)
        bpf_unlink(b);
    return rc;
}

static int bpf_detach(struct fas *f) {
    bpf_unlink(f->be);
    return 0;
}

static int bpf_fd(const struct fas *f) {
    return ((const struct bpf_be *)f->be)->events_fd;
}

static int bpf_read(struct fas *f, struct fas_event *out, size_t max) {
    return fas_ringbuf_consume(&((struct bpf_be *)f->be)->rb, out, max);
}

const struct fas_backend_ops fas_ops_bpf = {
    .id = FAS_BACKEND_BPF,
    .open = bpf_open,
    .close = bpf_close,
    .attach = bpf_attach,
    .detach = bpf_detach,
    .fd = bpf_fd,
    .read = bpf_read,
};

#else

static int bpf_open(struct fas *f, const struct fas_options *opts) {
    fas_logf(f, FAS_LOG_WARN, "bpf: this build of libfas has no libbpf support");
    return -ENOSYS;
}

static void bpf_close(struct fas *f) {
}

static int bpf_attach(struct fas *f, int pid, const char *lib_path, uint64_t dequeue_offset, uint64_t queue_offset) {
    return -ENOSYS;
}

static int bpf_detach(struct fas *f) {
    return -ENOSYS;
}

static int bpf_fd(const struct fas *f) {
    return -1;
}

static int bpf_read(struct fas *f, struct fas_event *out, size_t max) {
    return -ENOSYS;
}

const struct fas_backend_ops fas_ops_bpf = {
    .id = FAS_BACKEND_BPF,
    .open = bpf_open,
    .close = bpf_close,
    .attach = bpf_attach,
    .detach = bpf_detach,
    .fd = bpf_fd,
    .read = bpf_read,
};

#endif
