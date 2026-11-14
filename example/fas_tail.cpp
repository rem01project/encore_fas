// SPDX-License-Identifier: BSD-2-Clause

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <vector>

#include "fas/fas.h"
#include "fas/fas_frame.h"
#include "fas_guard.hpp"
#include "fas_sched.hpp"
#include "fas_sysfs.hpp"

namespace {

/**
 * @return CLOCK_MONOTONIC time in nanoseconds.
 */
uint64_t mono_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

struct FreqSample {
    std::vector<long long> cpu_khz;
    long long gpu_khz = -1;
};

/**
 * Samples current per-policy CPU and GPU frequency.
 */
void sample_freqs(fsched::SysfsActuator &hw, FreqSample &fs) {
    fs.cpu_khz.resize(hw.policy_count());
    for (size_t i = 0; i < fs.cpu_khz.size(); i++)
        fs.cpu_khz[i] = hw.sample_cpu_khz(i);
    fs.gpu_khz = hw.has_gpu_sample() ? hw.sample_gpu_khz() : -1;
}

/**
 * Prints one frame.
 *
 * @param fr The frame.
 * @param fs Frequency sample taken for this frame.
 * @param hw Hardware, used for policy ids.
 */
void print_frame(const fas_frame &fr, const FreqSample &fs, fsched::SysfsActuator &hw) {
    printf("tid=%d", fr.tid);
    if (fr.flags & FAS_FRAME_HAS_Q2Q)
        printf(" q2q=%.2fms", fr.q2q_ns / 1e6);
    if (fr.flags & FAS_FRAME_HAS_DEQUEUE)
        printf(" dq_wait=%.2fms hold=%.2fms", fr.dequeue_wait_ns / 1e6, fr.hold_ns / 1e6);
    if (fr.flags & FAS_FRAME_HAS_SCHED)
        printf(" exec=%.2fms delay=%.2fms", fr.exec_delta_ns / 1e6, fr.delay_delta_ns / 1e6);
    if (fr.flags & FAS_FRAME_RESYNC)
        printf(" resync(lost=%u)", fr.lost_events);
    for (size_t i = 0; i < fs.cpu_khz.size(); i++) {
        if (fs.cpu_khz[i] < 0)
            printf(" policy%d=?", hw.policies()[i].id);
        else
            printf(" policy%d=%lldkHz", hw.policies()[i].id, fs.cpu_khz[i]);
    }
    if (hw.has_gpu_sample()) {
        if (fs.gpu_khz < 0)
            printf(" gpu=?");
        else
            printf(" gpu=%lldkHz", fs.gpu_khz);
    }
    printf("\n");
}

void csv_header(FILE *fp, fsched::SysfsActuator &hw) {
    fputs("ts_ns,tid,q2q_ns,dequeue_wait_ns,hold_ns,exec_delta_ns,delay_delta_ns,resync,lost_events", fp);
    for (size_t i = 0; i < hw.policy_count(); i++)
        fprintf(fp, ",policy%d_khz", hw.policies()[i].id);
    if (hw.has_gpu_sample())
        fputs(",gpu_khz", fp);
    fputc('\n', fp);
    fflush(fp);
}

void csv_row(FILE *fp, const fas_frame &fr, const FreqSample &fs, fsched::SysfsActuator &hw) {
    fprintf(fp, "%llu,%d,", (unsigned long long)fr.ts_ns, fr.tid);
    if (fr.flags & FAS_FRAME_HAS_Q2Q)
        fprintf(fp, "%llu", (unsigned long long)fr.q2q_ns);
    fputc(',', fp);
    if (fr.flags & FAS_FRAME_HAS_DEQUEUE)
        fprintf(fp, "%llu,%llu", (unsigned long long)fr.dequeue_wait_ns, (unsigned long long)fr.hold_ns);
    else
        fputc(',', fp);
    fputc(',', fp);
    if (fr.flags & FAS_FRAME_HAS_SCHED)
        fprintf(fp, "%llu,%llu", (unsigned long long)fr.exec_delta_ns, (unsigned long long)fr.delay_delta_ns);
    else
        fputc(',', fp);
    fprintf(fp, ",%d,%u", (fr.flags & FAS_FRAME_RESYNC) ? 1 : 0, fr.lost_events);
    for (size_t i = 0; i < fs.cpu_khz.size(); i++) {
        fputc(',', fp);
        if (fs.cpu_khz[i] >= 0)
            fprintf(fp, "%lld", fs.cpu_khz[i]);
    }
    if (hw.has_gpu_sample()) {
        fputc(',', fp);
        if (fs.gpu_khz >= 0)
            fprintf(fp, "%lld", fs.gpu_khz);
    }
    fputc('\n', fp);
    fflush(fp);
}

/**
 * Parses a comma separated list of legal FPS targets.
 */
std::vector<int> parse_fps_list(const char *s) {
    std::vector<int> out;
    while (*s) {
        char *end;
        long v = strtol(s, &end, 10);
        if (end == s)
            break;
        if (v > 0)
            out.push_back((int)v);
        s = *end == ',' ? end + 1 : end;
        if (*end != ',')
            break;
    }
    return out;
}

/**
 * Pins the calling thread to the CPUs of the first policy so the controller
 * stays off the cores it is steering.
 */
void pin_to_first_policy(fsched::SysfsActuator &hw) {
    if (hw.policies().empty() || hw.policies()[0].cpus.empty())
        return;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (int c : hw.policies()[0].cpus)
        CPU_SET(c, &set);
    sched_setaffinity(0, sizeof(set), &set);
}

void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s -b <lkm|ebpf> [-q] [-l lib_path] [-c csv_file] [-s sched_log]\n"
            "          [-m powersave|balance|performance] [-f fps,fps,...] [-a] [-G adreno|mtk] [-M model_file|none] [-P] [-v] <pid>\n"
            "       %s -R\n"
            "  -m  enable the scheduler and apply frequency limits\n"
            "  -s  write scheduler decisions to a CSV; without -m the scheduler only observes\n"
            "  -f  legal FPS targets, e.g. 60,90,120\n"
            "  -a  also cap the other clusters in proportion to the primary one\n"
            "  -G  experimental GPU control\n"
            "  -M  file keeping the learned frequency elasticity between sessions, or none\n"
            "      (default /data/local/tmp/fas_sched_model.txt)\n"
            "  -P  do not pin the controller to the first cluster\n"
            "  -v  print every frame even when the scheduler is enabled\n"
            "  -B  accepted render-side miss rate in percent, replaces the mode default (e.g. 2)\n"
            "  -R  restore the frequency limits a killed run left behind, then exit\n"
            "      (Ctrl+C, SIGTERM and crashes restore automatically; only SIGKILL needs this)\n",
            prog, prog);
}

} // namespace

