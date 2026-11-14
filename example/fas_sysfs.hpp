// SPDX-License-Identifier: BSD-2-Clause

#ifndef FAS_SYSFS_HPP
#define FAS_SYSFS_HPP

#include <cstdio>
#include <string>
#include <vector>

#include "fas_sched.hpp"

namespace fsched {

enum class GpuBackend { Off, Adreno, Mtk };

/**
 * Restores, with async-signal-safe calls only, every limit recorded by the last
 * SysfsActuator::arm_recovery(). Safe in a signal handler or atexit; later calls
 * do nothing until the next arm_recovery().
 *
 * @param forget_state Also remove the recovery file; pass false when the restore
 *        is a blind last attempt after a failed verified restore.
 */
void emergency_restore(bool forget_state = true) noexcept;

/**
 * Actuator over cpufreq sysfs, Adreno devfreq and the MediaTek gpufreq proc
 * nodes. Also serves the per-frame frequency samples of the CSV recorder.
 */
class SysfsActuator : public Actuator {
public:
    /**
     * @param pid Target process id, used to resolve render thread CPUs.
     * @param gpu GPU backend to drive, Off leaves the GPU untouched.
     */
    SysfsActuator(int pid, GpuBackend gpu);
    ~SysfsActuator() override;

    const std::vector<Policy> &policies() const override { return pols_; }
    int render_cpu(int tid) override;
    uint32_t cpu_cur_khz(int pi) override;
    void set_cpu_band(int pi, uint32_t lo_khz, uint32_t hi_khz) override;
    void restore_cpu(int pi) override;

    GpuKind gpu_kind() const override { return gpu_kind_; }
    const std::vector<uint32_t> &gpu_opp_khz() const override { return gpu_opp_; }
    uint32_t gpu_cur_khz() override;
    void set_gpu_band(uint32_t lo_khz, uint32_t hi_khz) override;
    void gpu_lock(uint32_t khz) override;
    void gpu_unlock() override;
    void restore_gpu() override;
    uint64_t write_count() const override { return writes_; }
    int max_temp_mc() override;
    void invalidate_cpu(int pi) override;
    bool read_limits(int pi, uint32_t &min_khz, uint32_t &max_khz) override;

    /** @return True when scaling_min_freq and scaling_max_freq are writable for every policy. */
    bool can_control() const;

    /**
     * Prints the limits found at startup and warns about degenerate ones.
     *
     * @param out Destination stream.
     * @return Number of policies whose min equals max.
     */
    int report_limits(FILE *out) const;

    /** @return Number of policies restored from a previous unclean exit. */
    int recovered() const { return recovered_; }

    /**
     * Restores every cpufreq policy and the GPU to the startup state, then reads
     * the CPU limits back.
     *
     * @return True when every writable policy reads back its startup limits.
     */
    bool restore_all();

    /** @return True when a GPU band or lock left by a killed run was undone. */
    bool gpu_recovered() const { return gpu_recovered_; }

    /**
     * Persists the startup limits so a killed process can be undone, and builds
     * the table used by emergency_restore().
     */
    void arm_recovery();

    /** Removes the persisted limits after a clean restore. */
    void disarm_recovery();

    /** @return Number of cpufreq policies found. */
    size_t policy_count() const { return pols_.size(); }

    /**
     * @param pi Policy index.
     * @return Frequency read from cpuinfo_cur_freq, or -1 if unreadable.
     */
    long long sample_cpu_khz(size_t pi);

    /** @return GPU frequency for the recorder in kHz, or -1 if unavailable. */
    long long sample_gpu_khz();

    /** @return True when a GPU frequency source was opened. */
    bool has_gpu_sample() const { return fd_gpu_clock_ >= 0 || fd_gpu_cur_ >= 0; }

private:
    struct Io {
        int fd_cur = -1;
        int fd_min = -1;
        int fd_max = -1;
        int fd_rmin = -1;
        int fd_rmax = -1;
        uint32_t cur_min = 0;
        uint32_t cur_max = 0;
    };

    int recover_limits();
    bool restore_policy(size_t i);
    void fill_emergency();
    bool write_u(int fd, uint64_t v);
    void write_band(int fd_min, int fd_max, uint32_t &cur_min, uint32_t &cur_max, uint64_t lo, uint64_t hi);
    void init_gpu(GpuBackend gpu);

    int pid_;
    std::vector<Policy> pols_;
    std::vector<Io> io_;
    std::vector<int> therm_fds_;
    int fd_stat_ = -1;
    int stat_tid_ = -1;

    GpuKind gpu_kind_ = GpuKind::None;
    std::vector<uint32_t> gpu_opp_;
    int fd_gpu_clock_ = -1;
    int fd_gpu_cur_ = -1;
    int fd_gpu_min_ = -1;
    int fd_gpu_max_ = -1;
    int fd_gpu_lock_ = -1;
    uint64_t gpu_cur_min_hz_ = 0;
    uint64_t gpu_cur_max_hz_ = 0;
    uint64_t gpu_orig_min_hz_ = 0;
    uint64_t gpu_orig_max_hz_ = 0;
    uint64_t writes_ = 0;
    bool warned_ = false;
    int recovered_ = 0;
    bool gpu_recovered_ = false;
};

} // namespace fsched

#endif
