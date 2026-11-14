// SPDX-License-Identifier: BSD-2-Clause

#include "fas_sched.hpp"

#include "fas_margin.hpp"

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>

namespace fsched {

const std::vector<uint32_t> &Actuator::gpu_opp_khz() const {
    static const std::vector<uint32_t> empty;
    return empty;
}

namespace {

constexpr float kBinMs = 0.25f;
constexpr int kBins = 256;
constexpr int kRing = 512;
constexpr int kQWin = 100;
constexpr int kGapWin = 300;
constexpr int kWinFrames = 8;
constexpr uint64_t kWinMinNs = 50000000ull;
constexpr int kWarmupFrames = 160;
constexpr float kStallMs = 250.0f;
constexpr uint64_t kIdleNs = 400000000ull;
constexpr uint64_t kResetNs = 1500000000ull;
constexpr int kSettleFrames = 8;
constexpr int kMeasureFrames = 16;
constexpr int kMaxOpp = 64;
constexpr float kLocStep = 0.0002f;
constexpr float kBadK = 1.25f;
constexpr int kWarmFrames = 2500;
constexpr int kDither = 3;
constexpr int kGuardAgeWindows = 250;
constexpr int kGuardAgeFast = 20;
constexpr float kLvlAlpha = 0.1f;
constexpr float kTau = 0.9f;
constexpr int kMinResid = 128;
constexpr float kFnowAlpha = 0.5f;
constexpr float kPressureMs = 0.0f;
constexpr float kLnMin = 0.3f;
constexpr double kContrastClip = 0.35;
constexpr int kEpsKeep = 100;
constexpr uint64_t kSaveNs = 600000000000ull;

struct ModeParams {
    float budget; // accepted probability of a render-side miss per frame
    float zb;
    int down;
    int up;
    int dwell;
    float bq;
};

const ModeParams kModes[3] = {
    {0.035f, 0.0f, 2, 1, 4, 0.75f},
    {0.020f, 0.5f, 1, 2, 8, 0.9f},
    {0.010f, 1.0f, 0, 3, 16, 0.95f},
};

template <int N>
struct Ring {
    static_assert((N & (N - 1)) == 0, "ring size must be a power of two");
    float v[N];
    uint32_t head = 0;
    int count = 0;

    void push(float x) {
        v[head & (N - 1)] = x;
        head++;
        if (count < N)
            count++;
    }

    int copy_last(int n, float *dst) const {
        if (n > count)
            n = count;
        for (int i = 0; i < n; i++)
            dst[i] = v[(head - (uint32_t)n + (uint32_t)i) & (N - 1)];
        return n;
    }

    void clear() {
        head = 0;
        count = 0;
    }
};

inline float ms(uint64_t ns) {
    return (float)ns * 1e-6f;
}

inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : (x > hi ? hi : x);
}

/**
 * Sorts the buffer and returns the p-quantile.
 *
 * @param s Buffer, reordered in place.
 * @param n Element count, must be positive.
 * @param p Quantile in [0, 1].
 */
float quantile(float *s, int n, float p) {
    std::sort(s, s + n);
    return s[(int)(p * (float)(n - 1) + 0.5f)];
}

enum class Trial { Idle, SettleA, MeasA, SettleB, MeasB };

} // namespace

namespace detail {

struct Impl {
    Actuator &act;
    Config cfg;
    const ModeParams *mp;
    Summary sum;

    Ring<kRing> q, busy, gap, ovh;
    float tmp[kRing];
    float tmp2[kRing];

    uint8_t pace_bins[kRing];
    uint32_t pace_head = 0;
    int pace_n = 0;
    int hist[kBins];
    float t_fast = 0.0f;
    bool pace_valid = false;
    float env[32];
    int env_n = 0;
    uint32_t env_head = 0;
    uint64_t env_last_ns = 0;
    float t_ref = 0.0f;
    uint64_t futile_until_ns = 0;

    float pin_b[24], pin_g[24];
    int pin_n = 0;
    uint32_t pin_head = 0;
    uint64_t pin_last_ns = 0;
    int pinned = -1;

    uint64_t last_frame_ns = 0;
    uint64_t last_decide_ns = 0;
    uint64_t last_save_ns = 0;
    uint64_t frames_since_reset = 0;
    uint64_t frames_total = 0;
    uint64_t scored_frames = 0;
    double sum_q_ms = 0.0;
    double w_lo = 0.0, w_hi = 0.0, w_dt = 0.0;
    int win_frames = 0;
    float dqf = 0.0f;
    bool idle = false;
    int tid = 0;

    int votes[16];
    int vote_n = 0;
    uint32_t vote_head = 0;
    int primary = -1;
    std::vector<bool> touched;
    int idx = 0;
    uint32_t cmd_lo = 0, cmd_hi = 0;
    bool capped = false;
    int cap_idx = INT_MAX;

    int healthy_cnt = 0;
    int cooldown_up = 0;
    int cooldown_fail = 0;
    int dwell_mult = 1;
    int win_since_move = 0;
    int win_since_down = 1000;
    int healthy_total = 0;
    int fail_streak = 0;
    int floor_idx = 0;
    int guard_age = 0;
    int settle_left = 0;
    float gmin_cached = 0.0f;
    float gfloor = 0.0f;
    float qr0 = 0.0f;
    int temp_mc = -1;
    uint32_t rb_min = 0, rb_max = 0, cur_khz = 0;
    uint64_t temp_ns = 0;

    float loc = 0.0f;
    float b_short = 0.03f;
    float b_long = 0.03f;
    float prem_now = 0.0f;
    int tgt_idx = 0;

