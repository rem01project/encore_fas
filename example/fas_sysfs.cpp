// SPDX-License-Identifier: BSD-2-Clause

#include "fas_sysfs.hpp"

#include <dirent.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fsched {

namespace {

constexpr const char *kCpufreqDir = "/sys/devices/system/cpu/cpufreq";
constexpr const char *kAdrenoDir = "/sys/class/kgsl/kgsl-3d0/devfreq";
constexpr const char *kAdrenoClock = "/sys/kernel/gpu/gpu_clock";
constexpr const char *kMtkLock = "/proc/gpufreq/gpufreq_opp_freq";
constexpr const char *kMtkDump = "/proc/gpufreq/gpufreq_opp_dump";
constexpr const char *kMtkCur = "/sys/kernel/ged/hal/current_freqency";
constexpr const char *kStateDefault = "/data/local/tmp/fas_sched_orig.txt";

/**
 * @return cpufreq directory, overridable with FAS_CPUFREQ_DIR for testing.
 */
std::string cpufreq_dir() {
    const char *e = getenv("FAS_CPUFREQ_DIR");
    return e && *e ? e : kCpufreqDir;
}

/**
 * @return Path of the recovery file, overridable with FAS_SCHED_STATE.
 */
std::string state_path() {
    const char *e = getenv("FAS_SCHED_STATE");
    return e && *e ? e : kStateDefault;
}

/**
 * Reads up to len-1 bytes of a small text file into buf.
 *
 * @return Bytes read, or -1.
 */
ssize_t slurp(const char *path, char *buf, size_t len) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return -1;
    ssize_t n = read(fd, buf, len - 1);
    close(fd);
    if (n < 0)
        return -1;
    buf[n] = '\0';
    return n;
}

