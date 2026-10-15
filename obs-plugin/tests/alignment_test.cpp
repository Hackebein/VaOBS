// SPDX-License-Identifier: GPL-2.0-or-later
#include "alignment.hpp"
#include <cstdlib>
#include <iostream>
#include <random>
#include <string>

void require(bool value, const char *message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

std::vector<float> noise(size_t n, uint32_t seed)
{
    std::mt19937 rng(seed);
    std::normal_distribution<float> sample(0, .1f);
    std::vector<float> result(n);
    for (float &v : result) v = sample(rng);
    return result;
}

int main()
{
    using namespace vaobs;
    const size_t n = 4 * analysis_rate, maximum = 60 * analysis_rate;
    auto feed = noise(maximum + n, 7);
    const size_t lag = 42298; // 21,149 ms: above OBS WebSocket's 20,000 ms limit.
    std::vector<float> ref(n);
    auto voice = noise(n, 9);
    for (size_t i = 0; i < n; ++i) ref[i] = feed[maximum - lag + i] / 17 + voice[i] * .001f;
    std::vector<uint8_t> valid(feed.size(), 1);
    auto result = estimate(ref, feed, valid);
    require(result.reliable && std::abs(result.delay_ms - 21149) < .01, "21-second lead and gain mismatch");
    result = estimate(std::vector<float>(n), feed, valid);
    require(!result.reliable, "silence must not calibrate");
    result = estimate(noise(n, 85), feed, valid);
    require(!result.reliable, "unrelated speech/noise must not calibrate");
    for (size_t i = 0; i < feed.size(); ++i) feed[i] = float(std::sin(double(i) * .1));
    std::copy(feed.begin(), feed.begin() + n, ref.begin());
    require(!estimate(ref, feed, valid).reliable, "periodic ambiguous sound must not calibrate");

    Lock lock;
    lock.observe({1949, .88, 2, true}); lock.observe({1949, .88, 2, true});
    require(!lock.locked, "repeated phrases must not create a weak initial lock");
    Match strong{21149, .99, 2, true};
    lock.observe(strong); require(!lock.locked, "one observation is insufficient");
    lock.observe(strong); require(lock.locked, "two matching observations lock");
    Match changed{21198.5, .99, 2, true};
    lock.observe(changed); require(lock.delay_ms == 21149, "single timing jump must be confirmed");
    lock.observe(changed); require(lock.delay_ms == 21198.5, "confirmed timing jump updates delay");
    for (int i = 0; i < 9; ++i) lock.observe({});
    require(lock.locked, "short confidence loss holds the previous delay");
    lock.observe({}); require(!lock.locked, "sustained confidence loss mutes until recalibration");
    lock.reset(); require(!lock.locked && lock.delay_ms == 0, "stream change resets alignment");

    Timeline timeline(10000);
    auto audio = noise(88200, 12);
    const float *planes[]{audio.data()};
    timeline.append(planes, 1, audio.size(), 44100, 100000000000ULL);
    std::vector<float> values; std::vector<uint8_t> present;
    require(timeline.read(200000, 3999, values, present) == 3999, "44.1 kHz timestamp-grid resampling");
    timeline.read(199990, 10, values, present);
    require(std::all_of(present.begin(), present.end(), [](uint8_t v) { return v == 0; }), "missing packets stay invalid");

    // Exercise the actual anti-aliasing/timestamp path with a delay that falls
    // between analysis bins, not just already-resampled synthetic vectors.
    constexpr size_t native_rate = 48000, actual_delay = 1015141;
    auto native = noise(60 * native_rate, 96);
    Timeline ref_history, feed_history;
    const float *ref_native[]{native.data()};
    const float *feed_native[]{native.data() + actual_delay};
    ref_history.append(ref_native, 1, 30 * native_rate, native_rate, 0);
    feed_history.append(feed_native, 1, 30 * native_rate, native_rate, 0);
    const int64_t end = std::min(ref_history.end(), feed_history.end());
    std::vector<float> sampled_ref, sampled_feed;
    std::vector<uint8_t> sampled_valid, ignored;
    ref_history.read(end - int64_t(n), n, sampled_ref, ignored);
    feed_history.read(0, size_t(end), sampled_feed, sampled_valid);
    result = estimate(sampled_ref, sampled_feed, sampled_valid);
    require(result.reliable && result.correlation > .99 &&
            std::abs(result.delay_ms - double(actual_delay) / 48) < .05,
            "native-rate anti-aliasing and fractional-bin precision");

    TimedBuffer line;
    line.configure(1000, 1, 60000);
    auto signal = noise(23000, 42);
    auto delayed = signal;
    float *out[]{delayed.data()};
    const float *input[]{signal.data()};
    constexpr uint64_t origin = 1000000000000ULL;
    line.write(input, signal.size(), origin);
    line.render(out, delayed.size(), origin, true, 21149);
    require(std::abs(delayed[22000] - signal[851]) < 1e-6, "sample delay exceeds twenty seconds");
    line.configure(1000, 1, 60000);
    delayed = signal; out[0] = delayed.data();
    line.write(input, signal.size(), origin);
    line.render(out, delayed.size(), origin, true, 0);
    require(std::abs(delayed[22000] - signal[22000]) < 1e-6, "zero-delay passthrough after fade-in");
    line.configure(1000, 1, 60000);
    delayed = signal; out[0] = delayed.data();
    line.write(input, signal.size(), origin);
    line.render(out, delayed.size(), origin, false, 21149);
    require(std::all_of(delayed.begin(), delayed.end(), [](float v) { return v == 0; }), "unlocked feed remains silent");
    line.configure(1000, 2, 60000);
    auto other = signal;
    for (float &v : other) v *= -.37f;
    delayed = signal;
    float *stereo[]{delayed.data(), other.data()};
    const float *stereo_input[]{signal.data(), other.data()};
    line.write(stereo_input, signal.size(), origin);
    line.render(stereo, signal.size(), origin, true, 21149);
    require(std::abs(other[22000] + .37f * signal[851]) < 1e-6, "stereo channels retain their own samples and polarity");
    line.configure(1000, 1, 60000);
    line.write(input, 500, origin);
    std::vector<float> tail(1000);
    float *tail_output[]{tail.data()};
    line.render(tail_output, tail.size(), origin, true, 200);
    require(std::abs(tail[650] - signal[450]) < 1e-6 && tail[750] == 0,
            "buffered tail drains after input stops, then missing samples become silence");
    std::cout << "PASS: delay detection, ambiguity, confidence, timing jumps, resampling, and audio buffering\n";
}