    Ring<kRing> resid;
    float lvl = 0.0f;
    bool lvl_init = false;
    float f_now = 0.0f;
    float m_now = 0.0f, q_now = 0.0f, ex_pp = 0.0f;
    margin::RiskCurve curve;
    float ustar_s = -1.0f, m_star_now = 0.0f, u_cnt = 0.0f, u_var = 0.0064f;
    bool u_valid = false;
    float bmed = 0.0f, gmed = 0.0f;
    double sum_margin = 0.0, sum_mstar = 0.0;
    uint64_t n_counted = 0, n_addr = 0;
    uint64_t n_margin = 0;
    std::vector<margin::Elasticity> el;
    float eps_last = 0.0f, eps_sd_last = 0.0f;
    uint64_t trials_prior = 0;

    Trial trial = Trial::Idle;
    uint64_t stage_start_frame = 0;
    int trial_idx_a = 0;
    int trial_home = 0;
    int trial_dir = -kDither;
    bool trial_flip = false;
    int win_since_trial = 0;
    int trA_n = 0, trB_n = 0;
    double trA_lb = 0.0, trB_lb = 0.0;

    Ring<64> gpu_hist;
    int gpu_pressure = 0;
    int gpu_boost_left = 0;
    bool gpu_cap_active = false;
    bool gpu_lock_active = false;
    uint32_t gpu_boost_khz = 0;
    uint64_t gpu_lock_until_ns = 0;

    Impl(Actuator &a, const Config &c);