long long read_ll_fd(int fd) {
    char buf[32];
    ssize_t n = pread(fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    return strtoll(buf, nullptr, 10);
}

long long read_ll_path(const std::string &path) {
    char buf[32];
    if (slurp(path.c_str(), buf, sizeof(buf)) <= 0)
        return -1;
    return strtoll(buf, nullptr, 10);
}

/**
 * Parses whitespace separated integers.
 */
std::vector<uint64_t> parse_numbers(const char *s) {
    std::vector<uint64_t> out;
    while (*s) {
        while (*s && (*s < '0' || *s > '9'))
            s++;
        if (!*s)
            break;
        char *end;
        out.push_back(strtoull(s, &end, 10));
        s = end;
    }
    return out;
}

/**
 * Parses a cpu list such as "0-3 5".
 */
std::vector<int> parse_cpus(const char *s) {
    std::vector<int> out;
    while (*s) {
        while (*s == ' ' || *s == ',' || *s == '\n')
            s++;
        if (!*s)
            break;
        char *end;
        long a = strtol(s, &end, 10);
        long b = a;
        if (end == s)
            break;
        if (*end == '-')
            b = strtol(end + 1, &end, 10);
        for (long c = a; c <= b; c++)
            out.push_back((int)c);
        s = end;
    }
    return out;
}

/**
 * Opens path, writes the decimal value and closes it. Does not allocate or
 * touch stdio, so the same sequence is reused by the emergency path.
 *
 * @return True when the whole value was written.
 */
bool write_path(const std::string &path, uint64_t v) {
    char buf[24];
    const int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    const int fd = open(path.c_str(), O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const bool ok = write(fd, buf, (size_t)n) == n;
    close(fd);
    return ok;
}

/**
 * One pre-formatted sysfs write. The table is filled in arm_recovery() so a
 * signal handler only needs open(), write() and close().
 */
struct EmergencyWrite {
    char path[160];
    char val[24];
    unsigned char len;
};

constexpr int kMaxEmergency = 96;
EmergencyWrite g_emerg[kMaxEmergency];
int g_emerg_n = 0;
char g_emerg_state[160];
volatile sig_atomic_t g_emerg_armed = 0;

void emerg_add(const std::string &path, uint64_t v) {
    if (g_emerg_n >= kMaxEmergency || path.size() >= sizeof(g_emerg[0].path))
        return;
    EmergencyWrite &e = g_emerg[g_emerg_n++];
    memcpy(e.path, path.c_str(), path.size() + 1);
    e.len = (unsigned char)snprintf(e.val, sizeof(e.val), "%llu", (unsigned long long)v);
}

} // namespace

void emergency_restore(bool forget_state) noexcept {
    if (!g_emerg_armed)
        return;
    g_emerg_armed = 0;
    for (int i = 0; i < g_emerg_n; i++) {
        const int fd = open(g_emerg[i].path, O_WRONLY | O_CLOEXEC);
        if (fd < 0)
            continue;
        const ssize_t r = write(fd, g_emerg[i].val, g_emerg[i].len);
        (void)r;
        close(fd);
    }
    if (forget_state && g_emerg_state[0])
        unlink(g_emerg_state);
}

SysfsActuator::SysfsActuator(int pid, GpuBackend gpu) : pid_(pid) {
    const std::string dir = cpufreq_dir();
    DIR *d = opendir(dir.c_str());
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != nullptr) {
            int id;
            if (sscanf(de->d_name, "policy%d", &id) != 1)
                continue;
            std::string base = dir + "/" + de->d_name + "/";
            char buf[1024];
            Policy p;
            p.id = id;
            if (slurp((base + "related_cpus").c_str(), buf, sizeof(buf)) > 0)
                p.cpus = parse_cpus(buf);
            long long lo = read_ll_path(base + "cpuinfo_min_freq");
            long long hi = read_ll_path(base + "cpuinfo_max_freq");
            if (lo <= 0 || hi <= 0)
                continue;
            if (slurp((base + "scaling_available_frequencies").c_str(), buf, sizeof(buf)) > 0) {
                for (uint64_t v : parse_numbers(buf))
                    p.opp_khz.push_back((uint32_t)v);
            }
            if (p.opp_khz.size() < 2) {
                p.opp_khz.clear();
                for (int i = 0; i < 8; i++)
                    p.opp_khz.push_back((uint32_t)(lo + (hi - lo) * i / 7));
            }
            std::sort(p.opp_khz.begin(), p.opp_khz.end());
            p.opp_khz.erase(std::unique(p.opp_khz.begin(), p.opp_khz.end()), p.opp_khz.end());
            long long omin = read_ll_path(base + "scaling_min_freq");
            long long omax = read_ll_path(base + "scaling_max_freq");
            p.orig_min_khz = omin > 0 ? (uint32_t)omin : p.opp_khz.front();
            p.orig_max_khz = omax > 0 ? (uint32_t)omax : p.opp_khz.back();
            Io io;
            io.fd_cur = open((base + "cpuinfo_cur_freq").c_str(), O_RDONLY | O_CLOEXEC);
            if (io.fd_cur < 0)
                io.fd_cur = open((base + "scaling_cur_freq").c_str(), O_RDONLY | O_CLOEXEC);
            io.fd_min = open((base + "scaling_min_freq").c_str(), O_WRONLY | O_CLOEXEC);
            io.fd_max = open((base + "scaling_max_freq").c_str(), O_WRONLY | O_CLOEXEC);
            io.fd_rmin = open((base + "scaling_min_freq").c_str(), O_RDONLY | O_CLOEXEC);
            io.fd_rmax = open((base + "scaling_max_freq").c_str(), O_RDONLY | O_CLOEXEC);
            io.cur_min = p.orig_min_khz;
            io.cur_max = p.orig_max_khz;
            pols_.push_back(p);
            io_.push_back(io);
        }
        closedir(d);
    }
    std::vector<size_t> order(pols_.size());
    for (size_t i = 0; i < order.size(); i++)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return pols_[a].id < pols_[b].id; });
    std::vector<Policy> p2;
    std::vector<Io> io2;
    for (size_t i : order) {
        p2.push_back(pols_[i]);
        io2.push_back(io_[i]);
    }
    pols_.swap(p2);
    io_.swap(io2);

    for (int z = 0; z < 64; z++) {
        char path[96], type[64];
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/type", z);
        if (slurp(path, type, sizeof(type)) <= 0)
            continue;
        if (!strstr(type, "cpu") && !strstr(type, "gpu") && !strstr(type, "soc"))
            continue;
        snprintf(path, sizeof(path), "/sys/class/thermal/thermal_zone%d/temp", z);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd >= 0)
            therm_fds_.push_back(fd);
    }

    recovered_ = recover_limits();
    fd_gpu_clock_ = open(kAdrenoClock, O_RDONLY | O_CLOEXEC);
    init_gpu(gpu);
}

