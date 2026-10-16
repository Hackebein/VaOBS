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
    const size_t lag = 42298; // 21,149 ms at the analysis sample rate.
    std::vector<float> ref(n);
    auto voice = noise(n, 9);
    for (size_t i = 0; i < n; ++i) ref[i] = feed[maximum - lag + i] / 17 + voice[i] * .001f;
    std::vector<uint8_t> valid(feed.size(), 1);
    auto result = estimate(ref, feed, valid);
    require(result.reliable && std::abs(result.delay_ms - 21149) < .01, "21-second lead and gain mismatch");
    for (float &sample : ref) sample *= .001f;
    const auto quiet = estimate(ref, feed, valid);
    require(quiet.reliable && std::abs(quiet.delay_ms - 21149) < .01 && std::abs(quiet.correlation - result.correlation) < 1e-6,
        "Quiet reference audio retains the same normalized match and offset");
    for (float &sample : ref) sample *= .001f;
    require(!estimate(ref, feed, valid).reliable, "Numerical noise remains below the silence floor");
    result = estimate(std::vector<float>(n), feed, valid);
    require(!result.reliable, "silence must not calibrate");
    result = estimate(noise(n, 85), feed, valid);
    require(!result.reliable, "unrelated speech/noise must not calibrate");
    for (size_t i = 0; i < feed.size(); ++i) feed[i] = float(std::sin(double(i) * .1));
    std::copy(feed.begin(), feed.begin() + n, ref.begin());
    require(!estimate(ref, feed, valid).reliable, "periodic ambiguous sound must not calibrate");
    const auto beat = noise(2 * analysis_rate, 71);
    std::vector<float> repeated(72 * analysis_rate);
    for (size_t i = 0; i < repeated.size(); ++i) repeated[i] = beat[i % beat.size()];
    std::vector<float> repeated_ref(repeated.begin(), repeated.begin() + 12 * analysis_rate);
    const auto repeated_match = estimate(repeated_ref, repeated, std::vector<uint8_t>(repeated.size(), 1));
    require(repeated_match.correlation > .99 && !repeated_match.reliable,
        "A long repeated beat has competing offsets even with near-perfect correlation");
    Lock repeated_lock;
    for (int64_t time = 12000; time <= 72000; time += 1000)
        repeated_lock.observe(repeated_match, time, 12000, true);
    require(!repeated_lock.locked, "Listening longer does not turn ambiguous repeated beats into a lock");

    Lock lock;
    int64_t observed_ms = 0;
    auto observe = [&](const Match &m) { lock.observe(m, observed_ms += 1000, 4000, true); };
    observe({1949, .88, 2, true}); observe({1949, .88, 2, true});
    require(!lock.locked, "repeated phrases must not create a weak initial lock");
    Match strong{21149, .99, 2, true};
    observe(strong); require(!lock.locked, "one observation is insufficient");
    observe(strong); require(lock.locked, "two matching observations lock");
    Match changed{21198.5, .99, 2, true};
    observe(changed); require(lock.delay_ms == 21149, "single timing jump must be confirmed");
    observe(changed); require(lock.delay_ms == 21198.5, "confirmed timing jump updates delay");
    for (int i = 0; i < 29; ++i) observe({});
    require(lock.locked, "short confidence loss holds the previous delay");
    observe({}); require(!lock.locked, "sustained confidence loss mutes until recalibration");
    lock.reset(); require(!lock.locked && lock.delay_ms == 0, "stream change resets alignment");

    Lock weak;
    const Match quiet_match{10227.2, .94, 1.8, true};
    weak.observe(quiet_match, 4000, 4000, false);
    require(weak.confirming() && weak.window_ms() == 12000 && !weak.locked,
        "A weak match requests a longer analysis window");
    for (int64_t time = 12000; time <= 35000; time += 1000)
        weak.observe(quiet_match, time, 12000, true);
    require(!weak.locked, "Overlapping windows cannot replace three independent passages");
    for (int i = 0; i < 20; ++i) weak.observe(quiet_match, 35000, 12000, true);
    require(!weak.locked, "Re-reading frozen audio cannot confirm alignment");
    weak.observe(quiet_match, 36000, 12000, true);
    require(weak.locked && weak.delay_ms == quiet_match.delay_ms && weak.window_ms() == 0,
        "Three independent weak matches lock and restore the short tracking window");

    weak.reset();
    for (int64_t time = 12000; time <= 70000; time += 1000)
        weak.observe(quiet_match, time, 12000, false);
    require(!weak.locked, "Repeated phrases cannot lock before the full delay range has been heard");
    const Match different_passage{21149, .94, 1.8, true};
    weak.observe(different_passage, 71000, 12000, true);
    require(!weak.locked, "A different winning offset discards previous weak evidence");
    weak.observe(different_passage, 83000, 12000, true);
    require(!weak.locked, "The replacement offset needs its own independent confirmations");
    weak.observe(different_passage, 95000, 12000, true);
    require(weak.locked && weak.delay_ms == 21149, "The confirmed replacement offset locks");

    weak.reset();
    for (int64_t time = 12000; time <= 72000; time += 1000)
        weak.observe({10227, .94, 1.1, true}, time, 12000, true);
    require(!weak.locked && !weak.confirming(), "Competing beat peaks remain ambiguous despite long listening");
    for (int64_t time = 73000; time <= 120000; time += 1000)
        weak.observe({time % 2000 ? 10000.0 : 11000.0, .94, 1.8, true}, time, 12000, true);
    require(!weak.locked, "Alternating beat offsets cannot accumulate confirmations");

    weak.reset();
    weak.observe(quiet_match, 12000, 12000, true);
    weak.observe(quiet_match, 24000, 12000, true);
    weak.observe({}, 25000, 12000, true);
    weak.observe(quiet_match, 36000, 12000, true);
    weak.observe(quiet_match, 48000, 12000, true);
    require(!weak.locked, "Missing or rejected audio breaks the weak confirmation sequence");
    weak.observe(quiet_match, 60000, 12000, true);
    require(weak.locked, "Fresh independent evidence can confirm after an interruption");
    for (int64_t time = 61000; time < 91000; time += 1000)
        weak.observe({12000, .94, 1.8, true}, time, 12000, true);
    require(weak.locked && weak.delay_ms == 12000, "A consistent weak timing jump confirms while established audio stays playing");

    Lock tracking;
    tracking.observe(strong, 4000, 4000, true);
    tracking.observe(strong, 5000, 4000, true);
    for (int64_t t = 6000; t <= 30000; t += 1000) {
        tracking.observe({21149 + (t - 5000) * .002, .9, 1.5, true}, t, 4000, true);
        require(tracking.locked, "Continuous weak drift tracks without reacquiring or muting");
    }
    require(std::abs(tracking.delay_at(30000) - 21203) < 1,
        "Drift prediction accounts for the analysis window's midpoint");
    for (int64_t t = 31000; t <= 150000; t += 1000)
        tracking.observe({}, t, 4000, true, false);
    require(tracking.locked && tracking.holding(), "Two minutes of silent/missing reference holds playback timing");
    const double held = tracking.delay_at(150000);
    require(tracking.delay_at(160000) == held && tracking.drift_at(150000) == 0,
        "Holdover extrapolation is bounded rather than drifting indefinitely");
    for (int i = 0; i < 100; ++i) tracking.observe({}, 150000, 4000, true);
    require(tracking.locked, "A frozen missing window cannot exhaust tracking confidence");
    for (int64_t t = 151000; t < 180000; t += 1000) tracking.observe({}, t, 4000, true);
    require(tracking.locked, "Temporary active interference keeps established playback running");
    tracking.observe({}, 180000, 4000, true);
    require(!tracking.locked, "Sustained fresh evidence of different audio still releases alignment");

    const auto drifting_feed = noise(10 * analysis_rate, 65);
    const size_t tracking_n = 2 * analysis_rate, tracking_maximum = drifting_feed.size() - tracking_n;
    std::vector<float> drifting_reference(tracking_n);
    for (const double slope : {-4.0, -2.0, 2.0, 4.0}) {
        for (size_t i = 0; i < tracking_n; ++i) {
            const double position = double(tracking_maximum) - analysis_rate + i * (1 - slope / 1000);
            const size_t index = size_t(position);
            drifting_reference[i] = float(drifting_feed[index] * (1 - (position - index)) + drifting_feed[index + 1] * (position - index));
        }
        const double midpoint_delay = 1000 + slope;
        const auto tracked = estimate_tracking(drifting_reference, drifting_feed, std::vector<uint8_t>(drifting_feed.size(), 1), midpoint_delay, 0);
        require(tracked.reliable && std::abs(tracked.delay_ms - midpoint_delay) < 1,
            "Actual time-warped audio remains matchable while media clocks drift in either direction");
    }

    SearchProgress search;
    for (int i = 0; i < 120; ++i)
        require(!search.observe(false, false, true, false), "Do not skip candidates before covering the maximum delay");
    for (int i = 0; i < 120; ++i)
        require(!search.observe(false, false, false, true), "Silence or missing reference must not cycle streams");
    require(!search.observe(false, false, true, true) && !search.observe(false, false, true, true),
        "Full-range failure requires repeated confirmation");
    require(search.observe(false, false, true, true), "An exhausted unmatched candidate advances");
    search.reset();
    require(!search.observe(false, true, true, true), "A potential match gets time to confirm");
    require(!search.observe(true, true, true, true) && search.matched(), "A locked stream stays selected");
    for (int i = 0; i < 9; ++i)
        require(!search.observe(i < 8, false, true, true), "Brief matching loss does not cycle");
    require(!search.observe(false, true, true, true), "Recovered matching audio cancels the search countdown");
    for (int i = 0; i < 9; ++i) require(!search.observe(false, false, true, true), "Wait through sustained matching loss");
    require(search.observe(false, false, true, true), "Ten failed active comparisons resume the search");
    search.reset(); require(!search.matched(), "Playback changes reset the search budget");

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
    std::cout << "PASS: delay detection, ambiguity, confidence, stream search budget, timing jumps, resampling, and audio buffering\n";
}