    void reset_session();
    void release_all();
    void on_frame(const fas_frame &fr, uint64_t now);
    void tick(uint64_t now);
    void decide(uint64_t now);
    void update_pace();
    void pin_update(uint64_t now);
    void update_ref(uint64_t now);
    void select_primary();
    void init_primary(int pi);
    void apply(bool narrow);
    void set_band(int pi, uint32_t lo, uint32_t hi);
    int ceil_idx(uint32_t khz) const;
    int policy_of_cpu(int cpu) const;
    void sample_diag(uint64_t now);
    void model_load();
    void model_save();
    int choose_target_margin(int top_i);
    void run_trial(bool fail, int top_i);
    void finish_trial(int idx_b);
    void gpu_step(uint64_t now, bool fail, bool exhausted, bool comfortable);
    void log_header();
    void log_row(uint64_t now, const char *action, const char *reason, float q50, float q95, float miss, float gmin, float busy_eff, float h, float margin, float sigma);
};

/**
 * @param a Hardware interface.
 * @param c Scheduler configuration.
 */
Impl::Impl(Actuator &a, const Config &c) : act(a), cfg(c) {
    mp = &kModes[(int)c.mode];
    touched.assign(a.policies().size(), false);
    memset(hist, 0, sizeof(hist));
    memset(pace_bins, 0, sizeof(pace_bins));
    el.assign(a.policies().size(), margin::Elasticity());
    if (!cfg.model_path.empty())
        model_load();
    if (cfg.log)
        log_header();
}

/**
 * Clears all estimators and releases the applied limits.
 */
void Impl::reset_session() {
    q.clear();
    busy.clear();
    gap.clear();
    ovh.clear();
    resid.clear();
    lvl = 0.0f;
    lvl_init = false;
    u_valid = false;
    gmin_cached = 0.0f;
    gfloor = 0.0f;
    qr0 = 0.0f;
    loc = 0.0f;
    settle_left = kWarmFrames;
    gpu_hist.clear();
    memset(hist, 0, sizeof(hist));
    pace_head = 0;
    pace_n = 0;
    pace_valid = false;
    t_fast = 0.0f;
    t_ref = 0.0f;
    env_n = 0;
    pin_n = 0;
    pinned = -1;
    frames_since_reset = 0;
    win_frames = 0;
    vote_n = 0;
    healthy_cnt = 0;
    fail_streak = 0;
    trial = Trial::Idle;
    futile_until_ns = 0;
    release_all();
}

/**
 * Restores every limit that was changed.
 */
void Impl::release_all() {
    if (!cfg.observe_only) {
        for (size_t i = 0; i < touched.size(); i++) {
            if (touched[i])
                act.restore_cpu((int)i);
        }
        if (cfg.gpu)
            act.restore_gpu();
    }
    std::fill(touched.begin(), touched.end(), false);
    primary = -1;
    capped = false;
    cap_idx = INT_MAX;
    gpu_cap_active = false;
    gpu_lock_active = false;
    gpu_boost_left = 0;
}

/**
 * @param khz Target frequency.
 * @return Index of the lowest OPP of the primary policy that is >= khz.
 */
int Impl::ceil_idx(uint32_t khz) const {
    const std::vector<uint32_t> &o = act.policies()[(size_t)primary].opp_khz;
    int n = (int)o.size();
    int i = (int)(std::lower_bound(o.begin(), o.end(), khz) - o.begin());
    return i >= n ? n - 1 : i;
}

/**
 * @param cpu Logical CPU number.
 * @return Policy index owning the CPU, or -1.
 */
int Impl::policy_of_cpu(int cpu) const {
    const std::vector<Policy> &p = act.policies();
    for (size_t i = 0; i < p.size(); i++) {
        for (int c : p[i].cpus) {
            if (c == cpu)
                return (int)i;
        }
    }
    return -1;
}

/**
 * Writes a band for one policy unless the controller is observing only.
 */
void Impl::set_band(int pi, uint32_t lo, uint32_t hi) {
    if (pi == primary) {
        cmd_lo = lo;
        cmd_hi = hi;
    }
    if (!cfg.observe_only)
        act.set_cpu_band(pi, lo, hi);
    touched[(size_t)pi] = true;
}

/**
 * Applies the band derived from the current primary index.
 *
 * @param narrow Pin the primary policy to a single OPP (dither trials).
 */
void Impl::apply(bool narrow) {
    const std::vector<Policy> &pols = act.policies();
    const Policy &pp = pols[(size_t)primary];
    const int n = (int)pp.opp_khz.size();
    const int lo_i = narrow ? idx : std::max(0, idx - mp->down);
    const int hi_i = narrow ? idx : std::min(n - 1, idx + mp->up);

    set_band(primary, pp.opp_khz[(size_t)lo_i], pp.opp_khz[(size_t)hi_i]);
    if (!cfg.manage_all || n < 2)
        return;
    const float frac = (float)idx / (float)(n - 1);
    for (size_t k = 0; k < pols.size(); k++) {
        if ((int)k == primary || pols[k].opp_khz.size() < 2)
            continue;
        const int m = (int)pols[k].opp_khz.size();
        const int j = std::min(m - 1, (int)(frac * (float)(m - 1) + 0.5f) + mp->up + 1);
        set_band((int)k, pols[k].opp_khz.front(), pols[k].opp_khz[(size_t)j]);
    }
}

/**
 * Starts controlling a policy.
 *
 * @param pi Policy index that hosts the render thread.
 */
void Impl::init_primary(int pi) {
    if (primary >= 0 && primary != pi) {
        if (!cfg.observe_only)
            act.restore_cpu(primary);
        touched[(size_t)primary] = false;
        sum.migrations++;
    }
    primary = pi;
    settle_left = std::max(settle_left, kWarmFrames / 2);
    const uint32_t cur = act.cpu_cur_khz(pi);
    const int n = (int)act.policies()[(size_t)pi].opp_khz.size();
    idx = std::min(n - 1, (cur ? ceil_idx(cur) : n - 1) + 2);
    healthy_cnt = 0;
    cooldown_up = 0;
    cooldown_fail = 0;
    win_since_move = 0;
    trial = Trial::Idle;
    apply(false);
}

/**
 * Polls the render thread CPU and switches the primary policy on a stable
 * majority.
 */
void Impl::select_primary() {
    const int cpu = act.render_cpu(tid);
    if (cpu >= 0) {
        const int pi = policy_of_cpu(cpu);
        if (pi >= 0) {
            votes[vote_head & 15] = pi;
            vote_head++;
            if (vote_n < 16)
                vote_n++;
        }
    }
    if (vote_n < 8) {
        if (primary < 0 && act.policies().size() == 1)
            init_primary(0);
        return;
    }
    int best = -1, best_c = 0;
    for (size_t p = 0; p < act.policies().size(); p++) {
        int c = 0;
        for (int i = 0; i < vote_n; i++) {
            if (votes[i] == (int)p)
                c++;
        }
        if (c > best_c) {
            best_c = c;
            best = (int)p;
        }
    }
    if (best < 0)
        return;
    if (primary < 0 || (best != primary && best_c * 4 >= vote_n * 3))
        init_primary(best);
}

/**
 * Re-estimates the dominant frame period from the histogram of recent Q2Q.
 */
void Impl::update_pace() {
    if (pace_n < 120)
        return;
    int best = 0, best_s = -1;
    for (int i = 1; i < kBins - 1; i++) {
        const int s = hist[i - 1] + hist[i] + hist[i + 1];
        if (s > best_s) {
            best_s = s;
            best = i;
        }
    }
    int cur = best;
    if (pace_valid) {
        cur = std::min(kBins - 2, std::max(1, (int)(t_fast / kBinMs)));
        const int cs = hist[cur - 1] + hist[cur] + hist[cur + 1];
        if ((float)best_s > 1.25f * (float)cs)
            cur = best;
    }
    float num = 0.0f, den = 0.0f;
    for (int j = cur - 1; j <= cur + 1; j++) {
        num += (float)hist[j] * ((float)j + 0.5f) * kBinMs;
        den += (float)hist[j];
    }
    if (den > 0.0f) {
        t_fast = num / den;
        pace_valid = true;
    }
}

/**
 * Detects whether the game limiter absorbs render time: slope of gap against
 * busy over 0.25 s medians.
 */
void Impl::pin_update(uint64_t now) {
    if (dqf < 0.8f || now - pin_last_ns < 250000000ull)
        return;
    int n = busy.copy_last(50, tmp);
    if (n < 50)
        return;
    pin_last_ns = now;
    const float mb = quantile(tmp, n, 0.5f);
    n = gap.copy_last(50, tmp);
    const float mg = quantile(tmp, n, 0.5f);
    pin_b[pin_head % 24] = mb;
    pin_g[pin_head % 24] = mg;
    pin_head++;
    if (pin_n < 24)
        pin_n++;
    if (pin_n < 12)
        return;
    float ab = 0, ag = 0;
    for (int i = 0; i < pin_n; i++) {
        ab += pin_b[i];
        ag += pin_g[i];
    }
    ab /= (float)pin_n;
    ag /= (float)pin_n;
    float vb = 0, cv = 0;
    for (int i = 0; i < pin_n; i++) {
        vb += (pin_b[i] - ab) * (pin_b[i] - ab);
        cv += (pin_b[i] - ab) * (pin_g[i] - ag);
    }
    vb /= (float)pin_n;
    cv /= (float)pin_n;
    if (vb < 0.04f)
        return;
    const float slope = cv / vb;
    if (slope < -0.5f)
        pinned = 1;
    else if (slope > -0.25f)
        pinned = 0;
}

/**
 * Derives the reference period: the game pace when it is self limited, the
 * fastest recent pace otherwise. Clamped and snapped to legal FPS targets.
 */
void Impl::update_ref(uint64_t now) {
    if (now - env_last_ns >= 1000000000ull) {
        env_last_ns = now;
        env[env_head & 31] = t_fast;
        env_head++;
        if (env_n < 32)
            env_n++;
    }
    float t_env = t_fast;
    for (int i = 0; i < env_n; i++)
        t_env = std::min(t_env, env[i]);
    float r = (pinned == 1 || futile_until_ns > now) ? t_fast : t_env;
    if (!cfg.legal_fps.empty()) {
        int max_l = 0;
        for (int l : cfg.legal_fps)
            max_l = std::max(max_l, l);
        r = std::max(r, 1000.0f / (float)max_l * 0.97f);
        const float fps = 1000.0f / r;
        for (int l : cfg.legal_fps) {
            if (std::fabs(fps - (float)l) <= 4.0f) {
                r = 1000.0f / (float)l;
                break;
            }
        }
    }
    t_ref = r;
}

/**
 * Samples temperature once per second and the enforced limits and current
 * frequency of the primary policy every window. Counts windows in which the
 * kernel enforces a ceiling below the one we commanded.
 */
void Impl::sample_diag(uint64_t now) {
    if (now - temp_ns >= 1000000000ull) {
        temp_ns = now;
        temp_mc = act.max_temp_mc();
    }
    rb_min = rb_max = 0;
    capped = false;
    cap_idx = INT_MAX;
    cur_khz = act.cpu_cur_khz(primary);
    if (cur_khz)
        f_now = f_now > 0.0f ? f_now + kFnowAlpha * ((float)cur_khz - f_now) : (float)cur_khz;
    if (!act.read_limits(primary, rb_min, rb_max) || cfg.observe_only || !cmd_hi || !rb_max)
        return;
    if (rb_max < cmd_hi) {
        const std::vector<uint32_t> &o = act.policies()[(size_t)primary].opp_khz;
        sum.clamped++;
        capped = true;
        cap_idx = std::max(0, (int)(std::upper_bound(o.begin(), o.end(), rb_max) - o.begin()) - 1);
    } else if (rb_max > cmd_hi) {
        act.invalidate_cpu(primary);
    }
}

/**
 * Restores the elasticity statistics saved by an earlier session.
 */
void Impl::model_load() {
    FILE *fp = fopen(cfg.model_path.c_str(), "r");
    if (!fp)
        return;
    char line[256];
    while (fgets(line, sizeof(line), fp)) {
        int pi;
        double n, sx, sy, sxx, sxy, syy;
        unsigned long long t;
        if (sscanf(line, "v2 %d %lf %lf %lf %lf %lf %lf", &pi, &n, &sx, &sy, &sxx, &sxy, &syy) == 7) {
            if (pi < 0 || (size_t)pi >= el.size() || n <= 0.0)
                continue;
            const double c = n > (double)kEpsKeep ? (double)kEpsKeep / n : 1.0;
            el[(size_t)pi].n = n * c;
            el[(size_t)pi].sx = sx * c;
            el[(size_t)pi].sy = sy * c;
            el[(size_t)pi].sxx = sxx * c;
            el[(size_t)pi].sxy = sxy * c;
            el[(size_t)pi].syy = syy * c;
        } else if (sscanf(line, "v2t %llu", &t) == 1) {
            trials_prior = t;
        } else if (sscanf(line, "v3c %d %lf %lf", &pi, &n, &sx) == 3) {
            if (pi >= 0 && pi < margin::RiskCurve::kBins && n >= 0.0 && sx >= 0.0) {
                const double c = sx > margin::RiskCurve::kKeepWeight ? margin::RiskCurve::kKeepWeight / sx : 1.0;
                curve.a[pi] = n * c;
                curve.w[pi] = sx * c;
            }
        }
    }
    fclose(fp);
}

/**
 * Persists the elasticity statistics so learning accumulates across sessions.
 */
void Impl::model_save() {
    if (cfg.model_path.empty() || cfg.observe_only)
        return;
    FILE *fp = fopen(cfg.model_path.c_str(), "w");
    if (!fp)
        return;
    for (size_t i = 0; i < el.size(); i++) {
        if (el[i].n > 1.0)
            fprintf(fp, "v2 %d %.9g %.9g %.9g %.9g %.9g %.9g\n", (int)i, el[i].n, el[i].sx, el[i].sy, el[i].sxx, el[i].sxy, el[i].syy);
    }
    fprintf(fp, "v2t %llu\n", (unsigned long long)(trials_prior + sum.trials));
    for (int i = 0; i < margin::RiskCurve::kBins; i++)
        fprintf(fp, "v3c %d %.9g %.9g\n", i, curve.a[i], curve.w[i]);
    fclose(fp);
}

/**
 * Lowest OPP of the primary policy whose predicted margin reaches the mode
 * target. Needs q_now, m_now and f_now from decide(). The direction of the move
 * does not depend on eps; eps only sets its size, so a conservative bound is
 * used on each side: the upper bound when lowering, the lower bound when raising.
 *
 * @param top_i Highest reachable OPP index.
 * @return Target OPP index.
 */
int Impl::choose_target_margin(int top_i) {
    const std::vector<uint32_t> &o = act.policies()[(size_t)primary].opp_khz;
    const margin::Elasticity &e = el[(size_t)primary];
    const double mu = e.mean(), sd = e.sd();
    const double z = (double)mp->zb;
    const double eps_down = std::min(margin::Elasticity::kMax, mu + z * sd);
    const double eps_up = std::max(0.1, mu - z * sd);
    const double fnow = f_now > 0.0f ? (double)f_now : (double)o[(size_t)std::min(idx, top_i)];
    const double room = (double)(t_ref - gmin_cached);
    bool saturated = false;
    const double r = margin::freq_ratio((double)q_now, room, (double)m_star_now, eps_down, eps_up, saturated);
    if (saturated)
        return top_i;
    const double fstar = fnow * r;
    if (fstar >= (double)o[(size_t)top_i])
        return top_i;
    if (fstar <= (double)o.front())
        return 0;
    return std::min(top_i, ceil_idx((uint32_t)std::ceil(fstar)));
}

/**
 * Folds one finished A/B block pair into the elasticity posterior. The contrast
 * is the change in mean ln(busy) against the change in ln(frequency); the
 * trial pins the primary policy so the frequency change is exact.
 *
 * @param idx_b OPP index used in block B.
 */
void Impl::finish_trial(int idx_b) {
    if (trA_n < 12 || trB_n < 12)
        return;
    const std::vector<uint32_t> &o = act.policies()[(size_t)primary].opp_khz;
    const double fa = o[(size_t)trial_idx_a], fb = o[(size_t)idx_b];
    if (fa == fb)
        return;
    double y = trB_lb / (double)trB_n - trA_lb / (double)trA_n;
    y = std::max(-kContrastClip, std::min(kContrastClip, y));
    el[(size_t)primary].update(std::log(fb / fa), y);
    sum.trials++;
}

/**
 * Advances the paired A/B experiment: block A at the operating point, block B
 * a few OPPs away, then back. Bad frames are counted in on_frame().
 *
 * @param fail True if a severe failure was detected.
 * @param top_i Highest reachable OPP index.
 */
void Impl::run_trial(bool fail, int top_i) {
    if (fail) {
        idx = std::min(trial_home, top_i);
        trial = Trial::Idle;
        cooldown_fail = 3;
        win_since_trial = 0;
        apply(false);
        return;
    }
    const uint64_t done = frames_total - stage_start_frame;
    switch (trial) {
        case Trial::SettleA:
            if (done >= (uint64_t)kSettleFrames) {
                trial = Trial::MeasA;
                stage_start_frame = frames_total;
                trA_n = 0;
                trA_lb = 0.0;
            }
            break;
        case Trial::MeasA:
            if (done >= (uint64_t)kMeasureFrames) {
                idx = std::min(top_i, std::max(0, trial_idx_a + trial_dir));
                trial = Trial::SettleB;
                stage_start_frame = frames_total;
            }
            break;
        case Trial::SettleB:
            if (done >= (uint64_t)kSettleFrames) {
                trial = Trial::MeasB;
                stage_start_frame = frames_total;
                trB_n = 0;
                trB_lb = 0.0;
            }
            break;
        case Trial::MeasB:
            if (done >= (uint64_t)kMeasureFrames) {
                finish_trial(idx);
                idx = std::min(trial_home, top_i);
                trial = Trial::Idle;
                win_since_trial = 0;
                apply(false);
                return;
            }
            break;
        case Trial::Idle:
            return;
    }
    apply(true);
}

/**
 * Consumes one frame.
 */
void Impl::on_frame(const fas_frame &fr, uint64_t now) {
    if (!(fr.flags & FAS_FRAME_HAS_Q2Q))
        return;
    if (last_frame_ns && now - last_frame_ns > kResetNs)
        reset_session();
    last_frame_ns = now;
    idle = false;
    tid = fr.tid;
    if (fr.flags & FAS_FRAME_RESYNC)
        return;
    const float qf = ms(fr.q2q_ns);
    if (qf > kStallMs) {
        sum.stalls++;
        return;
    }

    const bool dq = (fr.flags & FAS_FRAME_HAS_DEQUEUE) != 0;
    const bool sc = (fr.flags & FAS_FRAME_HAS_SCHED) != 0;
    dqf += 0.02f * ((dq ? 1.0f : 0.0f) - dqf);

    q.push(t_ref > 0.0f ? std::min(qf, 4.0f * t_ref) : qf);
    const int bin = std::min(kBins - 1, (int)(qf / kBinMs));
    const uint32_t slot = pace_head & (kRing - 1);
    if (pace_n == kRing)
        hist[pace_bins[slot]]--;
    else
        pace_n++;
    pace_bins[slot] = (uint8_t)std::min(255, bin);
    hist[bin]++;
    pace_head++;

    bool have_busy = false;
    float lnb = 0.0f, busy_f = 0.0f, gap_f = 0.0f;
    if (dq) {
        const float wait = ms(fr.dequeue_wait_ns);
        const float hold = ms(fr.hold_ns);
        const float delay = sc ? ms(fr.delay_delta_ns) : 0.0f;
        const float b = std::max(hold - delay, 0.05f);
        busy.push(b);
        busy_f = b;
        gap_f = std::max(qf - wait - hold, 0.0f);
        gap.push(gap_f);
        ovh.push(wait + delay);
        have_busy = true;
        lnb = std::log(std::max(b, kLnMin));
        if (!lvl_init) {
            lvl = b;
            lvl_init = true;
        }
        resid.push(b - lvl);
        lvl += kLvlAlpha * (std::min(b, 3.0f * lvl) - lvl);
    }

    loc = loc <= 0.0f ? qf : loc + (qf > loc ? kLocStep : -kLocStep) * loc;
    const bool bad = qf > kBadK * loc;

    frames_total++;
    frames_since_reset++;
    sum.frames++;
    sum_q_ms += qf;
    if (t_ref > 0.0f && frames_since_reset > (uint64_t)kWarmupFrames) {
        scored_frames++;
        if (qf > 1.25f * t_ref)
            sum.misses++;
        if (settle_left > 0) {
            settle_left--;
        } else if (primary >= 0) {
            if (trial == Trial::MeasA) {
                if (have_busy) {
                    trA_n++;
                    trA_lb += (double)lnb;
                }
            } else if (trial == Trial::MeasB) {
                if (have_busy) {
                    trB_n++;
                    trB_lb += (double)lnb;
                }
            } else if (trial == Trial::Idle) {
                if (have_busy && u_valid) {
                    // A miss is addressable when the render thread's excess time over its
                    // recent median exceeds the gap-side excess; only those depend on frequency.
                    const bool addressable = qf > kBadK * t_ref && std::max(busy_f - bmed, 0.0f) > std::max(gap_f - gmed, 0.0f);
                    curve.update((double)u_cnt, addressable);
                    n_counted++;
                    n_addr += addressable ? 1 : 0;
                    if (ustar_s >= 0.0f) {
                        const float dev = u_cnt - ustar_s;
                        u_var += (dev * dev - u_var) * (1.0f / 5000.0f);
                    }
                }
                b_short += ((bad ? 1.0f : 0.0f) - b_short) * (1.0f / 300.0f);
                b_long += ((bad ? 1.0f : 0.0f) - b_long) * (1.0f / 6000.0f);
            }
        }
    }

    if (gpu_lock_active && gpu_boost_khz) {
        act.gpu_lock(gpu_boost_khz);
        gpu_lock_until_ns = now + (uint64_t)(0.6f * (t_ref > 0.0f ? t_ref : 10.0f) * 1e6f);
    }

    if (++win_frames >= kWinFrames && now - last_decide_ns >= kWinMinNs)
        decide(now);
}

/**
 * One control step: choose the target OPP from the margin model, move
 * toward it with hysteresis, apply guards, and schedule paired experiments.
 */
void Impl::decide(uint64_t now) {
    const uint64_t dt = last_decide_ns ? now - last_decide_ns : 0;
    last_decide_ns = now;
    win_frames = 0;
    sum.windows++;
    if (frames_since_reset < (uint64_t)kWarmupFrames)
        return;
    update_pace();
    if (!pace_valid)
        return;
    pin_update(now);
    update_ref(now);
    if (loc <= 0.0f || std::fabs(loc - t_fast) > 0.3f * t_fast)
        loc = t_fast;
    select_primary();
    if (primary < 0)
        return;
    if (now - last_save_ns >= kSaveNs) {
        last_save_ns = now;
        model_save();
    }

    const std::vector<uint32_t> &opp = act.policies()[(size_t)primary].opp_khz;
    const int n_opp = (int)opp.size();
    const float T = t_ref;
    sample_diag(now);

    int n = q.copy_last(kQWin, tmp);
    int cnt = 0;
    for (int i = 0; i < n; i++) {
        if (tmp[i] > 1.25f * T)
            cnt++;
    }
    const float miss = (float)cnt / (float)n;
    std::sort(tmp, tmp + n);
    const float q50 = tmp[n / 2];
    const float q95 = tmp[(int)(0.95f * (float)(n - 1))];
    const float qr = q50 / T;
    if (qr0 <= 0.0f)
        qr0 = clampf(qr, 1.0f, 1.04f);

    const float trend = settle_left == 0 ? b_short - b_long : 0.0f;
    bool mild = qr > qr0 + 0.06f || trend > 0.05f;
    bool fail = qr > qr0 + 0.12f || trend > 0.10f;
    const char *fail_why = qr > qr0 + 0.12f ? "tail_q50" : "tail_bad";

    win_since_move++;
    win_since_down++;
    win_since_trial++;
    if (cooldown_up > 0)
        cooldown_up--;
    if (cooldown_fail > 0)
        cooldown_fail--;
    if (!mild && win_since_move >= 10)
        qr0 = clampf(qr0 + 0.01f * (qr - qr0), 1.0f, 1.04f);

    const bool l1 = dqf > 0.8f && busy.count >= kQWin;
    float gmin = 0.0f, busy_eff = 0.0f, margin = 0.0f, h = 0.0f, ratio = 0.0f;
    if (l1) {
        n = busy.copy_last(kQWin, tmp);
        busy_eff = quantile(tmp, n, mp->bq);
        bmed = quantile(tmp, n, 0.5f);
        n = gap.copy_last(kGapWin, tmp);
        const float gp5 = quantile(tmp, n, 0.05f);
        gmed = quantile(tmp, n, 0.5f);
        gfloor = gfloor > 0.0f ? std::min(gfloor * 1.002f, gp5) : gp5;
        gmin = gfloor;
        gmin_cached = gmin;
        n = ovh.copy_last(kQWin, tmp);
        const float fixed = gmin + quantile(tmp, n, 0.5f);
        margin = 0.06f * T;
        h = T - fixed - busy_eff;
        ratio = busy_eff / std::max(0.25f * T, T - fixed - margin);
    }

    const int top_i = std::min(n_opp - 1, cap_idx);
    bool have_margin = false;
    q_now = 0.0f;
    ex_pp = 0.0f;
    if (l1 && lvl_init && resid.count >= kMinResid && T - gmin > 0.5f) {
        n = resid.copy_last(kRing, tmp2);
        q_now = std::max(0.1f, lvl + quantile(tmp2, n, kTau));
        m_now = (T - gmin) - q_now;
        const float sigma_u = clampf(std::sqrt(u_var), 0.03f, 0.25f);
        const float budget = cfg.miss_budget > 0.0f ? cfg.miss_budget : mp->budget;
        const float ustar = (float)curve.star((double)budget, (double)sigma_u);
        ustar_s = ustar_s < 0.0f ? ustar : ustar_s + 0.1f * (ustar - ustar_s);
        m_star_now = ustar_s * T;
        sum_mstar += (double)m_star_now;
        ex_pp = 100.0f * (float)curve.risk((double)(m_now / T));
        sum_margin += (double)m_now;
        n_margin++;
        have_margin = true;
    }
    // Lateness that is not addressable by frequency (gap-side stalls) must not
    // ratchet the floor or block downward moves.
    if (have_margin && m_now >= m_star_now + kPressureMs) {
        mild = false;
        fail = false;
    }
    int tgt = idx;
    if (have_margin)
        tgt = choose_target_margin(top_i);
    tgt = std::max(tgt, std::min(floor_idx, top_i));
    tgt_idx = tgt;

    if (capped) {
        trial = Trial::Idle;
        win_since_trial = 0;
        healthy_cnt = 0;
        idx = std::min(idx, cap_idx);
        apply(false);
        log_row(now, "hold", "capped", q50, q95, miss, gmin, busy_eff, h, margin, 0.0f);
        w_lo += (double)cmd_lo * (double)dt;
        w_hi += (double)cmd_hi * (double)dt;
        w_dt += (double)dt;
        return;
    }

    if (trial != Trial::Idle) {
        run_trial(fail, top_i);
        log_row(now, "dither", "trial", q50, q95, miss, gmin, busy_eff, h, margin, 0.0f);
        w_lo += (double)cmd_lo * (double)dt;
        w_hi += (double)cmd_hi * (double)dt;
        w_dt += (double)dt;
        return;
    }

    int want = idx;
    const char *reason = "hold";
    const int dwell = mp->dwell * dwell_mult;
    if (idx < tgt) {
        if (cooldown_up == 0) {
            want = tgt;
            reason = "target_up";
        }
        healthy_cnt = 0;
    } else if (idx > tgt) {
        if (!mild) {
            if (++healthy_cnt >= dwell) {
                want = std::max(tgt, idx - (have_margin && idx - tgt >= 3 ? 2 : 1));
                healthy_cnt = 0;
                reason = "target_down";
            }
        } else {
            healthy_cnt = 0;
        }
    } else {
        healthy_cnt = 0;
    }

    if (l1) {
        if (!have_margin && ratio > 1.0f && cooldown_up == 0) {
            want = std::max(want, idx + (ratio > 1.15f ? 2 : 1));
            reason = "ff_up";
            healthy_cnt = 0;
        }
        int over = 0;
        const int k = busy.copy_last(16, tmp);
        for (int i = 0; i < k; i++) {
            if (tmp[i] > T - gmin)
                over++;
        }
        if (over >= 8 && cooldown_up == 0) {
            want = std::max(want, idx + 2);
            reason = "storm";
        }
    }

    if (fail) {
        fail_streak++;
        if (cooldown_fail == 0) {
            want = std::max(want, idx + 1);
            floor_idx = std::min(n_opp - 1, std::max(floor_idx, idx + 1));
            cooldown_fail = 3;
            reason = fail_why;
            if (win_since_down < 40)
                dwell_mult = std::min(dwell_mult * 2, 8);
        }
        healthy_cnt = 0;
        guard_age = 0;
    } else {
        fail_streak = 0;
        if (mild) {
            guard_age = 0;
        } else if (++guard_age >= (have_margin && m_now >= m_star_now + 0.5f ? kGuardAgeFast : kGuardAgeWindows)) {
            guard_age = 0;
            floor_idx = std::max(0, floor_idx - 1);
        }
        if (++healthy_total >= 200) {
            healthy_total = 0;
            dwell_mult = std::max(1, dwell_mult / 2);
        }
    }

    want = std::min(n_opp - 1, std::max(0, want));
    if (fail_streak > 60 && idx == n_opp - 1) {
        futile_until_ns = now + 20000000000ull;
        env_n = 0;
        fail_streak = 0;
        reason = "futile";
    }

    const char *action = "hold";
    if (want > idx) {
        sum.moves_up++;
        action = "up";
        cooldown_up = 2;
        win_since_move = 0;
    } else if (want < idx) {
        sum.moves_down++;
        action = "down";
        win_since_move = 0;
        win_since_down = 0;
    }
    idx = want;

    const margin::Elasticity &ee = el[(size_t)primary];
    eps_last = (float)ee.mean();
    eps_sd_last = (float)ee.sd();
    const double cv = ee.sd() / std::max(ee.mean(), 0.1);
    const int period = cv > 0.6 ? 60 : (cv > 0.35 ? 300 : 3000);
    if (!cfg.observe_only && !mild && settle_left == 0 && idx == tgt && win_since_move >= 20 && win_since_trial >= period && cooldown_fail == 0 && cooldown_up == 0) {
        trial_flip = !trial_flip;
        int dir = trial_flip ? kDither : -kDither;
        if (have_margin && m_now < 1.0f)
            dir = kDither;
        if (idx + dir < 0 || idx + dir > top_i)
            dir = -dir;
        if (idx + dir >= 0 && idx + dir <= top_i && dir != 0) {
            trial = Trial::SettleA;
            trial_home = idx;
            trial_idx_a = idx;
            trial_dir = dir;
            stage_start_frame = frames_total;
            action = "dither";
            reason = "start";
        }
    }

    u_valid = false;
    if (have_margin && trial == Trial::Idle && primary >= 0) {
        const std::vector<uint32_t> &o = act.policies()[(size_t)primary].opp_khz;
        const int lo_i = std::max(0, idx - mp->down), hi_i = std::min(n_opp - 1, idx + mp->up);
        const double fnow = f_now > 0.0f ? (double)f_now : (double)o[(size_t)idx];
        const double fexp = std::min((double)o[(size_t)hi_i], std::max((double)o[(size_t)lo_i], fnow));
        const double m_exp = (double)(T - gmin) - (double)q_now * std::pow(fnow / fexp, el[(size_t)primary].mean());
        u_cnt = (float)(m_exp / (double)T);
        u_valid = true;
    }
    apply(trial != Trial::Idle);
    gpu_step(now, fail, idx == n_opp - 1, !mild && healthy_cnt >= mp->dwell);
    log_row(now, action, reason, q50, q95, miss, gmin, busy_eff, h, margin, 0.0f);
    w_lo += (double)cmd_lo * (double)dt;
    w_hi += (double)cmd_hi * (double)dt;
    w_dt += (double)dt;
}

/**
 * Optional GPU policy. Adreno: cap the ceiling at the observed DVFS demand
 * and lift the floor when the CPU lever is exhausted. MediaTek: frame
 * synchronous lock pulses while boosting.
 */
void Impl::gpu_step(uint64_t now, bool fail, bool exhausted, bool comfortable) {
    (void)now;
    if (!cfg.gpu || cfg.observe_only || act.gpu_kind() == GpuKind::None)
        return;
    const std::vector<uint32_t> &o = act.gpu_opp_khz();
    if (o.size() < 2)
        return;
    const uint32_t cur = act.gpu_cur_khz();
    if (cur)
        gpu_hist.push((float)cur);
    const bool adreno = act.gpu_kind() == GpuKind::Adreno;
    const int top = (int)o.size() - 1;
    auto ceil_gpu = [&](float khz) {
        int i = (int)(std::lower_bound(o.begin(), o.end(), (uint32_t)khz) - o.begin());
        return i > top ? top : i;
    };

    if (fail && exhausted) {
        if (++gpu_pressure >= 3 && gpu_boost_left == 0 && cur) {
            gpu_boost_khz = o[(size_t)std::min(top, ceil_gpu((float)cur) + 1)];
            gpu_boost_left = 10;
            if (adreno) {
                act.set_gpu_band(gpu_boost_khz, o.back());
                gpu_cap_active = false;
            } else {
                gpu_lock_active = true;
            }
        }
        return;
    }
    gpu_pressure = 0;
    if (gpu_boost_left > 0 && --gpu_boost_left == 0) {
        if (adreno)
            act.set_gpu_band(o.front(), o.back());
        else {
            gpu_lock_active = false;
            act.gpu_unlock();
        }
    }
    if (adreno && comfortable && gpu_boost_left == 0 && gpu_hist.count >= 32) {
        const int c = gpu_hist.copy_last(64, tmp);
        const float p95 = quantile(tmp, c, 0.95f);
        const int cap = std::min(top, ceil_gpu(p95) + 1);
        act.set_gpu_band(o.front(), o[(size_t)cap]);
        gpu_cap_active = true;
    }
}

/**
 * Releases the GPU lock pulse and detects a stalled frame stream.
 */
void Impl::tick(uint64_t now) {
    if (gpu_lock_active && gpu_lock_until_ns && now >= gpu_lock_until_ns) {
        act.gpu_unlock();
        gpu_lock_until_ns = 0;
    }
    if (!idle && last_frame_ns && now - last_frame_ns > kIdleNs) {
        idle = true;
        settle_left = kWarmFrames;
        release_all();
        if (cfg.log) {
            fprintf(cfg.log, "%llu,%d,idle", (unsigned long long)now, tid);
            for (int i = 0; i < 21; i++)
                fputc(',', cfg.log);
            fputs("idle,release,no_frames,,,,,,\n", cfg.log);
        }
    }
}

void Impl::log_header() {
    fputs("ts_ns,tid,level,pace_ms,ref_ms,pinned,q50_ms,q95_ms,miss,gap_floor_ms,busy_p80_ms,headroom_ms,margin_ms,sigma_ms,eps,eps_se,trials,policy,idx,lo_khz,hi_khz,gpu_cur_khz,gpu_cap,state,action,reason,floor_idx,r_ref,temp_mc,rb_min_khz,rb_max_khz,cur_khz,risk_margin_ms,q_busy_ms,fnow_khz,paddr_pp,mstar_ms,tgt_idx,b_short\n", cfg.log);
}

/**
 * Appends one decision row to the scheduler log. eps and eps_se are the
 * posterior of the frequency elasticity; margin_ms is T - gap_floor - Q at the
 * current frequency; r_ref is the long-run bad frame rate and floor_idx the
 * guard floor.
 */
void Impl::log_row(uint64_t now, const char *action, const char *reason, float q50, float q95, float miss, float gmin, float busy_eff, float h, float margin, float sigma) {
    if (!cfg.log)
        return;
    const bool l1 = dqf > 0.8f && busy.count >= kQWin;
    fprintf(cfg.log, "%llu,%d,%s,%.3f,%.3f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llu,%d,%d,%u,%u,%u,%d,%s,%s,%s,%d,%.4f,%d,%u,%u,%u,%.2f,%.2f,%.2f,%.2f,%.2f,%d,%.4f\n", (unsigned long long)now, tid, l1 ? "L1" : "L0", t_fast, t_ref, pinned, q50, q95, miss, gmin, busy_eff, h, margin, sigma, primary >= 0 ? (float)el[(size_t)primary].mean() : 0.0f, primary >= 0 ? (float)el[(size_t)primary].sd() : 0.0f, (unsigned long long)(trials_prior + sum.trials), primary >= 0 ? act.policies()[(size_t)primary].id : -1, idx, cmd_lo, cmd_hi, cfg.gpu ? act.gpu_cur_khz() : 0u, gpu_cap_active ? 1 : 0, capped ? "capped" : (trial == Trial::Idle ? "run" : "trial"), action, reason, floor_idx, b_long, temp_mc, rb_min, rb_max, cur_khz, m_now, q_now, f_now, ex_pp, m_star_now, tgt_idx, b_short);
}

} // namespace detail