SysfsActuator::~SysfsActuator() {
    for (Io &io : io_) {
        if (io.fd_cur >= 0)
            close(io.fd_cur);
        if (io.fd_min >= 0)
            close(io.fd_min);
        if (io.fd_max >= 0)
            close(io.fd_max);
        if (io.fd_rmin >= 0)
            close(io.fd_rmin);
        if (io.fd_rmax >= 0)
            close(io.fd_rmax);
    }
    for (int fd : therm_fds_)
        close(fd);
    for (int fd : {fd_stat_, fd_gpu_clock_, fd_gpu_cur_, fd_gpu_min_, fd_gpu_max_, fd_gpu_lock_}) {
        if (fd >= 0)
            close(fd);
    }
}

/**
 * Opens the requested GPU backend. A backend whose nodes are missing leaves
 * the GPU untouched.
 */
void SysfsActuator::init_gpu(GpuBackend gpu) {
    char buf[4096];
    if (gpu == GpuBackend::Adreno) {
        std::string base = kAdrenoDir;
        if (slurp((base + "/available_frequencies").c_str(), buf, sizeof(buf)) <= 0)
            return;
        for (uint64_t hz : parse_numbers(buf))
            gpu_opp_.push_back((uint32_t)(hz / 1000));
        std::sort(gpu_opp_.begin(), gpu_opp_.end());
        fd_gpu_cur_ = open((base + "/cur_freq").c_str(), O_RDONLY | O_CLOEXEC);
        fd_gpu_min_ = open((base + "/min_freq").c_str(), O_WRONLY | O_CLOEXEC);
        fd_gpu_max_ = open((base + "/max_freq").c_str(), O_WRONLY | O_CLOEXEC);
        gpu_orig_min_hz_ = (uint64_t)std::max(0LL, read_ll_path(base + "/min_freq"));
        gpu_orig_max_hz_ = (uint64_t)std::max(0LL, read_ll_path(base + "/max_freq"));
        gpu_cur_min_hz_ = gpu_orig_min_hz_;
        gpu_cur_max_hz_ = gpu_orig_max_hz_;
        if (fd_gpu_min_ < 0 || fd_gpu_max_ < 0 || gpu_opp_.size() < 2) {
            gpu_opp_.clear();
            return;
        }
        gpu_kind_ = GpuKind::Adreno;
    } else if (gpu == GpuBackend::Mtk) {
        if (slurp(kMtkDump, buf, sizeof(buf)) <= 0)
            return;
        const char *s = buf;
        while ((s = strstr(s, "freq")) != nullptr) {
            s += 4;
            while (*s && (*s < '0' || *s > '9') && *s != '\n')
                s++;
            if (*s >= '0' && *s <= '9')
                gpu_opp_.push_back((uint32_t)strtoul(s, nullptr, 10));
        }
        std::sort(gpu_opp_.begin(), gpu_opp_.end());
        gpu_opp_.erase(std::unique(gpu_opp_.begin(), gpu_opp_.end()), gpu_opp_.end());
        fd_gpu_lock_ = open(kMtkLock, O_WRONLY | O_CLOEXEC);
        fd_gpu_cur_ = open(kMtkCur, O_RDONLY | O_CLOEXEC);
        if (fd_gpu_lock_ < 0 || gpu_opp_.size() < 2) {
            gpu_opp_.clear();
            return;
        }
        gpu_kind_ = GpuKind::Mtk;
    }
}

/**
 * Restores limits saved by a process that died before it could restore them,
 * then adopts them as the startup limits. A GPU band or lock left behind is
 * undone too, whichever GPU backend this run selected.
 *
 * @return Number of CPU policies restored.
 */
