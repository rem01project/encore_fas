// SPDX-License-Identifier: BSD-2-Clause

#include "ringbuf.h"

#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define RB_BUSY_BIT (1u << 31)
#define RB_DISCARD_BIT (1u << 30)
#define RB_HDR_SIZE 8u

int fas_ringbuf_map(struct fas_ringbuf *rb, int map_fd, uint32_t size) {
    long page = sysconf(_SC_PAGESIZE);
    void *cons, *prod;

    if (page <= 0 || size == 0 || (size & (size - 1)))
        return -EINVAL;

    memset(rb, 0, sizeof(*rb));
    cons = mmap(NULL, (size_t)page, PROT_READ | PROT_WRITE, MAP_SHARED, map_fd, 0);
    if (cons == MAP_FAILED)
        return -errno;

    prod = mmap(NULL, (size_t)page + 2 * (size_t)size, PROT_READ, MAP_SHARED, map_fd, page);
    if (prod == MAP_FAILED) {
        int err = errno;

        munmap(cons, (size_t)page);
        return -err;
    }

    rb->cons_map = cons;
    rb->cons_len = (size_t)page;
    rb->prod_map = prod;
    rb->prod_len = (size_t)page + 2 * (size_t)size;
    rb->consumer_pos = cons;
    rb->producer_pos = prod;
    rb->data = (const uint8_t *)prod + page;
    rb->mask = size - 1;
    return 0;
}

void fas_ringbuf_unmap(struct fas_ringbuf *rb) {
    if (rb->prod_map)
        munmap(rb->prod_map, rb->prod_len);
    if (rb->cons_map)
        munmap(rb->cons_map, rb->cons_len);
    memset(rb, 0, sizeof(*rb));
}

int fas_ringbuf_consume(struct fas_ringbuf *rb, struct fas_event *out, size_t max) {
    unsigned long start = __atomic_load_n(rb->consumer_pos, __ATOMIC_RELAXED);
    unsigned long cons = start;
    unsigned long prod = __atomic_load_n(rb->producer_pos, __ATOMIC_ACQUIRE);
    size_t n = 0;

    while (cons < prod && n < max) {
        const uint32_t *hdr = (const uint32_t *)(rb->data + (cons & rb->mask));
        uint32_t len = __atomic_load_n(hdr, __ATOMIC_ACQUIRE);
        uint32_t size;

        if (len & RB_BUSY_BIT)
            break;

        size = len & ~(RB_BUSY_BIT | RB_DISCARD_BIT);
        cons += (size + RB_HDR_SIZE + 7u) & ~7ul;

        if (len & RB_DISCARD_BIT)
            continue;
        if (size != sizeof(*out)) {
            rb->skipped++;
            continue;
        }
        memcpy(&out[n++], (const uint8_t *)hdr + RB_HDR_SIZE, sizeof(*out));
    }

    if (cons != start)
        __atomic_store_n(rb->consumer_pos, cons, __ATOMIC_RELEASE);
    return (int)n;
}
