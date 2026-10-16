// SPDX-License-Identifier: GPL-2.0-or-later
#include "playback.hpp"
#include <cstdlib>
#include <iostream>

void require(bool value, const char *message)
{
    if (!value) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); }
}

constexpr size_t rate = 48000, block = 480;
constexpr uint64_t origin = 1000000000000ULL;
constexpr double pi = 3.14159265358979323846;
float tone(int64_t i) { return float(.3 * std::sin(2 * pi * 440 * double(i) / rate)); }
double envelope(double frame) {
    const double t = frame / rate;
    return .6 + .25 * std::sin(2 * pi * 1.7 * t) + .15 * std::sin(2 * pi * .71 * t);
}

void correction(double direction)
{
    vaobs::ElasticBuffer buffer;
    buffer.configure(rate, 2, 6000);
    float left[block], right[block], out_left[block], out_right[block];
    const float *input[]{left, right}; float *output[]{out_left, out_right};
    double extreme = 1, energy = 0, stereo_error = 0;
    size_t crossings = 0, counted = 0;
    float previous = 0;
    std::vector<double> amplitudes, positions;
    for (size_t packet = 0; packet < 6500; ++packet) {
        const double seconds = double(packet) / 100;
        const int64_t frame = int64_t(packet * block);
        const uint64_t time = origin + packet * 10000000ULL;
        for (size_t i = 0; i < block; ++i) {
            left[i] = float(tone(frame + int64_t(i)) * envelope(double(frame + int64_t(i))));
            right[i] = -.37f * left[i];
        }
        buffer.write(input, block, time);
        const double target = 1000 + (seconds >= 6 ? direction * 300 : 0);
        buffer.render(output, block, time, true, target);
        const double speed = buffer.speed();
        require(speed >= .98 - 1e-9 && speed <= 1.02 + 1e-9, "Correction speed stays bounded to two percent");
        if (std::abs(speed - 1) > std::abs(extreme - 1)) extreme = speed;
        if (seconds >= 8 && seconds < 20) {
            double packet_energy = 0;
            for (size_t i = 0; i < block; ++i) {
                packet_energy += double(out_left[i]) * out_left[i];
                stereo_error = std::max(stereo_error, std::abs(double(out_right[i] + .37f * out_left[i])));
                if (previous <= 0 && out_left[i] > 0) ++crossings;
                previous = out_left[i]; ++counted;
            }
            require(packet_energy / block > .0005, "Correction never inserts a silent audio packet");
            energy += packet_energy;
        }
        if (seconds >= 55) {
            double power = 0;
            for (float v : out_left) power += double(v) * v;
            amplitudes.push_back(std::sqrt(power / block) / (.3 / std::sqrt(2.0)));
            positions.push_back(double(frame + int64_t(block / 2)) - target * rate / 1000);
        }
    }
    std::cout << "correction " << direction << ": delay=" << buffer.delay_ms()
              << " speed=" << extreme << " pitch=" << double(crossings) * rate / counted << " Hz\n";
    require(direction > 0 ? extreme < .99 : extreme > 1.01, "Early audio slows down and late audio speeds up");
    require(std::abs(buffer.delay_ms() - (1000 + direction * 300)) < 5,
        "Both correction directions converge to the requested delay");
    require(std::abs(double(crossings) * rate / counted - 440) < 2,
        "Tempo correction preserves the original pitch");
    require(stereo_error < 1e-5 && energy > 0, "Channels stay phase coherent during time stretching");
    double best_error = 1e9; int measured_offset_ms = 0;
    for (int lag = -100; lag <= 100; ++lag) {
        double error = 0;
        for (size_t i = 0; i < amplitudes.size(); ++i) {
            const double delta = amplitudes[i] - envelope(positions[i] + lag * 48);
            error += delta * delta;
        }
        if (error < best_error) { best_error = error; measured_offset_ms = lag; }
    }
    std::cout << "measured content offset=" << measured_offset_ms << " ms\n";
    require(std::abs(measured_offset_ms) <= 10,
        "Actual output content converges, including stretcher buffering and ratio changes");
}

int main()
{
    correction(1); correction(-1);
    vaobs::ElasticBuffer buffer;
    buffer.configure(rate, 1, 10000);
    float input[block], output[block];
    const float *in[]{input}; float *out[]{output};
    double maximum_error = 0;
    for (size_t packet = 0; packet < 7000; ++packet) {
        const int64_t frame = int64_t(packet * block);
        const uint64_t time = origin + packet * 10000000ULL;
        for (size_t i = 0; i < block; ++i) input[i] = tone(frame + int64_t(i));
        buffer.write(in, block, time);
        // Then follow a genuine clock drift, including the feed-forward term.
        const double drift = packet >= 600 ? 2 : 0;
        const double delay = 1000 + (packet >= 600 ? double(packet - 600) * .02 : 0);
        buffer.render(out, block, time, true, delay, drift);
        if (packet >= 200 && packet < 500)
            for (size_t i = 0; i < block; ++i)
                maximum_error = std::max(maximum_error, std::abs(double(output[i] - tone(frame + int64_t(i) - 48000))));
    }
    std::cout << "unity error=" << maximum_error << ", drift delay=" << buffer.delay_ms() << '\n';
    require(maximum_error < 1e-4, "Unity tempo keeps exact sample alignment after stretcher latency compensation");
    require(std::abs(buffer.delay_ms() - 1128) < 5 && buffer.speed() < 1,
        "Continuous drift stays aligned without repeated jumps");
    // A real seek uses a short crossfade, then stabilizes at the new position.
    const uint64_t time = origin + 70000000000ULL;
    buffer.render(out, block, time, true, 5000);
    require(std::abs(buffer.delay_ms() - 5000) < 1, "Large confirmed jumps recover immediately");
    for (size_t i = 1; i <= 6; ++i) buffer.render(out, block, time + i * 10000000ULL, false, 5000);
    require(std::all_of(std::begin(output), std::end(output), [](float v) { return v == 0; }),
        "Actual loss of alignment still fades output to silence");
    buffer.configure(rate, 1, 1000);
    for (size_t packet = 0; packet < 3000; ++packet) {
        const int64_t frame = int64_t(packet * block);
        const uint64_t stamp = origin + packet * 10000000ULL;
        for (size_t i = 0; i < block; ++i) input[i] = tone(frame + int64_t(i));
        buffer.write(in, block, stamp);
        const double delay = packet < 1500 ? std::max(0.0, 100 - double(packet) / 10) : std::min(110.0, double(packet - 1500) / 10);
        buffer.render(out, block, stamp, true, delay);
        if (packet > 200) {
            double energy = 0;
            for (float v : output) energy += double(v) * v;
            require(energy / block > 1e-5, "Transitions to and from near-zero-delay playback do not mute the stream");
        }
    }
    std::cout << "PASS: continuous correction, preserved pitch, stereo coherence, drift tracking, seeks, and fade-out\n";
}
