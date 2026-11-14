// SPDX-License-Identifier: BSD-2-Clause

#ifndef FAS_MARGIN_HPP
#define FAS_MARGIN_HPP

#include <algorithm>
#include <cmath>

/**
 * Margin model used by the scheduler.
 *
 * Demand:   ln busy = level - eps * ln(f / f_ref) + noise   (busy = hold - delay)
 * Margin:   m(f)    = T - g - Q * (f_now / f)^eps           (ms; T pace, g gap floor,
 *                                                            Q forecast busy quantile)
 * Risk:     P(addressable miss | m / T), learned per device by RiskCurve. A miss is
 *           addressable when the render thread's excess time dominates the gap-side
 *           excess; gap-side stalls do not depend on frequency and are not modelled.
 *
 * The sign of "move up or down" does not depend on eps, only the step size does.
 */
namespace fsched {
namespace margin {

/**
 * Probability that a frame is a render-side miss as a function of margin / T,
 * learned per device from the frames the scheduler itself observes.
 *
 * Ten bins over margin / T with a prior measured on one Poco F5 session. Each
 * frame adds weight kFrameWeight to its bin; all counts decay with time constant
 * kTimeConstFrames so the curve follows the game and the scene. The posterior of
 * a bin is the prior-weighted mean, made non-increasing in margin by pooling
 * adjacent violators.
 */
class RiskCurve {
public:
    static constexpr int kBins = 10;
    static constexpr double kEdge[kBins - 1] = {-0.10, -0.05, 0.0, 0.05, 0.10, 0.15, 0.20, 0.30, 0.45};
    static constexpr double kCenter[kBins] = {-0.15, -0.075, -0.025, 0.025, 0.075, 0.125, 0.175, 0.25, 0.375, 0.55};
    static constexpr double kPrior[kBins] = {0.27, 0.17, 0.047, 0.022, 0.0147, 0.0084, 0.0054, 0.0038, 0.0031, 0.0030};
    static constexpr double kPriorN = 150.0;
    static constexpr double kFrameWeight = 0.5;
    static constexpr double kTimeConstFrames = 20000.0;
    static constexpr double kUMin = 0.0;
    static constexpr double kUMax = 0.60;
    static constexpr double kKeepWeight = 3000.0;

    double a[kBins] = {};
    double w[kBins] = {};

    /** @return Bin index of margin / T. */
    static int bin(double u) {
        int b = 0;
        while (b < kBins - 1 && u >= kEdge[b])
            b++;
        return b;
    }

    /**
     * Adds one frame.
     *
     * @param u Margin / T the frame ran under.
     * @param addressable True when the frame missed and the render thread caused it.
     */
    void update(double u, bool addressable) {
        const double d = 1.0 - 1.0 / kTimeConstFrames;
        for (int i = 0; i < kBins; i++) {
            a[i] *= d;
            w[i] *= d;
        }
        const int b = bin(u);
        a[b] += addressable ? kFrameWeight : 0.0;
        w[b] += kFrameWeight;
    }

    /**
     * Posterior mean per bin, non-increasing in margin.
     *
     * @param h Output, kBins values.
     */
    void posterior(double h[kBins]) const {
        double val[kBins], wt[kBins];
        int cnt[kBins];
        int m = 0;
        for (int i = 0; i < kBins; i++) {
            val[m] = (a[i] + kPriorN * kPrior[i]) / (w[i] + kPriorN);
            wt[m] = w[i] + kPriorN;
            cnt[m] = 1;
            m++;
            while (m > 1 && val[m - 2] < val[m - 1]) {
                const double tw = wt[m - 2] + wt[m - 1];
                val[m - 2] = (val[m - 2] * wt[m - 2] + val[m - 1] * wt[m - 1]) / tw;
                wt[m - 2] = tw;
                cnt[m - 2] += cnt[m - 1];
                m--;
            }
        }
        int k = 0;
        for (int g = 0; g < m; g++) {
            for (int j = 0; j < cnt[g]; j++)
                h[k++] = val[g];
        }
    }

    /**
     * @param u Margin / T.
     * @return Posterior probability of a render-side miss at that margin.
     */
    double risk(double u) const {
        double h[kBins];
        posterior(h);
        return h[bin(u)];
    }

    /**
     * @param z Standard score.
     * @return Standard normal CDF at z.
     */
    static double phi(double z) {
        return 0.5 * std::erfc(-z / 1.4142135623730951);
    }

    /**
     * Expected risk when the margin of the frames scatters around a target. The
     * risk curve is convex in margin, so the average over the scatter is higher
     * than the risk at the target itself.
     *
     * @param h Posterior per bin.
     * @param u Target margin / T.
     * @param sigma Standard deviation of the frames' margin / T around the target.
     */
    static double smoothed(const double h[kBins], double u, double sigma) {
        double acc = 0.0, lower = 0.0;
        for (int b = 0; b < kBins; b++) {
            const double upper = b == kBins - 1 ? 1.0 : phi((kEdge[b] - u) / sigma);
            acc += h[b] * (upper - lower);
            lower = upper;
        }
        return acc;
    }

