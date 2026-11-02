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

struct freq_sample {
    long long cpu_khz[MAX_POLICIES]; // -1 if unreadable
    long long gpu_khz;               // -1 if unreadable or absent
};

/**
 * Samples current per-policy CPU and GPU frequency.
 */
static void sample_freqs(struct freq_sample *fs) {
    for (int i = 0; i < g_nr_cpu; i++)
        fs->cpu_khz[i] = read_sysfs_ll(g_cpu[i].fd);
    fs->gpu_khz = -1;
    if (g_gpu_fd >= 0) {
        long long mhz = read_sysfs_ll(g_gpu_fd);

        if (mhz >= 0)
            fs->gpu_khz = mhz * 1000;
    }
}

/**
 * Prints a frequency sample (policyN=XkHz gpu=XkHz).
 */
static void print_freqs(const struct freq_sample *fs) {
    for (int i = 0; i < g_nr_cpu; i++) {
        if (fs->cpu_khz[i] < 0)
            printf(" policy%d=?", g_cpu[i].id);
        else
            printf(" policy%d=%lldkHz", g_cpu[i].id, fs->cpu_khz[i]);
    }
    if (g_gpu_fd >= 0) {
        if (fs->gpu_khz < 0)
            printf(" gpu=?");
        else
            printf(" gpu=%lldkHz", fs->gpu_khz);
    }
}

/**
 * Prints one frame.
 *
 * @param fr The frame.
 * @param fs Frequency sample taken for this frame.
 */
static void print_frame(const struct fas_frame *fr, const struct freq_sample *fs) {
    printf("tid=%d", fr->tid);
    if (fr->flags & FAS_FRAME_HAS_Q2Q)
        printf(" q2q=%.2fms", fr->q2q_ns / 1e6);
    if (fr->flags & FAS_FRAME_HAS_DEQUEUE)
        printf(" dq_wait=%.2fms hold=%.2fms", fr->dequeue_wait_ns / 1e6, fr->hold_ns / 1e6);
    if (fr->flags & FAS_FRAME_HAS_SCHED)
        printf(" exec=%.2fms delay=%.2fms", fr->exec_delta_ns / 1e6, fr->delay_delta_ns / 1e6);
    if (fr->flags & FAS_FRAME_RESYNC)
        printf(" resync(lost=%u)", fr->lost_events);
    print_freqs(fs);
    printf("\n");
}

static void csv_header(FILE *fp) {
    fputs("ts_ns,tid,q2q_ns,dequeue_wait_ns,hold_ns,exec_delta_ns,delay_delta_ns,resync,lost_events", fp);
    for (int i = 0; i < g_nr_cpu; i++)
        fprintf(fp, ",policy%d_khz", g_cpu[i].id);
    if (g_gpu_fd >= 0)
        fputs(",gpu_khz", fp);
    fputc('\n', fp);
    fflush(fp);
}

static void csv_row(FILE *fp, const struct fas_frame *fr, const struct freq_sample *fs) {
    fprintf(fp, "%llu,%d,", (unsigned long long)fr->ts_ns, fr->tid);
    if (fr->flags & FAS_FRAME_HAS_Q2Q)
        fprintf(fp, "%llu", (unsigned long long)fr->q2q_ns);
    fputc(',', fp);
    if (fr->flags & FAS_FRAME_HAS_DEQUEUE)
        fprintf(fp, "%llu,%llu", (unsigned long long)fr->dequeue_wait_ns, (unsigned long long)fr->hold_ns);
    else
        fputc(',', fp);
    fputc(',', fp);
    if (fr->flags & FAS_FRAME_HAS_SCHED)
        fprintf(fp, "%llu,%llu", (unsigned long long)fr->exec_delta_ns, (unsigned long long)fr->delay_delta_ns);
    else
        fputc(',', fp);
    fprintf(fp, ",%d,%u", (fr->flags & FAS_FRAME_RESYNC) ? 1 : 0, fr->lost_events);
    for (int i = 0; i < g_nr_cpu; i++) {
        fputc(',', fp);
        if (fs->cpu_khz[i] >= 0)
            fprintf(fp, "%lld", fs->cpu_khz[i]);
    }
    if (g_gpu_fd >= 0) {
        fputc(',', fp);
        if (fs->gpu_khz >= 0)
            fprintf(fp, "%lld", fs->gpu_khz);
    }
    fputc('\n', fp);
    fflush(fp);
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s -b <lkm|ebpf> [-q] [-l lib_path] [-c csv_file] <pid>\n", prog);
}