Scheduler::Scheduler(Actuator &act, const Config &cfg) : p_(new detail::Impl(act, cfg)) {}

Scheduler::~Scheduler() {
    delete p_;
}

void Scheduler::on_frame(const fas_frame &fr, uint64_t now_ns) {
    p_->on_frame(fr, now_ns);
}

void Scheduler::tick(uint64_t now_ns) {
    p_->tick(now_ns);
}

int Scheduler::next_timeout_ms() const {
    return p_->gpu_lock_active ? 2 : 200;
}

void Scheduler::shutdown() {
    p_->model_save();
    p_->release_all();
}

Summary Scheduler::summary() const {
    Summary s = p_->sum;
    s.writes = p_->act.write_count();
    s.mean_fps = p_->sum.frames ? 1000.0 * (double)p_->sum.frames / p_->sum_q_ms : 0.0;
    s.mean_lo_khz = p_->w_dt > 0.0 ? p_->w_lo / p_->w_dt : 0.0;
    s.mean_hi_khz = p_->w_dt > 0.0 ? p_->w_hi / p_->w_dt : 0.0;
    s.eps = p_->eps_last;
    s.eps_sd = p_->eps_sd_last;
    {
        double h[margin::RiskCurve::kBins];
        p_->curve.posterior(h);
        for (int i = 0; i < margin::RiskCurve::kBins; i++) {
            s.risk_pp[i] = (float)(100.0 * h[i]);
            s.risk_w[i] = (float)p_->curve.w[i];
        }
        s.addr_pct = p_->n_counted ? (float)(100.0 * (double)p_->n_addr / (double)p_->n_counted) : 0.0f;
        s.mstar_ms = p_->n_margin ? (float)(p_->sum_mstar / (double)p_->n_margin) : 0.0f;
    }
    s.mean_margin = p_->n_margin ? (float)(p_->sum_margin / (double)p_->n_margin) : 0.0f;
    if (p_->scored_frames == 0)
        s.misses = 0;
    return s;
}

} // namespace fsched