int main(int argc, char **argv) {
    struct fas_options opts;
    struct fas_target target;
    struct fas_info info;
    struct fas_event ev[64];
    struct fas_frame fr;
    char diag[512] = "";
    const char *lib_path = FAS_DEFAULT_LIB_PATH;
    const char *csv_path = nullptr;
    const char *sched_path = nullptr;
    FILE *csv = nullptr;
    FILE *slog = nullptr;
    uint32_t mode = FAS_MODE_AUTO;
    uint32_t backend = FAS_BACKEND_AUTO;
    bool have_backend = false;
    bool have_sched_mode = false;
    bool verbose = false;
    bool restore_only = false;
    bool pin = true;
    const char *model_path = "/data/local/tmp/fas_sched_model.txt";
    fsched::Config scfg;
    fsched::GpuBackend gpu = fsched::GpuBackend::Off;
    fas_frame_ctx_t *an;
    fas_t *f;
    int opt, rc, ret = 0;

    while ((opt = getopt(argc, argv, "b:c:ql:s:m:f:aG:PvM:RB:")) != -1) {
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
                have_backend = true;
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
            case 's':
                sched_path = optarg;
                break;
            case 'm':
                if (!strcmp(optarg, "powersave"))
                    scfg.mode = fsched::Mode::Powersave;
                else if (!strcmp(optarg, "balance"))
                    scfg.mode = fsched::Mode::Balance;
                else if (!strcmp(optarg, "performance"))
                    scfg.mode = fsched::Mode::Performance;
                else {
                    fprintf(stderr, "Invalid mode '%s'\n", optarg);
                    return 2;
                }
                have_sched_mode = true;
                break;
            case 'f':
                scfg.legal_fps = parse_fps_list(optarg);
                break;
            case 'a':
                scfg.manage_all = true;
                break;
            case 'G':
                if (!strcmp(optarg, "adreno"))
                    gpu = fsched::GpuBackend::Adreno;
                else if (!strcmp(optarg, "mtk"))
                    gpu = fsched::GpuBackend::Mtk;
                else {
                    fprintf(stderr, "Invalid GPU backend '%s'\n", optarg);
                    return 2;
                }
                scfg.gpu = true;
                break;
            case 'M':
                model_path = optarg;
                break;
            case 'P':
                pin = false;
                break;
            case 'v':
                verbose = true;
                break;
            case 'R':
                restore_only = true;
                break;
            case 'B':
                scfg.miss_budget = (float)atof(optarg) / 100.0f;
                break;
            default:
                usage(argv[0]);
                return 2;
        }
    }
    if (restore_only) {
        // The actuator undoes the limits recorded by an earlier run while it is constructed.
        fsched::SysfsActuator hw(0, fsched::GpuBackend::Off);
        if (hw.recovered() > 0 || hw.gpu_recovered())
            fprintf(stderr, "restored limits of %d policies%s\n", hw.recovered(), hw.gpu_recovered() ? " and the GPU" : "");
        else
            fprintf(stderr, "nothing to restore: no recovery file from an earlier run\n");
        return 0;
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
    const int pid = atoi(argv[optind]);
    const bool use_sched = have_sched_mode || sched_path;
    scfg.observe_only = !have_sched_mode;
    if (strcmp(model_path, "none") != 0)
        scfg.model_path = model_path;

    fsched::guard::install();

    if (csv_path) {
        csv = fopen(csv_path, "w");
        if (!csv) {
            fprintf(stderr, "cannot open %s: %s\n", csv_path, strerror(errno));
            return 1;
        }
    }
    if (sched_path) {
        slog = fopen(sched_path, "w");
        if (!slog) {
            fprintf(stderr, "cannot open %s: %s\n", sched_path, strerror(errno));
            if (csv)
                fclose(csv);
            return 1;
        }
        scfg.log = slog;
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
        if (slog)
            fclose(slog);
        return 1;
    }

    memset(&target, 0, sizeof(target));
    target.size = sizeof(target);
    target.pid = pid;
    target.lib_path = lib_path;
    target.mode = mode;
    rc = fas_attach(f, &target);
    if (rc < 0) {
        fprintf(stderr, "fas_attach failed: %s\n", strerror(-rc));
        fas_close(f);
        if (csv)
            fclose(csv);
        if (slog)
            fclose(slog);
        return 1;
    }

    fas_get_info(f, &info);
    fprintf(stderr, "backend=%s mode=%s\n", fas_backend_name(info.backend), info.mode == FAS_MODE_Q2Q || mode == FAS_MODE_Q2Q ? "q2q" : "full");

    an = fas_frame_create();
    if (!an) {
        fas_close(f);
        if (csv)
            fclose(csv);
        if (slog)
            fclose(slog);
        return 1;
    }
    fas_frame_set_mode(an, info.mode ? info.mode : mode);

    fsched::SysfsActuator hw(pid, gpu);
    if (hw.recovered() > 0)
        fprintf(stderr, "restored limits of %d policies left behind by an earlier unclean exit\n", hw.recovered());
    if (hw.report_limits(stderr) > 0)
        fprintf(stderr, "limits are locked before this run starts; a baseline taken now is not a governor baseline\n");
    std::unique_ptr<fsched::Scheduler> sched;
    if (use_sched) {
        if (have_sched_mode && !hw.can_control()) {
            fprintf(stderr, "cpufreq limits are not writable (need root); running in observe-only mode\n");
            scfg.observe_only = true;
        }
        if (scfg.gpu && hw.gpu_kind() == fsched::GpuKind::None) {
            fprintf(stderr, "GPU backend not available on this device, GPU control disabled\n");
            scfg.gpu = false;
        }
        if (!scfg.observe_only)
            hw.arm_recovery();
        sched.reset(new fsched::Scheduler(hw, scfg));
        if (pin)
            pin_to_first_policy(hw);
        fprintf(stderr, "scheduler %s, %zu policies%s\n", scfg.observe_only ? "observing" : "controlling", hw.policy_count(), slog ? ", logging" : "");
    }
    if (csv) {
        fprintf(stderr, "recording csv to %s\n", csv_path);
        csv_header(csv, hw);
    }
    const bool need_samples = csv || !use_sched || verbose;

    while (!fsched::guard::stopping()) {
        int n = fas_wait(f, sched ? sched->next_timeout_ms() : 500);

        if (sched)
            sched->tick(mono_ns());
        if (n == -EINTR || n == 0)
            continue;
        if (n < 0)
            break;
        n = fas_read(f, ev, sizeof(ev) / sizeof(ev[0]));
        if (n == -ENODATA)
            break;
        const uint64_t now = mono_ns();
        for (int i = 0; i < n; i++) {
            if (fas_frame_feed(an, &ev[i], &fr) != 1)
                continue;
            if (sched)
                sched->on_frame(fr, now);
            if (!need_samples)
                continue;
            FreqSample fs;
            sample_freqs(hw, fs);
            if (!use_sched || verbose)
                print_frame(fr, fs, hw);
            if (csv)
                csv_row(csv, fr, fs, hw);
        }
    }

    if (fsched::guard::stopping() && sched && !scfg.observe_only)
        fprintf(stderr, "stopping, restoring frequency limits\n");
    if (sched) {
        sched->shutdown();
        if (scfg.observe_only || hw.restore_all())
            hw.disarm_recovery();
        else
            fprintf(stderr, "warning: could not verify that every frequency limit was restored;\n"
                            "the recovery file was kept, run '%s -R' or start again to retry\n", argv[0]);
        fsched::guard::finished();
        const fsched::Summary s = sched->summary();
        fprintf(stderr,
                "frames=%llu fps=%.1f miss=%.2f%% windows=%llu up=%llu down=%llu trials=%llu migrations=%llu clamped=%llu writes=%llu mean_band=%.0f..%.0fkHz eps=%.2f+-%.2f margin=%.2fms\n",
                (unsigned long long)s.frames, s.mean_fps, s.frames ? 100.0 * (double)s.misses / (double)s.frames : 0.0, (unsigned long long)s.windows, (unsigned long long)s.moves_up, (unsigned long long)s.moves_down, (unsigned long long)s.trials, (unsigned long long)s.migrations, (unsigned long long)s.clamped, (unsigned long long)s.writes, s.mean_lo_khz, s.mean_hi_khz, s.eps, s.eps_sd, s.mean_margin);
        fprintf(stderr, "risk by margin/T (P render-side miss, %%) <-.10 <-.05 <0 <.05 <.10 <.15 <.20 <.30 <.45 >=.45: %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f | mean target margin %.2fms | measured render-side miss %.2f%%\n",
                s.risk_pp[0], s.risk_pp[1], s.risk_pp[2], s.risk_pp[3], s.risk_pp[4], s.risk_pp[5], s.risk_pp[6], s.risk_pp[7], s.risk_pp[8], s.risk_pp[9], s.mstar_ms, s.addr_pct);
        sched.reset();
    }
    fas_frame_destroy(an);
    fas_close(f);
    if (csv && fclose(csv) != 0) {
        fprintf(stderr, "error writing %s: %s\n", csv_path, strerror(errno));
        ret = 1;
    }
    if (slog)
        fclose(slog);
    return ret;
}