int main(int argc, char **argv) {
    struct fas_options opts;
    struct fas_target target;
    struct fas_info info;
    struct fas_event ev[64];
    struct fas_frame fr;
    char diag[512] = "";
    const char *lib_path = FAS_DEFAULT_LIB_PATH;
    const char *csv_path = NULL;
    FILE *csv = NULL;
    uint32_t mode = FAS_MODE_AUTO;
    uint32_t backend = FAS_BACKEND_AUTO;
    int have_backend = 0;
    fas_frame_ctx_t *an;
    fas_t *f;
    int opt, rc, ret = 0;

    while ((opt = getopt(argc, argv, "b:c:ql:")) != -1) {
        switch (opt) {
            case 'b':
                if (!strcmp(optarg, "lkm"))
                    backend = FAS_BACKEND_LKM;
                else if (!strcmp(optarg, "ebpf") || !strcmp(optarg, "bpf"))
                    backend = FAS_BACKEND_BPF;
                else {
                    fprintf(stderr, "Invalid backend '%s' (expected lkm or ebpf)\n", optarg);
                    return 2;
                }
                have_backend = 1;
                break;
            case 'c':
                csv_path = optarg;
                break;
            case 'q':
                mode = FAS_MODE_Q2Q;
                break;
            case 'l':
                lib_path = optarg;
                break;
            default:
                usage(argv[0]);
                return 2;
        }
    }
    if (!have_backend) {
        fprintf(stderr, "Backend must be chosen explicitly: -b lkm or -b ebpf\n");
        usage(argv[0]);
        return 2;
    }
    if (optind >= argc) {
        usage(argv[0]);
        return 2;
    }

    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) {
            fprintf(stderr, "cannot open %s: %s\n", csv_path, strerror(errno));
            return 1;
        }
    }

    memset(&opts, 0, sizeof(opts));
    opts.size = sizeof(opts);
    opts.diag = diag;
    opts.diag_size = sizeof(diag);
    opts.backend = backend;

    rc = fas_open(&opts, &f);
    if (rc < 0) {
        fprintf(stderr, "fas_open failed: %s\n%s", strerror(-rc), diag);
        if (csv)
            fclose(csv);
        return 1;
    }

    memset(&target, 0, sizeof(target));
    target.size = sizeof(target);
    target.pid = atoi(argv[optind]);
    target.lib_path = lib_path;
    target.mode = mode;
    rc = fas_attach(f, &target);
    if (rc < 0) {
        fprintf(stderr, "fas_attach failed: %s\n", strerror(-rc));
        fas_close(f);
        if (csv)
            fclose(csv);
        return 1;
    }

    fas_get_info(f, &info);
    fprintf(stderr, "backend=%s mode=%s\n", fas_backend_name(info.backend), info.mode == FAS_MODE_Q2Q || mode == FAS_MODE_Q2Q ? "q2q" : "full");
    if (csv)
        fprintf(stderr, "recording csv to %s\n", csv_path);

    an = fas_frame_create();
    if (!an) {
        fas_close(f);
        if (csv)
            fclose(csv);
        return 1;
    }
    fas_frame_set_mode(an, info.mode ? info.mode : mode);

    freq_init();
    if (csv)
        csv_header(csv);

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
            if (fas_frame_feed(an, &ev[i], &fr) == 1) {
                struct freq_sample fs;

                sample_freqs(&fs);
                print_frame(&fr, &fs);
                if (csv)
                    csv_row(csv, &fr, &fs);
            }
        }
    }

    freq_close();
    fas_frame_destroy(an);
    fas_close(f);
    if (csv && fclose(csv) != 0) {
        fprintf(stderr, "error writing %s: %s\n", csv_path, strerror(errno));
        ret = 1;
    }
    return ret;
}
