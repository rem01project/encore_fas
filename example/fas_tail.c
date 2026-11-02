// SPDX-License-Identifier: BSD-2-Clause

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fas/fas.h"
#include "fas/fas_frame.h"

#define CPUFREQ_DIR "/sys/devices/system/cpu/cpufreq"
#define GPU_CLOCK_PATH "/sys/kernel/gpu/gpu_clock" // Adreno only for now
#define MAX_POLICIES 16

struct cpu_policy {
    int id;
    int fd;
};

static volatile sig_atomic_t g_stop;
static struct cpu_policy g_cpu[MAX_POLICIES];
static int g_nr_cpu;
static int g_gpu_fd = -1;

/**
 * Sets the stop flag.
 *
 * @param sig Signal number.
 */
static void on_signal(int sig) {
    (void)sig;
    g_stop = 1;
}

static int cmp_policy(const void *a, const void *b) {
    return ((const struct cpu_policy *)a)->id - ((const struct cpu_policy *)b)->id;
}

static void freq_init(void) {
    struct dirent *de;
    DIR *d = opendir(CPUFREQ_DIR);

    if (d) {
        while ((de = readdir(d)) != NULL && g_nr_cpu < MAX_POLICIES) {
            char path[512];
            int id, fd;

            if (sscanf(de->d_name, "policy%d", &id) != 1)
                continue;
            snprintf(path, sizeof(path), CPUFREQ_DIR "/%s/cpuinfo_cur_freq", de->d_name);
            fd = open(path, O_RDONLY | O_CLOEXEC);
            if (fd < 0)
                continue;
            g_cpu[g_nr_cpu].id = id;
            g_cpu[g_nr_cpu].fd = fd;
            g_nr_cpu++;
        }
        closedir(d);
        qsort(g_cpu, g_nr_cpu, sizeof(g_cpu[0]), cmp_policy);
    }

    g_gpu_fd = open(GPU_CLOCK_PATH, O_RDONLY | O_CLOEXEC);
}

static void freq_close(void) {
    for (int i = 0; i < g_nr_cpu; i++)
        close(g_cpu[i].fd);
    g_nr_cpu = 0;
    if (g_gpu_fd >= 0)
        close(g_gpu_fd);
    g_gpu_fd = -1;
}

/**
 * Reads a single unsigned integer from the start of a sysfs file.
 *
 * @return The value, or -1 on failure.
 */
static long long read_sysfs_ll(int fd) {
    char buf[32];
    ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);

    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return strtoll(buf, NULL, 10);
}

/**
 * Prints current per-policy CPU and GPU frequency (policyN=XkHz gpu=XkHz).
 */
static void print_freqs(void) {
    for (int i = 0; i < g_nr_cpu; i++) {
        long long khz = read_sysfs_ll(g_cpu[i].fd);

        if (khz < 0)
            printf(" policy%d=?", g_cpu[i].id);
        else
            printf(" policy%d=%lldkHz", g_cpu[i].id, khz);
    }
    if (g_gpu_fd >= 0) {
        long long mhz = read_sysfs_ll(g_gpu_fd);

        if (mhz < 0)
            printf(" gpu=?");
        else
            printf(" gpu=%lldkHz", mhz * 1000);
    }
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
    print_freqs();
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

    if (!replay_path)
        freq_init();

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

    freq_close();
    fas_frame_destroy(an);
    fas_close(f);
    return 0;
}
