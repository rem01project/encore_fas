// SPDX-License-Identifier: BSD-2-Clause

#ifndef FAS_RINGBUF_H
#define FAS_RINGBUF_H

#include <stddef.h>
#include <stdint.h>

#include "fas_internal.h"

struct fas_ringbuf {
    unsigned long *consumer_pos;
    const unsigned long *producer_pos;
    const uint8_t *data;
    unsigned long mask;
    uint64_t skipped;
    void *cons_map;
    size_t cons_len;
    void *prod_map;
    size_t prod_len;
};

FAS_INTERNAL int fas_ringbuf_map(struct fas_ringbuf *rb, int map_fd, uint32_t size);

FAS_INTERNAL void fas_ringbuf_unmap(struct fas_ringbuf *rb);

FAS_INTERNAL int fas_ringbuf_consume(struct fas_ringbuf *rb, struct fas_event *out, size_t max);

#endif
