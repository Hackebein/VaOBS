// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <vector>

namespace vaobs {

constexpr int analysis_rate = 2000;

struct Match {
    double delay_ms = 0;
    double correlation = 0;
    double prominence = 0;
    bool reliable = false;
};

inline void fft(std::vector<std::complex<double>> &a, bool inverse)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    constexpr double pi = 3.14159265358979323846;
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = 2 * pi / double(len) * (inverse ? 1 : -1);
        const std::complex<double> root(std::cos(angle), std::sin(angle));
        for (size_t base = 0; base < n; base += len) {
            std::complex<double> w(1, 0);
            for (size_t j = 0; j < len / 2; ++j) {
                const auto u = a[base + j], v = a[base + j + len / 2] * w;
                a[base + j] = u + v;
                a[base + j + len / 2] = u - v;
                w *= root;
            }
        }
    }
    if (inverse) for (auto &v : a) v /= double(n);
}

// Reference is the latest window. Feed includes that window plus earlier audio.
// Both vectors must occupy the same timestamp grid and have the same end time.
// A positive result means the feed plays early and needs a delay.
inline Match estimate(const std::vector<float> &reference, const std::vector<float> &feed,
                      const std::vector<uint8_t> &valid, double minimum_correlation = .75)
{
    Match result;
    const size_t n = reference.size();
    if (n < size_t(analysis_rate) || feed.size() < n || valid.size() != feed.size()) return result;
    const size_t max_lag = feed.size() - n;
    double mean = 0, energy = 0;
    for (float v : reference) mean += v;
    mean /= double(n);
    size_t nfft = 1;
    while (nfft < feed.size() + n - 1) nfft <<= 1;
    std::vector<std::complex<double>> a(nfft), b(nfft);
    for (size_t i = 0; i < n; ++i) {
        const double v = reference[i] - mean;
        a[i] = v;
        energy += v * v;
    }
    // Don't calibrate on silence or floating point noise.
    if (energy / n < 1e-10) return result;
    std::vector<double> sums(feed.size() + 1), squares(feed.size() + 1);
    std::vector<size_t> counts(feed.size() + 1);
    for (size_t i = 0; i < feed.size(); ++i) {
        const double v = valid[i] && std::isfinite(feed[i]) ? feed[i] : 0;
        b[i] = v;
        sums[i + 1] = sums[i] + v;
        squares[i + 1] = squares[i] + v * v;
        counts[i + 1] = counts[i] + (valid[i] != 0);
    }
    fft(a, false); fft(b, false);
    for (size_t i = 0; i < nfft; ++i) b[i] *= std::conj(a[i]);
    fft(b, true);
    std::vector<double> scores(max_lag + 1), signed_scores(max_lag + 1);
    for (size_t k = 0; k <= max_lag; ++k) {
        if (counts[k + n] - counts[k] < n * .98) continue;
        const double sum = sums[k + n] - sums[k];
        const double variance = squares[k + n] - squares[k] - sum * sum / n;
        if (variance / n < 1e-10) continue;
        signed_scores[k] = b[k].real() / std::sqrt(energy * variance);
        scores[k] = std::min(1.0, std::abs(signed_scores[k]));
    }
    // Refine several candidate peaks so a fractional-bin true match cannot
    // lose to an integer-bin repeated beat. Histories are anti-aliased; Lanczos
    // interpolation gives sub-millisecond precision without a huge full FFT.
    auto sinc = [](double x) {
        constexpr double pi = 3.14159265358979323846;
        return std::abs(x) < 1e-12 ? 1.0 : std::sin(pi * x) / (pi * x);
    };
    double best_position = 0, best_score = 0, runner_up = 0;
    for (int candidate = 0; candidate < 8; ++candidate) {
        const size_t k = size_t(std::max_element(scores.begin(), scores.end()) - scores.begin());
        if (scores[k] <= 0) break;
        double peak = scores[k], position = double(k);
        if (k >= 8 && k + 8 < signed_scores.size()) {
            for (int step = -16; step <= 16; ++step) {
                const double offset = double(step) / 32;
                double value = 0, weights = 0;
                for (int tap = -7; tap <= 8; ++tap) {
                    const double distance = offset - tap;
                    const double weight = sinc(distance) * sinc(distance / 8);
                    value += signed_scores[size_t(int64_t(k) + tap)] * weight;
                    weights += weight;
                }
                const double refined = std::min(1.0, std::abs(value / weights));
                if (refined > peak) { peak = refined; position = double(k) + offset; }
            }
        }
        if (peak > best_score) { runner_up = best_score; best_score = peak; best_position = position; }
        else runner_up = std::max(runner_up, peak);
        const size_t radius = size_t(analysis_rate * .05);
        std::fill(scores.begin() + (k > radius ? k - radius : 0),
                  scores.begin() + std::min(scores.size(), k + radius + 1), 0);
    }
    result.delay_ms = (double(max_lag) - best_position) * 1000 / analysis_rate;
    result.correlation = best_score;
    result.prominence = runner_up > 0 ? best_score / runner_up : 100;
    result.reliable = result.correlation >= minimum_correlation && result.prominence >= 1.06;
    return result;
}

