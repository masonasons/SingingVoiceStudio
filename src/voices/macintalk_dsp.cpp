#include "voices/macintalk_dsp.h"

#include <algorithm>
#include <cmath>

namespace svs {
namespace dsp {

namespace {
constexpr double kPi = 3.14159265358979323846;
//: YIN's threshold on the normalised difference: lower is stricter about
//: what counts as periodic
constexpr double kYinThreshold = 0.15;
//: a frame has to carry some energy to be voiced at all
constexpr double kQuietRms = 0.004;
}  // namespace

double F0Track::at(double seconds) const {
    if (f0.empty()) return 0.0;
    long i = long(std::floor(seconds / hop_s + 0.5));
    if (i < 0) i = 0;
    if (i >= long(f0.size())) i = long(f0.size()) - 1;
    return f0[size_t(i)];
}

F0Track frame_track(const std::vector<std::pair<size_t, double>> &frames, size_t frame, size_t length, double rate) {
    F0Track t;
    for (size_t i = 0;; ++i) {
        size_t at = size_t(std::lround(double(i) * t.hop_s * rate));
        if (at >= length) break;
        auto it = std::upper_bound(frames.begin(), frames.end(), at,
                                   [](size_t v, const std::pair<size_t, double> &e) { return v < e.first; });
        bool in = it != frames.begin() && at < (it - 1)->first + frame;
        t.f0.push_back(in ? (it - 1)->second : 0.0);
    }
    return t;
}

F0Track track_f0(const std::vector<float> &x, double rate, double fmin, double fmax) {
    F0Track t;
    t.hop_s = 0.005;
    const size_t hop = size_t(std::lround(rate * t.hop_s));
    const size_t tau_max = size_t(rate / fmin) + 1;
    const size_t tau_min = std::max<size_t>(2, size_t(rate / fmax));
    const size_t window = tau_max * 2;   // two of the longest period
    if (x.size() < window + tau_max) return t;
    std::vector<double> d(tau_max + 1), cm(tau_max + 1);
    for (size_t start = 0; start + window + tau_max <= x.size(); start += hop) {
        const float *s = x.data() + start;
        double energy = 0.0;
        for (size_t i = 0; i < window; ++i) energy += double(s[i]) * s[i];
        if (std::sqrt(energy / window) < kQuietRms) {
            t.f0.push_back(0.0);
            continue;
        }
        // the difference function, then its cumulative mean normalisation
        d[0] = 0.0;
        double running = 0.0;
        cm[0] = 1.0;
        for (size_t tau = 1; tau <= tau_max; ++tau) {
            double acc = 0.0;
            for (size_t i = 0; i < window; i += 1) {
                double diff = double(s[i]) - s[i + tau];
                acc += diff * diff;
            }
            d[tau] = acc;
            running += acc;
            cm[tau] = running > 0 ? acc * tau / running : 1.0;
        }
        // the first dip under the threshold, taken to its bottom
        size_t best = 0;
        for (size_t tau = tau_min; tau <= tau_max; ++tau) {
            if (cm[tau] < kYinThreshold) {
                while (tau + 1 <= tau_max && cm[tau + 1] < cm[tau]) ++tau;
                best = tau;
                break;
            }
        }
        if (best == 0) {
            // nothing clearly periodic: the lowest dip, if it is low enough
            double low = 1.0;
            for (size_t tau = tau_min; tau <= tau_max; ++tau)
                if (cm[tau] < low) {
                    low = cm[tau];
                    best = tau;
                }
            if (low > 0.35) best = 0;
        }
        if (best == 0) {
            t.f0.push_back(0.0);
            continue;
        }
        double tau = double(best);
        if (best > tau_min && best < tau_max) {
            // a parabola through the three points around the dip
            double a = cm[best - 1], b = cm[best], c = cm[best + 1];
            double denom = a - 2 * b + c;
            if (std::fabs(denom) > 1e-12) tau += 0.5 * (a - c) / denom;
        }
        t.f0.push_back(rate / tau);
    }
    // a lone voiced or unvoiced frame between two of the other kind is
    // usually the estimator, not the voice
    for (size_t i = 1; i + 1 < t.f0.size(); ++i) {
        bool v = t.f0[i] > 0, before = t.f0[i - 1] > 0, after = t.f0[i + 1] > 0;
        if (v && !before && !after) t.f0[i] = 0.0;
        if (!v && before && after) t.f0[i] = 0.5 * (t.f0[i - 1] + t.f0[i + 1]);
    }
    return t;
}

std::vector<Segment> split_on_silence(const std::vector<float> &x, double rate, double gap_s, double floor_db,
                                      double min_s) {
    std::vector<Segment> out;
    const size_t hop = std::max<size_t>(1, size_t(rate * 0.005));
    std::vector<double> env;
    double peak = 0.0;
    for (size_t i = 0; i < x.size(); i += hop) {
        double acc = 0.0;
        size_t n = std::min(hop, x.size() - i);
        for (size_t k = 0; k < n; ++k) acc += double(x[i + k]) * x[i + k];
        double r = std::sqrt(acc / double(n));
        env.push_back(r);
        peak = std::max(peak, r);
    }
    if (peak <= 0) return out;
    const double threshold = std::max(peak * std::pow(10.0, floor_db / 20.0), 1e-4);
    const size_t gap = size_t(std::ceil(gap_s / 0.005));
    size_t i = 0;
    while (i < env.size()) {
        if (env[i] < threshold) {
            ++i;
            continue;
        }
        size_t j = i, last_loud = i;
        while (j < env.size() && j - last_loud <= gap) {
            if (env[j] >= threshold) last_loud = j;
            ++j;
        }
        Segment s;
        s.begin = i * hop;
        s.end = std::min(x.size(), (last_loud + 1) * hop);
        if (double(s.end - s.begin) / rate >= min_s) out.push_back(s);
        i = j;
    }
    return out;
}

double rms(const std::vector<float> &x, double rate, const F0Track *voicing) {
    double acc = 0.0;
    size_t n = 0;
    for (size_t i = 0; i < x.size(); ++i) {
        if (voicing && !voicing->f0.empty() && !voicing->voiced_at(i / rate)) continue;
        acc += double(x[i]) * x[i];
        ++n;
    }
    if (n == 0) {
        for (float v : x) acc += double(v) * v;
        n = x.size();
    }
    return n ? std::sqrt(acc / double(n)) : 0.0;
}

namespace {

struct Mark {
    long pos;        //: a glottal pulse, as a sample index
    double period;   //: the period there, in samples
};

//: One mark per glottal pulse through every voiced stretch: the strongest
//: peak of the first period, then the strongest peak about a period on from
//: each.
std::vector<Mark> pitch_marks(const std::vector<float> &x, double rate, const F0Track &f0) {
    std::vector<Mark> marks;
    const long n = long(x.size());
    long i = 0;
    while (i < n) {
        double hz = f0.at(i / rate);
        if (hz <= 0) {
            ++i;
            continue;
        }
        long period = std::max(2L, long(std::lround(rate / hz)));
        long end = std::min(n, i + period);
        long m = i;
        for (long k = i; k < end; ++k)
            if (x[size_t(k)] > x[size_t(m)]) m = k;
        marks.push_back({m, double(period)});
        long last = m;
        while (true) {
            double hz_here = f0.at(last / rate);
            if (hz_here <= 0) break;
            long p = std::max(2L, long(std::lround(rate / hz_here)));
            long lo = last + long(std::lround(0.75 * p)), hi = last + long(std::lround(1.25 * p));
            if (lo >= n) break;
            hi = std::min(hi, n - 1);
            long best = lo;
            for (long k = lo; k <= hi; ++k)
                if (x[size_t(k)] > x[size_t(best)]) best = k;
            if (f0.at(best / rate) <= 0) break;
            marks.push_back({best, double(best - last)});
            last = best;
        }
        i = last + 1;
    }
    return marks;
}

struct FineMark {
    double pos;      //: a glottal pulse, in samples, between samples if need be
    double period;   //: the period there, in samples
};

//: `x` at `p` samples, read between samples in a straight line.
double sample_at(const std::vector<float> &x, double p) {
    long i = long(std::floor(p));
    if (i < 0 || i + 1 >= long(x.size())) return 0.0;
    double f = p - double(i);
    return x[size_t(i)] * (1.0 - f) + x[size_t(i + 1)] * f;
}

//: One mark per glottal pulse through every voiced stretch: the strongest
//: peak of the first period, then each one a period on from the last, at the
//: lag (within a quarter of a period of the expected one) where the period
//: from there correlates best with the period from the last.
std::vector<FineMark> matched_marks(const std::vector<float> &x, double rate, const F0Track &f0) {
    std::vector<FineMark> marks;
    const long n = long(x.size());
    long i = 0;
    while (i < n) {
        double hz = f0.at(i / rate);
        if (hz <= 0) {
            ++i;
            continue;
        }
        long period = std::max(2L, long(std::lround(rate / hz)));
        long end = std::min(n, i + period);
        long m = i;
        for (long k = i; k < end; ++k)
            if (x[size_t(k)] > x[size_t(m)]) m = k;
        double last = double(m);
        marks.push_back({last, double(period)});
        std::vector<double> score;
        while (true) {
            double hz_here = f0.at(last / rate);
            if (hz_here <= 0) break;
            double p = rate / hz_here;
            long lo = std::max(2L, long(std::lround(0.75 * p))), hi = long(std::lround(1.25 * p));
            long span = std::max(2L, long(std::lround(p)));
            if (long(last) + hi + span + 1 >= n) break;
            score.assign(size_t(hi - lo + 1), 0.0);
            for (long d = lo; d <= hi; ++d) {
                double uv = 0.0, uu = 0.0, vv = 0.0;
                for (long k = 0; k < span; ++k) {
                    double u = sample_at(x, last + double(k)), v = sample_at(x, last + double(d + k));
                    uv += u * v;
                    uu += u * u;
                    vv += v * v;
                }
                score[size_t(d - lo)] = uv / std::sqrt(uu * vv + 1e-20);
            }
            long best = long(std::max_element(score.begin(), score.end()) - score.begin());
            double frac = 0.0;
            if (best > 0 && best + 1 < long(score.size())) {
                double a = score[size_t(best - 1)], b = score[size_t(best)], c = score[size_t(best + 1)];
                double den = a - 2.0 * b + c;
                if (den < 0.0) frac = 0.5 * (a - c) / den;
            }
            double d = double(lo + best) + frac;
            if (f0.at((last + d) / rate) <= 0) break;
            marks.push_back({last + d, d});
            last += d;
        }
        i = long(last) + 1;
    }
    return marks;
}

}  // namespace

std::vector<float> psola(const std::vector<float> &src, double rate, const F0Track &f0, size_t length,
                         const std::function<double(double)> &source_time,
                         const std::function<double(double)> &target_hz) {
    std::vector<float> out(length, 0.0f);
    if (src.empty() || length == 0) return out;
    const std::vector<Mark> marks = pitch_marks(src, rate, f0);
    const long n = long(src.size());
    const long hop = std::max(1L, long(std::lround(rate * 0.005)));
    auto add_grain = [&](long centre_src, long half, long centre_out, double gain) {
        for (long k = -half; k < half; ++k) {
            long si = centre_src + k, oi = centre_out + k;
            if (si < 0 || si >= n || oi < 0 || oi >= long(length)) continue;
            double w = 0.5 * (1.0 + std::cos(kPi * double(k) / double(half)));
            out[size_t(oi)] += float(src[size_t(si)] * w * gain);
        }
    };
    double t = 0.0;   // output position, in samples
    while (t < double(length)) {
        double s_sec = source_time(t / rate);
        long s = long(std::lround(std::max(0.0, std::min(double(n - 1), s_sec * rate))));
        double hz_src = f0.at(s / rate);
        double hz_want = target_hz(t / rate);
        if (hz_src > 0 && !marks.empty()) {
            // the nearest mark, and a grain two of its periods wide
            auto it = std::lower_bound(marks.begin(), marks.end(), s,
                                       [](const Mark &m, long v) { return m.pos < v; });
            if (it == marks.end() || (it != marks.begin() && s - (it - 1)->pos < it->pos - s)) --it;
            const Mark &m = *it;
            double p_src = m.period;
            double p_out = hz_want > 0 ? rate / std::max(40.0, std::min(1200.0, hz_want)) : p_src;
            long half = std::max(2L, long(std::lround(p_src)));
            add_grain(m.pos, half, long(std::lround(t)), p_out / p_src);
            t += p_out;
        } else {
            add_grain(s, hop, long(std::lround(t)), 1.0);
            t += double(hop);
        }
    }
    return out;
}

std::vector<float> psola_matched(const std::vector<float> &src, double rate, const F0Track &f0, size_t length,
                                 const std::function<double(double)> &source_time,
                                 const std::function<double(double)> &target_hz) {
    std::vector<float> out(length, 0.0f);
    if (src.empty() || length == 0) return out;
    const std::vector<FineMark> marks = matched_marks(src, rate, f0);
    const double n = double(src.size());
    const long hop = std::max(1L, long(std::lround(rate * 0.005)));
    // a grain `half` wide either side, read around `centre_src` and laid
    // around `centre_out`, both of them between samples if need be
    auto add_grain = [&](double centre_src, long half, double centre_out, double gain) {
        long c = long(std::floor(centre_out));
        double frac = centre_out - double(c);
        for (long k = -half; k <= half + 1; ++k) {
            long oi = c + k;
            double d = double(k) - frac;
            if (oi < 0 || oi >= long(length) || std::fabs(d) >= double(half)) continue;
            double w = 0.5 * (1.0 + std::cos(kPi * d / double(half)));
            out[size_t(oi)] += float(sample_at(src, centre_src + d) * w * gain);
        }
    };
    double t = 0.0;   // output position, in samples
    while (t < double(length)) {
        double s = std::max(0.0, std::min(n - 1.0, source_time(t / rate) * rate));
        double hz_src = f0.at(s / rate);
        double hz_want = target_hz(t / rate);
        if (hz_src > 0 && !marks.empty()) {
            // the two marks either side, their grains mixed by how near
            // each is: taking the nearest alone, a voice raised by more than
            // an octave takes the same mark twice and the next one twice,
            // and that pattern is heard as an undertone an octave down
            auto it = std::lower_bound(marks.begin(), marks.end(), s,
                                       [](const FineMark &m, double v) { return m.pos < v; });
            const FineMark &b = it == marks.end() ? marks.back() : *it;
            const FineMark &a = it == marks.begin() ? b : *(it - 1);
            double mix = b.pos - a.pos > 1.0 && b.pos - a.pos < 1.5 * b.period
                             ? std::max(0.0, std::min(1.0, (s - a.pos) / (b.pos - a.pos)))
                             : (s - a.pos < b.pos - s ? 0.0 : 1.0);
            double p_src = a.period + (b.period - a.period) * mix;
            double p_out = hz_want > 0 ? rate / std::max(40.0, std::min(1200.0, hz_want)) : p_src;
            long half = std::max(2L, long(std::lround(p_src)));
            if (mix < 1.0) add_grain(a.pos, half, t, (1.0 - mix) * p_out / p_src);
            if (mix > 0.0) add_grain(b.pos, half, t, mix * p_out / p_src);
            t += p_out;
        } else {
            add_grain(std::floor(s), hop, std::floor(t), 1.0);
            t += double(hop);
        }
    }
    return out;
}

std::function<double(double)> syllable_time_map(size_t src_length, double rate, const F0Track &f0,
                                                size_t out_length, double head_out, double tail_out) {
    const double src_s = src_length / rate, out_s = out_length / rate;
    // the vowel: the longest voiced stretch
    double v0 = 0.0, v1 = 0.0, run0 = -1.0;
    for (size_t i = 0; i <= f0.f0.size(); ++i) {
        bool voiced = i < f0.f0.size() && f0.f0[i] > 0;
        double at = std::min(src_s, i * f0.hop_s);
        if (voiced && run0 < 0) run0 = at;
        if (!voiced && run0 >= 0) {
            if (at - run0 > v1 - v0) {
                v0 = run0;
                v1 = at;
            }
            run0 = -1.0;
        }
    }
    const double head = v0, tail = std::max(0.0, src_s - v1), core = v1 - v0;
    if (head_out < 0) head_out = head;
    if (tail_out < 0) tail_out = tail;
    if (core < 0.030 || out_s <= head_out + tail_out + 0.020 || src_s <= 0) {
        // no vowel to lean on, or the syllable is being shortened: evenly
        double k = src_s > 0 ? src_s / std::max(out_s, 1e-6) : 1.0;
        return [k](double t) { return t * k; };
    }
    const double new_core = out_s - head_out - tail_out;
    const double k_head = head_out > 0.001 ? head / head_out : 0.0;
    const double k_core = core / new_core;
    const double k_tail = tail_out > 0.001 ? tail / tail_out : 0.0;
    return [=](double t) {
        if (t < head_out) return head_out > 0.001 ? t * k_head : v0;
        if (t < out_s - tail_out) return v0 + (t - head_out) * k_core;
        return tail_out > 0.001 ? v1 + (t - (out_s - tail_out)) * k_tail : v1;
    };
}

}  // namespace dsp
}  // namespace svs
