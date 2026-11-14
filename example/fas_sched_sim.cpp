// SPDX-License-Identifier: BSD-2-Clause

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <unistd.h>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "fas_sched.hpp"

using namespace fsched;

namespace {

struct Row {
    uint64_t ts;
    float q, wait, hold, delay;
    std::vector<uint32_t> f;
};

/**
 * Mock hardware that records the commanded band of each policy.
 */
class MockActuator : public Actuator {
public:
    std::vector<Policy> pols;
    std::vector<uint32_t> lo, hi, cur;
    int primary_id = 4;
    uint64_t writes = 0;
    uint32_t ext_cap = 0;
    int primary_index = 0;

    const std::vector<Policy> &policies() const override { return pols; }
    int render_cpu(int) override { return primary_id; }
    uint32_t cpu_cur_khz(int pi) override { return cur[(size_t)pi]; }
    void set_cpu_band(int pi, uint32_t l, uint32_t h) override {
        if (lo[(size_t)pi] != l || hi[(size_t)pi] != h)
            writes++;
        lo[(size_t)pi] = l;
        hi[(size_t)pi] = h;
    }
    void restore_cpu(int pi) override {
        lo[(size_t)pi] = pols[(size_t)pi].orig_min_khz;
        hi[(size_t)pi] = pols[(size_t)pi].orig_max_khz;
    }
    uint64_t write_count() const override { return writes; }
    bool read_limits(int pi, uint32_t &mn, uint32_t &mx) override {
        eff((size_t)pi, mn, mx);
        return true;
    }
    void invalidate_cpu(int) override {}
    void eff(size_t pi, uint32_t &mn, uint32_t &mx) const {
        mx = hi[pi];
        mn = lo[pi];
        if (ext_cap && (int)pi == primary_index) {
            mx = std::min(mx, ext_cap);
            mn = std::min(mn, mx);
        }
    }
};

std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> out;
    std::string cur;
    std::stringstream ss(s);
    while (std::getline(ss, cur, ','))
        out.push_back(cur);
    if (!s.empty() && s.back() == ',')
        out.push_back("");
    return out;
}

float pct(std::vector<float> v, float p) {
    std::sort(v.begin(), v.end());
    return v[(size_t)(p * (float)(v.size() - 1))];
}

struct Result {
    double fps, p50, p95, p99, jank, mean_f, power;
};

Result summarize(const std::vector<float> &q, const std::vector<float> &lim, const std::vector<double> &f, double fmax) {
    Result r{};
    double sq = 0, jank = 0, mf = 0, pw = 0;
    for (size_t i = 0; i < q.size(); i++) {
        sq += q[i];
        if (q[i] > 1.25f * lim[i])
            jank++;
        mf += f[i];
        pw += std::pow(f[i] / fmax, 3.0);
    }
    r.fps = 1000.0 * (double)q.size() / sq;
    r.p50 = pct(q, 0.5f);
    r.p95 = pct(q, 0.95f);
    r.p99 = pct(q, 0.99f);
    r.jank = 100.0 * jank / (double)q.size();
    r.mean_f = mf / (double)q.size();
    r.power = pw / (double)q.size();
    return r;
}

/**
 * Extra spike probability as a function of effective frequency, measured on
 * a real device run (jank rate by band floor: 12.7%, 5.1%, 2.5%, 1.9%).
 */
double spike_prob(double khz) {
    static const double f[] = {0.0, 900e3, 1100e3, 1500e3, 1900e3, 1e9};
    static const double p[] = {0.127, 0.127, 0.051, 0.025, 0.019, 0.019};
    for (int i = 0; i < 5; i++) {
        if (khz <= f[i + 1])
            return p[i] + (p[i + 1] - p[i]) * (khz - f[i]) / (f[i + 1] - f[i]);
    }
    return p[5];
}

uint64_t g_rng = 0x9E3779B97F4A7C15ull;

double urand() {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (double)(g_rng >> 11) / 9007199254740992.0;
}

} // namespace