int SysfsActuator::recover_limits() {
    FILE *fp = fopen(state_path().c_str(), "r");
    if (!fp)
        return 0;
    int n = 0;
    char line[160];
    while (fgets(line, sizeof(line), fp)) {
        int id;
        unsigned lo, hi;
        unsigned long long gmin, gmax, glow;
        if (sscanf(line, "policy%d %u %u", &id, &lo, &hi) == 3) {
            for (size_t i = 0; i < pols_.size(); i++) {
                if (pols_[i].id != id || lo == 0 || hi < lo)
                    continue;
                pols_[i].orig_min_khz = lo;
                pols_[i].orig_max_khz = hi;
                restore_policy(i);
                n++;
            }
        } else if (sscanf(line, "gpu adreno %llu %llu %llu", &gmin, &gmax, &glow) == 3) {
            if (gmax > 0) {
                const std::string base = kAdrenoDir;
                write_path(base + "/min_freq", glow);
                write_path(base + "/max_freq", gmax);
                if (write_path(base + "/min_freq", gmin))
                    gpu_recovered_ = true;
            }
        } else if (!strncmp(line, "gpu mtk", 7)) {
            if (write_path(kMtkLock, 0))
                gpu_recovered_ = true;
        }
    }
    fclose(fp);
    unlink(state_path().c_str());
    return n;
}

/**
 * Fills the table emergency_restore() replays. Every limit is put back with
 * floor-to-lowest, ceiling, floor so the order is valid whatever the current
 * limits are, including on 4.x kernels that reject max below min.
 */
void SysfsActuator::fill_emergency() {
    g_emerg_armed = 0;
    g_emerg_n = 0;
    snprintf(g_emerg_state, sizeof(g_emerg_state), "%s", state_path().c_str());
    const std::string dir = cpufreq_dir();
    for (size_t i = 0; i < pols_.size(); i++) {
        const Policy &p = pols_[i];
        if (io_[i].fd_min < 0 || io_[i].fd_max < 0)
            continue;
        const std::string base = dir + "/policy" + std::to_string(p.id) + "/";
        const uint64_t lowest = std::min<uint64_t>(p.opp_khz.empty() ? p.orig_min_khz : p.opp_khz.front(), p.orig_min_khz);
        emerg_add(base + "scaling_min_freq", lowest);
        emerg_add(base + "scaling_max_freq", p.orig_max_khz);
        emerg_add(base + "scaling_min_freq", p.orig_min_khz);
    }
    if (gpu_kind_ == GpuKind::Adreno && gpu_orig_max_hz_ > 0 && !gpu_opp_.empty()) {
        const std::string base = kAdrenoDir;
        emerg_add(base + "/min_freq", std::min<uint64_t>((uint64_t)gpu_opp_.front() * 1000, gpu_orig_min_hz_));
        emerg_add(base + "/max_freq", gpu_orig_max_hz_);
        emerg_add(base + "/min_freq", gpu_orig_min_hz_);
    } else if (gpu_kind_ == GpuKind::Mtk) {
        emerg_add(kMtkLock, 0);
    }
    g_emerg_armed = 1;
}

void SysfsActuator::arm_recovery() {
    fill_emergency();
    FILE *fp = fopen(state_path().c_str(), "w");
    if (!fp)
        return;
    for (const Policy &p : pols_)
        fprintf(fp, "policy%d %u %u\n", p.id, p.orig_min_khz, p.orig_max_khz);
    if (gpu_kind_ == GpuKind::Adreno && gpu_orig_max_hz_ > 0 && !gpu_opp_.empty())
        fprintf(fp, "gpu adreno %llu %llu %llu\n", (unsigned long long)gpu_orig_min_hz_, (unsigned long long)gpu_orig_max_hz_, (unsigned long long)std::min<uint64_t>((uint64_t)gpu_opp_.front() * 1000, gpu_orig_min_hz_));
    else if (gpu_kind_ == GpuKind::Mtk)
        fprintf(fp, "gpu mtk\n");
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
}

void SysfsActuator::disarm_recovery() {
    g_emerg_armed = 0;
    unlink(state_path().c_str());
}

int SysfsActuator::report_limits(FILE *out) const {
    int locked = 0;
    for (const Policy &p : pols_) {
        fprintf(out, "policy%d limits %u..%u kHz (opp %zu)%s\n", p.id, p.orig_min_khz, p.orig_max_khz, p.opp_khz.size(), p.orig_min_khz == p.orig_max_khz ? "  WARNING: min==max, frequency is locked" : "");
        if (p.orig_min_khz == p.orig_max_khz)
            locked++;
    }
    return locked;
}

/**
 * @return True when every policy exposes writable limit files.
 */