// Timestamped, bounded analysis history. Missing packets stay invalid rather
// than being silently treated as real audio. Samples are averaged onto a common
// 2 kHz grid, including when OBS uses 44.1 kHz or variable packet sizes.
class Timeline {
    struct LowPass {
        double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        void configure(uint32_t rate, double q)
        {
            constexpr double pi = 3.14159265358979323846;
            const double w = 2 * pi * (analysis_rate * .3) / rate;
            const double c = std::cos(w), alpha = std::sin(w) / (2 * q), a0 = 1 + alpha;
            b0 = (1 - c) / 2 / a0; b1 = (1 - c) / a0; b2 = b0;
            a1 = -2 * c / a0; a2 = (1 - alpha) / a0; z1 = z2 = 0;
        }
        double process(double x)
        {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2; z2 = b2 * x - a2 * y;
            return y;
        }
    } lowpass_[2];
    uint32_t input_rate_ = 0;
    std::vector<float> values_;
    std::vector<int64_t> stamps_;
    int64_t partial_ = -1, first_ = -1, last_ = -1;
    double sum_ = 0;
    size_t count_ = 0;

    void put(int64_t index, float value)
    {
        const size_t slot = size_t(index) % values_.size();
        values_[slot] = value; stamps_[slot] = index;
        if (first_ < 0) first_ = index;
        last_ = std::max(last_, index);
    }

public:
    explicit Timeline(size_t capacity = 140 * analysis_rate)
        : values_(capacity), stamps_(capacity, -1) {}

    void clear()
    {
        std::fill(stamps_.begin(), stamps_.end(), -1);
        partial_ = first_ = last_ = -1; sum_ = 0; count_ = 0; input_rate_ = 0;
    }
    int64_t first() const { return std::max(first_, last_ - int64_t(values_.size()) + 1); }
    int64_t end() const { return last_ + 1; }

    void append(const float *const *planes, size_t channels, size_t frames,
                uint32_t sample_rate, uint64_t timestamp)
    {
        if (!channels || !sample_rate || !planes[0]) return;
        if (input_rate_ != sample_rate) {
            lowpass_[0].configure(sample_rate, .541196100146197);
            lowpass_[1].configure(sample_rate, 1.306562964876377);
            input_rate_ = sample_rate;
        }
        for (size_t i = 0; i < frames; ++i) {
            const uint64_t t = timestamp + uint64_t(i) * 1000000000ULL / sample_rate;
            const int64_t index = int64_t(t / 1000000000ULL * analysis_rate +
                                         (t % 1000000000ULL) * analysis_rate / 1000000000ULL);
            if (index < partial_) continue;
            if (index != partial_) {
                if (count_) put(partial_, float(sum_ / count_));
                partial_ = index; sum_ = 0; count_ = 0;
            }
            // Channel one avoids cancellation in stereo sources with inverted
            // side information; gain differences are removed by correlation.
            const float v = planes[0][i];
            sum_ += lowpass_[1].process(lowpass_[0].process(std::isfinite(v) ? v : 0)); ++count_;
        }
    }

    size_t read(int64_t start, size_t count, std::vector<float> &out,
                std::vector<uint8_t> &valid) const
    {
        out.resize(count); valid.resize(count);
        size_t present = 0;
        for (size_t i = 0; i < count; ++i) {
            const int64_t index = start + int64_t(i);
            const size_t slot = index < 0 ? 0 : size_t(index) % values_.size();
            const bool exists = index >= 0 && stamps_[slot] == index;
            out[i] = exists ? values_[slot] : 0; valid[i] = exists;
            present += exists;
        }
        return present;
    }
};

