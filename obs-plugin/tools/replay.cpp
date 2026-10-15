// SPDX-License-Identifier: GPL-2.0-or-later
// Offline exercise of the exact live matching algorithm against decoded audio.
#include "alignment.hpp"
#include <fstream>
#include <iostream>
#include <iterator>

std::vector<float> read_pcm(const char *path)
{
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error(std::string("Could not open ") + path);
    const size_t bytes = size_t(input.tellg());
    if (bytes % sizeof(float)) throw std::runtime_error("Expected raw mono float32 PCM at 2 kHz");
    std::vector<float> data(bytes / sizeof(float));
    input.seekg(0); input.read(reinterpret_cast<char *>(data.data()), std::streamsize(bytes));
    return data;
}

int main(int argc, char **argv)
{
    if (argc != 3 && argc != 4) { std::cerr << "Usage: vaobs-replay.exe reference.f32 feed.f32 [sample-rate]\n"; return 2; }
    try {
        auto reference = read_pcm(argv[1]), feed = read_pcm(argv[2]);
        const int rate = argc == 4 ? std::stoi(argv[3]) : vaobs::analysis_rate;
        if (rate < 8000 && rate != vaobs::analysis_rate) throw std::runtime_error("Unsupported sample rate");
        if (rate != vaobs::analysis_rate) {
            auto resample = [rate](const std::vector<float> &input) {
                vaobs::Timeline timeline(input.size() * vaobs::analysis_rate / rate + 2);
                const float *planes[]{input.data()};
                timeline.append(planes, 1, input.size(), uint32_t(rate), 0);
                std::vector<float> values; std::vector<uint8_t> valid;
                timeline.read(0, size_t(timeline.end()), values, valid);
                return values;
            };
            reference = resample(reference); feed = resample(feed);
        }
        vaobs::Lock lock;
        size_t matches = 0;
        for (int seconds = 5; size_t(seconds * vaobs::analysis_rate) < std::min(reference.size(), feed.size()); ++seconds) {
            const size_t end = size_t(seconds * vaobs::analysis_rate);
            const size_t n = 4 * vaobs::analysis_rate, max_lag = std::min(size_t(60 * vaobs::analysis_rate), end - n);
            std::vector<float> ref(reference.begin() + end - n, reference.begin() + end);
            std::vector<float> target(feed.begin() + end - n - max_lag, feed.begin() + end);
            auto match = vaobs::estimate(ref, target, std::vector<uint8_t>(target.size(), 1));
            const bool was_locked = lock.locked;
            const double previous = lock.delay_ms;
            lock.observe(match);
            if (match.reliable) ++matches;
            if ((lock.locked && (!was_locked || previous != lock.delay_ms)) || (was_locked && !lock.locked))
                std::cout << seconds << "s: " << (lock.locked ? "aligned" : "searching") << ", delay="
                          << lock.delay_ms << "ms, correlation=" << match.correlation << '\n';
        }
        std::cout << "Reliable matching windows: " << matches << '\n';
        return matches ? 0 : 1;
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
