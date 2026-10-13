// SPDX-License-Identifier: BSD-2-Clause

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fas/fas.h"
#include "fas/fas_frame.h"

static volatile sig_atomic_t g_stop;

/**
 * Sets the stop flag.
 *
 * @param sig Signal number.
 */
static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

/**
 * Prints one frame.
 *
 * @param fr The frame.
 */
static void print_frame(const struct fas_frame *fr) {
    printf("tid=%d", fr->tid);
    if (fr->flags & FAS_FRAME_HAS_Q2Q)
        printf(" q2q=%.2fms", fr->q2q_ns / 1e6);
    if (fr->flags & FAS_FRAME_HAS_DEQUEUE)
        printf(" dq_wait=%.2fms hold=%.2fms", fr->dequeue_wait_ns / 1e6, fr->hold_ns / 1e6);
    if (fr->flags & FAS_FRAME_HAS_SCHED)
        printf(" exec=%.2fms delay=%.2fms", fr->exec_delta_ns / 1e6, fr->delay_delta_ns / 1e6);
    if (fr->flags & FAS_FRAME_RESYNC)
        printf(" resync(lost=%u)", fr->lost_events);
    printf("\n");
}

int main(int argc, char **argv) {
    struct fas_options opts;
    struct fas_target target;
    struct fas_info info;
    struct fas_event ev[64];
    struct fas_frame fr;
    char diag[512] = "";
    const char *lib_path = FAS_DEFAULT_LIB_PATH;
    const char *replay_path = NULL;
    uint32_t mode = FAS_MODE_AUTO;
    fas_frame_ctx_t *an;
    fas_t *f;
    int opt, rc;

    while ((opt = getopt(argc, argv, "ql:r:")) != -1) {
        switch (opt) {
            case 'q':
                mode = FAS_MODE_Q2Q;
                break;
            case 'l':
                lib_path = optarg;
                break;
            case 'r':
                replay_path = optarg;
                break;
            default:
                return 2;
        }
    }
    if (optind >= argc && !replay_path) {
        fprintf(stderr, "Usage: %s [-q] [-l lib_path] [-r replay_file] <pid>\n", argv[0]);
        return 2;
    }

    memset(&opts, 0, sizeof(opts));
    opts.size = sizeof(opts);
    opts.diag = diag;
    opts.diag_size = sizeof(diag);
    if (replay_path) {
        opts.backend = FAS_BACKEND_REPLAY;
        opts.replay_path = replay_path;
    }

    rc = fas_open(&opts, &f);
    if (rc < 0) {
        fprintf(stderr, "fas_open failed: %s\n%s", strerror(-rc), diag);
        return 1;
    }

    memset(&target, 0, sizeof(target));
    target.size = sizeof(target);
    target.pid = optind < argc ? atoi(argv[optind]) : 0;
    target.lib_path = lib_path;
    target.mode = mode;
    rc = fas_attach(f, &target);
    if (rc < 0) {
        fprintf(stderr, "fas_attach failed: %s\n", strerror(-rc));
        fas_close(f);
        return 1;
    }

    fas_get_info(f, &info);
    fprintf(stderr, "backend=%s mode=%s\n", fas_backend_name(info.backend), info.mode == FAS_MODE_Q2Q || mode == FAS_MODE_Q2Q ? "q2q" : "full");

    an = fas_frame_create();
    if (!an) {
        fas_close(f);
        return 1;
    }
    fas_frame_set_mode(an, info.mode ? info.mode : mode);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    while (!g_stop) {
        int n = fas_wait(f, 500);

        if (n == -EINTR || n == 0)
            continue;
        if (n < 0)
            break;
        n = fas_read(f, ev, sizeof(ev) / sizeof(ev[0]));
        if (n == -ENODATA)
            break;
        for (int i = 0; i < n; i++) {
            if (fas_frame_feed(an, &ev[i], &fr) == 1)
                print_frame(&fr);
        }
    }

    fas_frame_destroy(an);
    fas_close(f);
    return 0;
}