class Lock {
    double pending_ = -1;
    int confirmations_ = 0, failures_ = 0;
    bool acquired_ = false;
public:
    bool locked = false;
    double delay_ms = 0;
    void reset() { pending_ = -1; confirmations_ = failures_ = 0; locked = acquired_ = false; delay_ms = 0; }
    void observe(const Match &m)
    {
        // Short repeated musical phrases can resemble a valid match before
        // the true delayed passage has reached the reference. Initial lock
        // requires near-identical audio, not just the tracking threshold.
        if (!m.reliable || (!acquired_ && m.correlation < .97)) {
            confirmations_ = 0;
            if (++failures_ >= 10) locked = false;
            return;
        }
        failures_ = 0;
        if (locked && std::abs(m.delay_ms - delay_ms) <= 1) return;
        if (pending_ < 0 || std::abs(m.delay_ms - pending_) > 2) {
            pending_ = m.delay_ms; confirmations_ = 1; return;
        }
        if (++confirmations_ >= 2) {
            delay_ms = m.delay_ms; locked = acquired_ = true;
            pending_ = -1; confirmations_ = 0;
        }
    }
};

// Timestamped PCM, read by an independent output clock. Incoming packet gaps
// do not stop playback of audio already held in the buffer.
class TimedBuffer {
    std::vector<std::vector<float>> ring_;
    std::vector<int64_t> stamps_;
    uint32_t rate_ = 0;
    double delay_ = 0, old_delay_ = 0, wet_ = 0;
    size_t fade_left_ = 0, fade_length_ = 0;
    float read(size_t channel, double index) const
    {
        if (index < 0) return 0;
        const uint64_t i = uint64_t(index);
        const double f = index - double(i);
        const auto &r = ring_[channel];
        const size_t slot = size_t(i % r.size()), next = size_t((i + 1) % r.size());
        if (stamps_[slot] != int64_t(i)) return 0;
        if (f < 1e-7) return r[slot];
        if (stamps_[next] != int64_t(i + 1)) return 0;
        return float(r[slot] * (1 - f) + r[next] * f);
    }
public:
    static int64_t frame_at(uint64_t timestamp, uint32_t rate)
    {
        return int64_t(timestamp / 1000000000ULL) * rate +
            int64_t(std::llround(double(timestamp % 1000000000ULL) * rate / 1e9));
    }
    void configure(uint32_t sample_rate, size_t channels, double max_delay_ms)
    {
        rate_ = sample_rate;
        const size_t capacity = size_t(std::ceil((max_delay_ms / 1000 + 1) * rate_));
        ring_.assign(channels, std::vector<float>(capacity));
        stamps_.assign(capacity, -1); delay_ = old_delay_ = wet_ = 0;
        fade_left_ = 0; fade_length_ = std::max(size_t(1), size_t(rate_ * .04));
    }
    void write(const float *const *planes, size_t frames, uint64_t timestamp)
    {
        if (ring_.empty()) return;
        const int64_t start = frame_at(timestamp, rate_);
        for (size_t i = 0; i < frames; ++i) {
            const int64_t index = start + int64_t(i);
            const size_t slot = size_t(index) % stamps_.size();
            stamps_[slot] = index;
            for (size_t ch = 0; ch < ring_.size(); ++ch)
                ring_[ch][slot] = planes[ch] && std::isfinite(planes[ch][i]) ? planes[ch][i] : 0;
        }
    }
    void render(float *const *planes, size_t frames, uint64_t timestamp, bool locked, double delay_ms)
    {
        if (ring_.empty()) return;
        const double wanted = std::clamp(delay_ms * rate_ / 1000, 0.0, double(ring_[0].size() - 2));
        if (locked && std::abs(wanted - delay_) >= rate_ * .0005 && !fade_left_) {
            old_delay_ = delay_; delay_ = wanted; fade_left_ = fade_length_;
        }
        const int64_t start = frame_at(timestamp, rate_);
        for (size_t i = 0; i < frames; ++i) {
            const int64_t index = start + int64_t(i);
            const double desired_wet = locked ? 1 : 0;
            wet_ += std::clamp(desired_wet - wet_, -1.0 / fade_length_, 1.0 / fade_length_);
            const double blend = fade_left_ ? 1 - double(fade_left_) / fade_length_ : 1;
            for (size_t ch = 0; ch < ring_.size(); ++ch) {
                const float current = read(ch, double(index) - delay_);
                const float previous = fade_left_ ? read(ch, double(index) - old_delay_) : current;
                if (planes[ch]) planes[ch][i] = float((previous * (1 - blend) + current * blend) * wet_);
            }
            if (fade_left_) --fade_left_;
        }
    }
};
} // namespace vaobs