bool SysfsActuator::can_control() const {
    if (pols_.empty())
        return false;
    for (const Io &io : io_) {
        if (io.fd_min < 0 || io.fd_max < 0)
            return false;
    }
    return true;
}

bool SysfsActuator::write_u(int fd, uint64_t v) {
    char buf[24];
    int n = snprintf(buf, sizeof(buf), "%llu", (unsigned long long)v);
    writes_++;
    if (pwrite(fd, buf, (size_t)n, 0) != n) {
        if (!warned_) {
            warned_ = true;
            fprintf(stderr, "sysfs write failed: %s\n", strerror(errno));
        }
        return false;
    }
    return true;
}

/**
 * Writes a min/max pair in the order the kernel accepts: ceiling first when
 * raising, floor first when lowering.
 */
void SysfsActuator::write_band(int fd_min, int fd_max, uint32_t &cur_min, uint32_t &cur_max, uint64_t lo, uint64_t hi) {
    if (lo == cur_min && hi == cur_max)
        return;
    if (hi > cur_max) {
        write_u(fd_max, hi);
        write_u(fd_min, lo);
    } else {
        write_u(fd_min, lo);
        write_u(fd_max, hi);
    }
    cur_min = (uint32_t)lo;
    cur_max = (uint32_t)hi;
}

void SysfsActuator::set_cpu_band(int pi, uint32_t lo_khz, uint32_t hi_khz) {
    Io &io = io_[(size_t)pi];
    if (io.fd_min < 0 || io.fd_max < 0)
        return;
    write_band(io.fd_min, io.fd_max, io.cur_min, io.cur_max, lo_khz, hi_khz);
}

void SysfsActuator::restore_cpu(int pi) {
    restore_policy((size_t)pi);
}

/**
 * Puts one policy back to its startup limits and verifies by reading them back.
 * The write order does not depend on the limits currently in force: the floor
 * goes to the lowest value first, then the ceiling, then the real floor. A
 * ceiling-first order is rejected by 4.x kernels whenever the floor is still
 * above the old ceiling.
 *
 * @param i Policy index.
 * @return True when the read-back equals the startup limits.
 */
bool SysfsActuator::restore_policy(size_t i) {
    Io &io = io_[i];
    const Policy &p = pols_[i];
    if (io.fd_min < 0 || io.fd_max < 0)
        return false;
    const uint64_t lowest = std::min<uint64_t>(p.opp_khz.empty() ? p.orig_min_khz : p.opp_khz.front(), p.orig_min_khz);
    bool ok = false;
    for (int attempt = 0; attempt < 3 && !ok; attempt++) {
        write_u(io.fd_min, lowest);
        write_u(io.fd_max, p.orig_max_khz);
        write_u(io.fd_min, p.orig_min_khz);
        if (io.fd_rmin < 0 || io.fd_rmax < 0) {
            ok = true;
            break;
        }
        ok = read_ll_fd(io.fd_rmin) == (long long)p.orig_min_khz && read_ll_fd(io.fd_rmax) == (long long)p.orig_max_khz;
    }
    io.cur_min = ok ? p.orig_min_khz : 0;
    io.cur_max = ok ? p.orig_max_khz : 0;
    return ok;
}

bool SysfsActuator::restore_all() {
    bool ok = true;
    for (size_t i = 0; i < pols_.size(); i++) {
        if (io_[i].fd_min < 0 || io_[i].fd_max < 0)
            continue;
        ok = restore_policy(i) && ok;
    }
    restore_gpu();
    return ok;
}

void SysfsActuator::invalidate_cpu(int pi) {
    io_[(size_t)pi].cur_min = io_[(size_t)pi].cur_max = 0;
}

int SysfsActuator::max_temp_mc() {
    long long best = -1;
    for (int fd : therm_fds_)
        best = std::max(best, read_ll_fd(fd));
    return (int)best;
}

bool SysfsActuator::read_limits(int pi, uint32_t &min_khz, uint32_t &max_khz) {
    if (pi < 0)
        return false;
    const Io &io = io_[(size_t)pi];
    if (io.fd_rmin < 0 || io.fd_rmax < 0)
        return false;
    long long lo = read_ll_fd(io.fd_rmin), hi = read_ll_fd(io.fd_rmax);
    if (lo < 0 || hi < 0)
        return false;
    min_khz = (uint32_t)lo;
    max_khz = (uint32_t)hi;
    return true;
}