int main(int argc, char **argv) {
    const char *path = nullptr, *logp = nullptr, *oppp = nullptr;
    Config cfg;
    float eps_true = 0.6f;
    bool tail_model = false;
    bool drift = false;
    bool ext = false;
    bool load = false;
    int level = 2, primary = 4;
    int opt;
    while ((opt = getopt(argc, argv, "m:e:L:P:s:f:oTFO:S:DXWM:B:")) != -1) {
        switch (opt) {
            case 'm':
                cfg.mode = !strcmp(optarg, "powersave") ? Mode::Powersave : !strcmp(optarg, "performance") ? Mode::Performance : Mode::Balance;
                break;
            case 'B':
                cfg.miss_budget = (float)atof(optarg) / 100.0f;
                break;
            case 'e':
                eps_true = (float)atof(optarg);
                break;
            case 'L':
                level = atoi(optarg);
                break;
            case 'P':
                primary = atoi(optarg);
                break;
            case 's':
                logp = optarg;
                break;
            case 'f': {
                std::stringstream ss(optarg);
                std::string t;
                while (std::getline(ss, t, ','))
                    cfg.legal_fps.push_back(atoi(t.c_str()));
                break;
            }
            case 'o':
                cfg.observe_only = true;
                break;
            case 'S':
                g_rng ^= strtoull(optarg, nullptr, 10) * 0xD1B54A32D192ED03ull;
                for (int k = 0; k < 8; k++)
                    urand();
                break;
            case 'O':
                oppp = optarg;
                break;
            case 'D':
                drift = true;
                break;
            case 'X':
                ext = true;
                break;
            case 'W':
                load = true;
                break;
            case 'T':
                tail_model = true;
                break;
            case 'F':
                break;
            case 'M':
                cfg.model_path = optarg;
                break;
            default:
                fprintf(stderr, "usage: %s [-m mode] [-e eps_true] [-L 0|1|2] [-P policy] [-s log] [-f fps,..] [-o] trace.csv\n", argv[0]);
                return 2;
        }
    }
    if (optind >= argc)
        return 2;
    path = argv[optind];

    std::ifstream in(path);
    std::string line;
    std::getline(in, line);
    std::vector<std::string> hdr = split(line);
    std::vector<int> pcols, pids;
    for (size_t i = 0; i < hdr.size(); i++) {
        int id;
        if (sscanf(hdr[i].c_str(), "policy%d_khz", &id) == 1) {
            pcols.push_back((int)i);
            pids.push_back(id);
        }
    }
    std::vector<Row> rows;
    while (std::getline(in, line)) {
        std::vector<std::string> c = split(line);
        if (c.size() < 7 || c[2].empty() || c[3].empty() || c[4].empty())
            continue;
        Row r;
        r.ts = strtoull(c[0].c_str(), nullptr, 10);
        r.q = (float)strtod(c[2].c_str(), nullptr) * 1e-6f;
        r.wait = (float)strtod(c[3].c_str(), nullptr) * 1e-6f;
        r.hold = (float)strtod(c[4].c_str(), nullptr) * 1e-6f;
        r.delay = c[6].empty() ? 0.0f : (float)strtod(c[6].c_str(), nullptr) * 1e-6f;
        for (int pc : pcols)
            r.f.push_back((size_t)pc < c.size() && !c[(size_t)pc].empty() ? (uint32_t)strtoul(c[(size_t)pc].c_str(), nullptr, 10) : 0u);
        rows.push_back(r);
    }
    const size_t N = rows.size();
    int pidx = -1;
    for (size_t i = 0; i < pids.size(); i++) {
        if (pids[i] == primary)
            pidx = (int)i;
    }
    if (N < 1000 || pidx < 0) {
        fprintf(stderr, "bad trace or primary policy\n");
        return 1;
    }

    std::vector<Row> opp_rows;
    if (oppp) {
        std::ifstream oin(oppp);
        std::string ol;
        std::getline(oin, ol);
        std::vector<std::string> oh = split(ol);
        std::vector<int> ocol(pids.size(), -1);
        for (size_t i = 0; i < oh.size(); i++) {
            int id;
            if (sscanf(oh[i].c_str(), "policy%d_khz", &id) == 1) {
                for (size_t k = 0; k < pids.size(); k++) {
                    if (pids[k] == id)
                        ocol[k] = (int)i;
                }
            }
        }
        while (std::getline(oin, ol)) {
            std::vector<std::string> c = split(ol);
            Row r;
            for (size_t k = 0; k < pids.size(); k++)
                r.f.push_back(ocol[k] >= 0 && (size_t)ocol[k] < c.size() && !c[(size_t)ocol[k]].empty() ? (uint32_t)strtoul(c[(size_t)ocol[k]].c_str(), nullptr, 10) : 0u);
            opp_rows.push_back(r);
        }
    }
    const std::vector<Row> &opp_src = oppp ? opp_rows : rows;

    MockActuator mock;
    mock.primary_id = primary;
    for (size_t i = 0; i < pids.size(); i++) {
        std::map<uint32_t, int> cnt;
        for (const Row &r : opp_src)
            cnt[r.f[i]]++;
        Policy p;
        p.id = pids[i];
        p.cpus = {pids[i]};
        for (auto &kv : cnt) {
            if (kv.first && kv.second >= 20)
                p.opp_khz.push_back(kv.first);
        }
        p.orig_min_khz = p.opp_khz.front();
        p.orig_max_khz = p.opp_khz.back();
        mock.pols.push_back(p);
        mock.lo.push_back(p.orig_min_khz);
        mock.hi.push_back(p.orig_max_khz);
        mock.cur.push_back(p.opp_khz.back());
    }
    const double fmax = mock.pols[(size_t)pidx].opp_khz.back();
    mock.primary_index = pidx;

    std::vector<float> gmin_t(N), tlim_t(N);
    for (size_t i = 0; i < N; i += 25) {
        const size_t a = i > 150 ? i - 150 : 0, b = std::min(N, i + 175);
        std::vector<float> g;
        int hist[256] = {0};
        for (size_t k = a; k < b; k++) {
            g.push_back(std::max(0.0f, rows[k].q - rows[k].wait - rows[k].hold));
            hist[std::min(255, (int)(rows[k].q / 0.25f))]++;
        }
        const float gm = pct(g, 0.05f);
        int best = 1, bs = -1;
        for (int j = 1; j < 255; j++) {
            const int s = hist[j - 1] + hist[j] + hist[j + 1];
            if (s > bs) {
                bs = s;
                best = j;
            }
        }
        const float tl = ((float)best + 0.5f) * 0.25f;
        for (size_t k = i; k < std::min(N, i + 25); k++) {
            gmin_t[k] = gm;
            tlim_t[k] = tl;
        }
    }

    FILE *log = logp ? fopen(logp, "w") : nullptr;
    cfg.log = log;
    Scheduler sched(mock, cfg);

    std::vector<float> q_base, q_ctl, lim;
    std::vector<double> f_base, f_ctl;
    uint64_t ts_off = 0;
    std::vector<size_t> load_at = {0, N / 4, N / 2, (3 * N) / 4};
    for (size_t i = 0; i < N; i++) {
        const Row &r = rows[i];
        bool stutter = false;
        if (load) {
            for (size_t a : load_at) {
                if (i >= a && i < a + 2000)
                    stutter = true;
                if (i == a && a > 0) {
                    sched.tick(r.ts + ts_off + 500000000ull);
                    ts_off += 700000000ull;
                }
            }
        }
        for (size_t k = 0; k < mock.cur.size(); k++)
            mock.cur[k] = r.f[k] ? r.f[k] : mock.cur[k];
        const double fo = r.f[(size_t)pidx] ? r.f[(size_t)pidx] : fmax;
        if (ext) {
            const double ts = (double)(r.ts - rows[0].ts) * 1e-9;
            const double ph = std::fmod(ts, 150.0);
            static const uint32_t caps[4] = {1113600, 1324800, 1555200, 1766400};
            mock.ext_cap = (ts >= 100.0 && ph >= 100.0) ? caps[((int)(ts / 150.0)) & 3] : 0u;
        }
        double fe = fo;
        if (!cfg.observe_only) {
            uint32_t emn, emx;
            mock.eff((size_t)pidx, emn, emx);
            fe = std::min<double>(std::max<double>(fo, emn), emx);
        }

        mock.cur[(size_t)pidx] = (uint32_t)fe;
        const float busy = std::max(r.hold - r.delay, 0.05f);
        const float busy_n = busy * (float)std::pow(fo / fe, (double)eps_true);
        const float xo = gmin_t[i] + busy + r.delay + r.wait;
        const float xn = gmin_t[i] + busy_n + r.delay + r.wait;
        float qn = r.q + std::max(tlim_t[i], xn) - std::max(tlim_t[i], xo);
        if (tail_model && !cfg.observe_only) {
            const double dp = spike_prob(fe) - spike_prob(fo);
            if (dp > 0 && urand() < dp)
                qn += (float)(2.0 - 3.3 * std::log(1.0 - urand()));
        }
        const float hold_n = r.hold + (busy_n - busy);

        if (stutter && urand() < 0.25)
            qn += (float)(2.0 - 3.3 * std::log(1.0 - urand()));
        float r_q = r.q;
        if (drift) {
            const double u = (double)i / (double)N;
            const float k = 1.0f + 0.11f * (float)std::min(1.0, std::max(0.0, (u - 0.5) / 0.4));
            qn *= k;
            r_q *= k;
        }
        fas_frame fr;
        memset(&fr, 0, sizeof(fr));
        fr.ts_ns = r.ts + ts_off;
        fr.tid = 1;
        fr.flags = FAS_FRAME_HAS_Q2Q;
        fr.q2q_ns = (uint64_t)(qn * 1e6f);
        if (level >= 1) {
            fr.flags |= FAS_FRAME_HAS_DEQUEUE;
            fr.dequeue_wait_ns = (uint64_t)(r.wait * 1e6f);
            fr.hold_ns = (uint64_t)(hold_n * 1e6f);
        }
        if (level >= 2) {
            fr.flags |= FAS_FRAME_HAS_SCHED;
            fr.exec_delta_ns = (uint64_t)(busy_n * 0.7f * 1e6f);
            fr.delay_delta_ns = (uint64_t)(r.delay * 1e6f);
        }
        sched.on_frame(fr, r.ts + ts_off);

        q_base.push_back(r_q);
        q_ctl.push_back(qn);
        lim.push_back(tlim_t[i]);
        f_base.push_back(fo);
        f_ctl.push_back(fe);
    }
    sched.shutdown();
    Summary s = sched.summary();
    if (log)
        fclose(log);

    const Result b = summarize(q_base, lim, f_base, fmax);
    const Result c = summarize(q_ctl, lim, f_ctl, fmax);
    printf("trace=%zu frames primary=policy%d level=L%d eps_true=%.2f mode=%d%s\n", N, primary, level, eps_true, (int)cfg.mode, cfg.observe_only ? " observe" : "");
    printf("%-10s fps=%6.2f p50=%5.2f p95=%5.2f p99=%5.2f jank>1.25T=%5.2f%% meanF=%7.0f power(f^3)=%.3f\n", "governor", b.fps, b.p50, b.p95, b.p99, b.jank, b.mean_f, b.power);
    printf("%-10s fps=%6.2f p50=%5.2f p95=%5.2f p99=%5.2f jank>1.25T=%5.2f%% meanF=%7.0f power(f^3)=%.3f\n", "scheduler", c.fps, c.p50, c.p95, c.p99, c.jank, c.mean_f, c.power);
    printf("delta: fps %+.2f%%  meanF %+.1f%%  power %+.1f%%  jank %+.2fpp | up=%llu down=%llu trials=%llu writes=%llu eps=%.2f+-%.2f margin=%.2f\n", 100.0 * (c.fps / b.fps - 1.0), 100.0 * (c.mean_f / b.mean_f - 1.0), 100.0 * (c.power / b.power - 1.0), c.jank - b.jank, (unsigned long long)s.moves_up, (unsigned long long)s.moves_down, (unsigned long long)s.trials, (unsigned long long)s.writes, s.eps, s.eps_sd, s.mean_margin);
        printf("curve: P(render-side miss)%% by margin/T bin = %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f | mean target margin %.2f ms | measured render-side miss %.2f%%\n", s.risk_pp[0], s.risk_pp[1], s.risk_pp[2], s.risk_pp[3], s.risk_pp[4], s.risk_pp[5], s.risk_pp[6], s.risk_pp[7], s.risk_pp[8], s.risk_pp[9], s.mstar_ms, s.addr_pct);
    return 0;
}
