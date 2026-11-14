// SPDX-License-Identifier: BSD-2-Clause

#ifndef FAS_SCHED_HPP
#define FAS_SCHED_HPP

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "fas/fas_frame.h"

namespace fsched {

enum class Mode { Powersave, Balance, Performance };

enum class GpuKind { None, Adreno, Mtk };

struct Policy {
    int id = 0;
    std::vector<int> cpus;
    std::vector<uint32_t> opp_khz; // ascending
    uint32_t orig_min_khz = 0;
    uint32_t orig_max_khz = 0;
};

/**
 * Hardware access used by the scheduler. Implemented over sysfs on device and
 * over a mock plant in the offline simulator.
 */
class Actuator {
public:
    virtual ~Actuator() = default;

    /** @return All cpufreq policies sorted by id. */
    virtual const std::vector<Policy> &policies() const = 0;

    /**
     * @param tid Render thread id.
     * @return CPU the thread last ran on, or -1 if unknown.
     */
    virtual int render_cpu(int tid) = 0;

    /**
     * @param policy_index Index into policies().
     * @return Instantaneous frequency in kHz, or 0 if unreadable.
     */
    virtual uint32_t cpu_cur_khz(int policy_index) = 0;

    /**
     * Restricts a policy to [lo_khz, hi_khz].
     *
     * @param policy_index Index into policies().
     * @param lo_khz Frequency floor.
     * @param hi_khz Frequency ceiling.
     */
    virtual void set_cpu_band(int policy_index, uint32_t lo_khz, uint32_t hi_khz) = 0;

    /** Restores the limits found at startup. */
    virtual void restore_cpu(int policy_index) = 0;

    virtual GpuKind gpu_kind() const { return GpuKind::None; }
    virtual const std::vector<uint32_t> &gpu_opp_khz() const;
    virtual uint32_t gpu_cur_khz() { return 0; }
    virtual void set_gpu_band(uint32_t lo_khz, uint32_t hi_khz) { (void)lo_khz; (void)hi_khz; }
    virtual void gpu_lock(uint32_t khz) { (void)khz; }
    virtual void gpu_unlock() {}
    virtual void restore_gpu() {}
    virtual uint64_t write_count() const { return 0; }

    /** Forgets cached writes so the next band is written unconditionally. */
    virtual void invalidate_cpu(int policy_index) { (void)policy_index; }

    /** @return Hottest CPU/GPU/SoC thermal zone in milli-degrees C, or -1. */
    virtual int max_temp_mc() { return -1; }

    /**
     * Reads back the limits currently enforced by the kernel.
     *
     * @param policy_index Index into policies().
     * @param min_khz Receives scaling_min_freq.
     * @param max_khz Receives scaling_max_freq.
     * @return True on success.
     */
    virtual bool read_limits(int policy_index, uint32_t &min_khz, uint32_t &max_khz) {
        (void)policy_index;
        (void)min_khz;
        (void)max_khz;
        return false;
    }
};

struct Config {
    Mode mode = Mode::Balance;
    bool observe_only = false;
    bool manage_all = false;
    bool gpu = false;
    std::string model_path;
    float miss_budget = 0.0f; ///< accepted probability of a render-side miss per frame, 0 uses the mode default
    std::vector<int> legal_fps;
    FILE *log = nullptr;
};

struct Summary {
    uint64_t frames = 0;
    uint64_t windows = 0;
    uint64_t stalls = 0;
    uint64_t misses = 0;
    uint64_t moves_up = 0;
    uint64_t moves_down = 0;
    uint64_t trials = 0;
    uint64_t migrations = 0;
    uint64_t clamped = 0;
    uint64_t writes = 0;
    double mean_fps = 0.0;
    double mean_lo_khz = 0.0;
    double mean_hi_khz = 0.0;
    float eps = 0.0f;
    float eps_sd = 0.0f;
    float mean_margin = 0.0f;
    float addr_pct = 0.0f; ///< measured share of frames that were render-side misses
    float mstar_ms = 0.0f; ///< mean target margin over the session, in ms
    float risk_pp[10] = {};
    float risk_w[10] = {};
};

namespace detail {
struct Impl;
}

/**
 * Headroom controller. Feed frames with on_frame(), call tick() whenever the
 * event loop wakes, and call shutdown() before exit.
 */
class Scheduler {
public:
    Scheduler(Actuator &act, const Config &cfg);
    ~Scheduler();

    Scheduler(const Scheduler &) = delete;
    Scheduler &operator=(const Scheduler &) = delete;

    /**
     * Consumes one frame.
     *
     * @param fr Frame from fas_frame_feed().
     * @param now_ns CLOCK_MONOTONIC time at which the frame was delivered.
     */
    void on_frame(const fas_frame &fr, uint64_t now_ns);

    /**
     * Watchdog and GPU pulse handling.
     *
     * @param now_ns CLOCK_MONOTONIC time.
     */
    void tick(uint64_t now_ns);

    /** @return Poll timeout in milliseconds the event loop should use. */
    int next_timeout_ms() const;

    /** Releases every limit that was applied. */
    void shutdown();

    /** @return Aggregated statistics since construction. */
    Summary summary() const;

private:
    detail::Impl *p_;
};

} // namespace fsched

#endif