uint32_t SysfsActuator::cpu_cur_khz(int pi) {
    long long v = pi >= 0 ? sample_cpu_khz((size_t)pi) : -1;
    return v > 0 ? (uint32_t)v : 0;
}

long long SysfsActuator::sample_cpu_khz(size_t pi) {
    return io_[pi].fd_cur >= 0 ? read_ll_fd(io_[pi].fd_cur) : -1;
}

/**
 * Reads the CPU field of /proc/pid/task/tid/stat.
 */
int SysfsActuator::render_cpu(int tid) {
    if (tid != stat_tid_) {
        if (fd_stat_ >= 0)
            close(fd_stat_);
        char path[64];
        snprintf(path, sizeof(path), "/proc/%d/task/%d/stat", pid_, tid);
        fd_stat_ = open(path, O_RDONLY | O_CLOEXEC);
        stat_tid_ = tid;
    }
    if (fd_stat_ < 0)
        return -1;
    char buf[512];
    ssize_t n = pread(fd_stat_, buf, sizeof(buf) - 1, 0);
    if (n <= 0)
        return -1;
    buf[n] = '\0';
    const char *s = strrchr(buf, ')');
    if (!s)
        return -1;
    s++;
    for (int tok = 0; tok < 37 && *s; tok++) {
        while (*s == ' ')
            s++;
        if (tok == 36)
            return atoi(s);
        while (*s && *s != ' ')
            s++;
    }
    return -1;
}

long long SysfsActuator::sample_gpu_khz() {
    if (fd_gpu_clock_ >= 0) {
        long long mhz = read_ll_fd(fd_gpu_clock_);
        return mhz >= 0 ? mhz * 1000 : -1;
    }
    if (fd_gpu_cur_ >= 0) {
        long long v = read_ll_fd(fd_gpu_cur_);
        if (v < 0)
            return -1;
        return gpu_kind_ == GpuKind::Adreno ? v / 1000 : v;
    }
    return -1;
}

uint32_t SysfsActuator::gpu_cur_khz() {
    long long v = sample_gpu_khz();
    return v > 0 ? (uint32_t)v : 0;
}

void SysfsActuator::set_gpu_band(uint32_t lo_khz, uint32_t hi_khz) {
    if (gpu_kind_ != GpuKind::Adreno)
        return;
    const uint64_t lo = (uint64_t)lo_khz * 1000, hi = (uint64_t)hi_khz * 1000;
    if (lo == gpu_cur_min_hz_ && hi == gpu_cur_max_hz_)
        return;
    if (hi > gpu_cur_max_hz_) {
        write_u(fd_gpu_max_, hi);
        write_u(fd_gpu_min_, lo);
    } else {
        write_u(fd_gpu_min_, lo);
        write_u(fd_gpu_max_, hi);
    }
    gpu_cur_min_hz_ = lo;
    gpu_cur_max_hz_ = hi;
}

void SysfsActuator::gpu_lock(uint32_t khz) {
    if (gpu_kind_ == GpuKind::Mtk && fd_gpu_lock_ >= 0)
        write_u(fd_gpu_lock_, khz);
}

void SysfsActuator::gpu_unlock() {
    if (gpu_kind_ == GpuKind::Mtk && fd_gpu_lock_ >= 0)
        write_u(fd_gpu_lock_, 0);
}

void SysfsActuator::restore_gpu() {
    if (gpu_kind_ == GpuKind::Adreno) {
        if (gpu_orig_max_hz_ == 0 || gpu_opp_.empty())
            return;
        write_u(fd_gpu_min_, std::min<uint64_t>((uint64_t)gpu_opp_.front() * 1000, gpu_orig_min_hz_));
        write_u(fd_gpu_max_, gpu_orig_max_hz_);
        write_u(fd_gpu_min_, gpu_orig_min_hz_);
        gpu_cur_min_hz_ = gpu_orig_min_hz_;
        gpu_cur_max_hz_ = gpu_orig_max_hz_;
    } else if (gpu_kind_ == GpuKind::Mtk) {
        gpu_unlock();
    }
}

} // namespace fsched
