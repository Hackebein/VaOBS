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
constexpr double minimum_energy = 1e-14;

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
    if (energy / n < minimum_energy) return result;
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
        if (variance / n < minimum_energy) continue;
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

// Track near an established offset, allowing a small difference in media
// clock rate. Fixed-rate correlation loses the peak when the signals slide
// past one another within an analysis window.
inline Match estimate_tracking(const std::vector<float> &reference, const std::vector<float> &feed,
                              const std::vector<uint8_t> &valid, double expected_delay_ms,
                              double expected_drift_ms_per_second, double minimum_correlation = .75)
{
    Match best;
    if (feed.size() < reference.size() || valid.size() != feed.size()) return best;
    constexpr size_t radius = analysis_rate / 4;
    const size_t n = reference.size(), maximum = feed.size() - n;
    const double center = double(maximum) + double(n) / 2 - expected_delay_ms * analysis_rate / 1000;
    std::vector<float> warped(n + 2 * radius);
    std::vector<uint8_t> present(warped.size());
    for (int candidate = 0; candidate <= 16; ++candidate) {
        const double adjustment = candidate == 0 ? 0 : ((candidate + 1) / 2) * .5 * (candidate % 2 ? 1 : -1);
        const double drift = std::clamp(expected_drift_ms_per_second + adjustment, -20.0, 20.0);
        const double scale = 1 - drift / 1000;
        for (size_t i = 0; i < warped.size(); ++i) {
            const double position = center + (double(i) - radius - double(n) / 2) * scale;
            const int64_t index = int64_t(std::floor(position));
            const bool exists = index >= 0 && index + 1 < int64_t(feed.size()) && valid[size_t(index)] && valid[size_t(index + 1)];
            present[i] = exists;
            const double fraction = position - double(index);
            warped[i] = exists ? float(feed[size_t(index)] * (1 - fraction) + feed[size_t(index + 1)] * fraction) : 0;
        }
        auto match = estimate(reference, warped, present, minimum_correlation);
        match.delay_ms = expected_delay_ms + (match.delay_ms - 250) * scale;
        if (match.reliable && (!best.reliable || match.correlation > best.correlation)) best = match;
        if (best.reliable && best.correlation >= .97) break;
    }
    return best;
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
    int strong_confirmations_ = 0, weak_confirmations_ = 0, failures_ = 0;
    int64_t last_window_end_ = -1, last_confirmation_end_ = -1;
    int64_t measured_at_ = -1, pending_at_ = -1;
    double drift_ = 0;
    bool holding_ = false;
    int verification_window_ms_ = 0;
    void clear_pending()
    {
        pending_ = -1; strong_confirmations_ = weak_confirmations_ = 0;
        last_confirmation_end_ = -1;
    }
public:
    bool locked = false;
    double delay_ms = 0;
    int window_ms() const { return verification_window_ms_; }
    bool confirming() const { return pending_ >= 0; }
    bool holding() const { return locked && holding_; }
    double drift_at(int64_t time_ms) const { return measured_at_ >= 0 && time_ms - measured_at_ < 10000 ? drift_ : 0; }
    double delay_at(int64_t time_ms) const {
        return std::max(0.0, delay_ms + drift_ * std::clamp(double(time_ms - measured_at_) / 1000, 0.0, 10.0));
    }
    void reset()
    {
        clear_pending(); failures_ = 0; last_window_end_ = -1;
        verification_window_ms_ = 0; locked = false; delay_ms = drift_ = 0;
        measured_at_ = pending_at_ = -1; holding_ = false;
    }
    void observe(const Match &m, int64_t window_end_ms, int window_ms, bool full_range,
                 bool reference_active = true)
    {
        // Frozen windows and absent/silent reference audio are not evidence
        // that the selected stream is wrong. Keep its established timing.
        if (window_end_ms <= last_window_end_) { holding_ = true; return; }
        last_window_end_ = window_end_ms;
        if (!reference_active) { clear_pending(); holding_ = true; return; }
        const bool strong = m.correlation >= .97;
        holding_ = !m.reliable;
        if (!m.reliable) {
            clear_pending();
            if (++failures_ >= 30) locked = false;
            return;
        }
        const int64_t measured = window_end_ms - window_ms / 2;
        // Once acquired, a nearby reliable peak can track a moving offset.
        // The strict acquisition path below still protects unexpected jumps.
        if (locked && std::abs(m.delay_ms - delay_at(measured)) <= 25) {
            const double seconds = double(measured - measured_at_) / 1000;
            if (std::abs(m.delay_ms - delay_ms) < .1) drift_ = 0;
            else if (seconds > 0 && seconds <= 10)
                drift_ = .8 * drift_ + .2 * std::clamp((m.delay_ms - delay_ms) / seconds, -20.0, 20.0);
            else drift_ = 0;
            delay_ms = m.delay_ms; measured_at_ = measured;
            failures_ = 0; clear_pending(); verification_window_ms_ = 0; return;
        }
        if (!strong) verification_window_ms_ = 12000;
        // Weak matches need a clearer winner than normal tracking does.
        if (!strong && m.prominence < 1.2) {
            clear_pending();
            holding_ = true;
            if (++failures_ >= 30) locked = false;
            return;
        }
        const double tolerance = 2 + std::max(0.0, double(measured - pending_at_)) * .02;
        if (pending_ < 0 || std::abs(m.delay_ms - pending_) > tolerance) {
            clear_pending(); pending_ = m.delay_ms;
        }
        pending_ = m.delay_ms; pending_at_ = measured;
        strong_confirmations_ = strong ? strong_confirmations_ + 1 : 0;
        // Count distinct passages, rather than successive overlapping windows.
        if (window_ms >= 12000 && m.prominence >= 1.2 &&
            (last_confirmation_end_ < 0 || window_end_ms - last_confirmation_end_ >= window_ms)) {
            ++weak_confirmations_; last_confirmation_end_ = window_end_ms;
        }
        // Before the full range exists, a repeated phrase can hide the real
        // delayed passage. Strong matches retain the quick two-check path.
        if (strong_confirmations_ >= 2 || (weak_confirmations_ >= 3 && full_range)) {
            delay_ms = m.delay_ms; measured_at_ = measured; drift_ = 0;
            locked = true; holding_ = false; failures_ = 0;
            clear_pending(); verification_window_ms_ = 0;
        } else if (++failures_ >= 30) {
            locked = false;
        }
    }
};

// Do not reject an initial candidate before the whole delay range is covered.
// Silence or a missing reference cannot tell us which player is audible.
class SearchProgress {
    bool matched_ = false;
    unsigned failures_ = 0;
public:
    void reset() { matched_ = false; failures_ = 0; }
    bool matched() const { return matched_; }
    bool observe(bool locked, bool reliable, bool reference_active, bool full_range)
    {
        matched_ = matched_ || locked;
        if (!reference_active || reliable || (!matched_ && !full_range)) { failures_ = 0; return false; }
        ++failures_;
        return !locked && failures_ >= (matched_ ? 10U : 3U);
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
    float sample(size_t channel, double index) const { return read(channel, index); }
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