    /**
     * Smallest target margin / T whose expected risk, averaged over the scatter of
     * the margin around it, is within the budget. The budget is never set below 1.5
     * times the risk of the top bin, which is the floor the classification noise
     * leaves.
     *
     * @param budget Accepted probability of a render-side miss per frame.
     * @param sigma Scatter of the frames' margin / T around the target.
     * @return Target margin / T in [kUMin, kUMax].
     */
    double star(double budget, double sigma) const {
        double h[kBins];
        posterior(h);
        const double bud = std::max(budget, 1.5 * h[kBins - 1]);
        constexpr double kStep = 0.005;
        double prev_u = kUMax, prev_r = smoothed(h, kUMax, sigma);
        if (prev_r > bud)
            return kUMax;
        for (double u = kUMax - kStep; u >= kUMin - 1e-9; u -= kStep) {
            const double r = smoothed(h, u, sigma);
            if (r > bud) {
                const double t = (bud - prev_r) / std::max(r - prev_r, 1e-12);
                return std::min(kUMax, std::max(kUMin, prev_u + t * (u - prev_u)));
            }
            prev_u = u;
            prev_r = r;
        }
        return kUMin;
    }
};

/**
 * Posterior of the frequency elasticity eps from paired block contrasts.
 *
 * Each contrast is y = mean ln(busy) in block B minus block A against
 * x = ln(f_B / f_A). The model is y = c - eps * x with an unknown intercept c
 * that absorbs slow drift of the workload. Statistics decay exponentially so
 * the estimate follows scene changes.
 */
class Elasticity {
public:
    static constexpr double kPriorMu = 0.5;
    static constexpr double kPriorSd = 0.4;
    static constexpr double kSigmaY = 0.08;
    static constexpr double kDecay = 0.995;
    static constexpr double kMin = 0.05;
    static constexpr double kMax = 1.5;

    double n = 0.0, sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, syy = 0.0;

    /**
     * Folds one contrast into the statistics.
     *
     * @param dlnf ln(f_B / f_A), must be non-zero.
     * @param dlnb Mean ln(busy) of block B minus block A.
     */
    void update(double dlnf, double dlnb) {
        n = n * kDecay + 1.0;
        sx = sx * kDecay + dlnf;
        sy = sy * kDecay + dlnb;
        sxx = sxx * kDecay + dlnf * dlnf;
        sxy = sxy * kDecay + dlnf * dlnb;
        syy = syy * kDecay + dlnb * dlnb;
    }

    /**
     * @return Contrast noise in ln units: the default, or the residual estimate
     *         when the data are noisier than assumed (never smaller than the default).
     */
    double sigma() const {
        if (n < 8.0)
            return kSigmaY;
        const double sxx_c = sxx - sx * sx / n;
        if (sxx_c <= 1e-9)
            return kSigmaY;
        const double syy_c = syy - sy * sy / n;
        const double sxy_c = sxy - sx * sy / n;
        const double rss = std::max(0.0, syy_c - sxy_c * sxy_c / sxx_c);
        return std::max(kSigmaY, std::sqrt(rss / (n - 2.0)));
    }

    /** @return Precision contributed by the data (1 / variance). */
    double info() const {
        if (n < 2.0)
            return 0.0;
        const double sxx_c = sxx - sx * sx / n;
        const double sg = sigma();
        return sxx_c > 1e-9 ? sxx_c / (sg * sg) : 0.0;
    }

    /** @return Posterior mean of eps, clamped to [kMin, kMax]. */
    double mean() const {
        const double p0 = 1.0 / (kPriorSd * kPriorSd);
        const double I = info();
        double hat = kPriorMu;
        if (I > 0.0) {
            const double sxx_c = sxx - sx * sx / n;
            hat = -(sxy - sx * sy / n) / sxx_c;
        }
        return std::min(kMax, std::max(kMin, (I * hat + p0 * kPriorMu) / (I + p0)));
    }

    /** @return Posterior standard deviation of eps. */
    double sd() const {
        return 1.0 / std::sqrt(info() + 1.0 / (kPriorSd * kPriorSd));
    }
};

/**
 * Frequency ratio f* / f_now at which the margin equals m_star.
 *
 * @param q Forecast busy quantile at f_now, in ms.
 * @param room T - g, in ms.
 * @param m_star Margin the mode asks for, in ms.
 * @param eps_down Elasticity assumed when lowering frequency (use an upper bound).
 * @param eps_up Elasticity assumed when raising frequency (use a lower bound).
 * @param saturated Set when no finite ratio satisfies the target.
 * @return Ratio, below 1 when there is spare margin.
 */
inline double freq_ratio(double q, double room, double m_star, double eps_down, double eps_up, bool &saturated) {
    const double denom = room - m_star;
    saturated = denom < 0.2 * room || denom <= 0.0;
    if (saturated)
        return 1.0e3;
    const double x = q / denom;
    return std::pow(x, 1.0 / (x < 1.0 ? eps_down : eps_up));
}

} // namespace margin
} // namespace fsched

#endif
