// SPDX-License-Identifier: GPL-2.0-or-later
// Real libobs, isolated from OBS Studio's profiles and scene collections.
#include "obs-api.hpp"
#include "alignment.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

template<typename T> T load_api(HMODULE module, const char *name)
{
    auto fn = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!fn) throw std::runtime_error(std::string("Missing libobs API: ") + name);
    return fn;
}
#define LOAD(name) auto name##_fn = load_api<decltype(&::name)>(module, #name)

struct Probe {
    const std::vector<float> *music;
    std::atomic<int64_t> origin_frame{0};
    size_t good_frames = 0, tail_frames = 0, active_blocks = 0;
    double error = 0, energy = 0;
    std::atomic<bool> should_be_silent{false};
    bool reset_silent = true;
};

void capture(void *data, obs_source_t *, const audio_data *audio, bool)
{
    auto &probe = *static_cast<Probe *>(data);
    const auto *samples = reinterpret_cast<const float *>(audio->data[0]);
    double energy = 0;
    for (size_t i = 0; i < audio->frames; ++i) energy += samples[i] * samples[i];
    if (probe.should_be_silent) { if (energy > 1e-12) probe.reset_silent = false; return; }
    if (energy < 1e-12) return;
    if (++probe.active_blocks < 100) return;
    const int64_t start = vaobs::TimedBuffer::frame_at(audio->timestamp, 48000) - probe.origin_frame.load();
    for (size_t i = 0; i < audio->frames; ++i) {
        const int64_t index = start + int64_t(i);
        if (index < 0 || index >= int64_t(probe.music->size())) continue;
        // Feed packets stop at 28 s; its final received content ends at
        // reference time 28 s + 21.149 s. The output must then become silence.
        const int64_t available_end = 28 * 48000 + 1015152;
        const double expected = index < available_end ? (*probe.music)[size_t(index)] / 17 : 0;
        const double delta = samples[i] / 17 - expected;
        probe.error += delta * delta; probe.energy += expected * expected; ++probe.good_frames;
        if (index >= 29 * 48000 && index < available_end) ++probe.tail_frames;
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) { std::cerr << "Usage: obs-integration-test.exe OBS-bin-directory plugin-dll\n"; return 2; }
    try {
        const std::string binary = std::string(argv[1]) + "\\obs.dll";
        HMODULE module = LoadLibraryExA(binary.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module || !obsapi::load()) throw std::runtime_error("Could not load installed libobs");
        LOAD(obs_startup); LOAD(obs_shutdown); LOAD(obs_reset_audio);
        LOAD(obs_source_create); LOAD(obs_source_update); LOAD(obs_source_set_muted);
        LOAD(obs_open_module); LOAD(obs_init_module); LOAD(obs_set_output_source);
        LOAD(obs_data_create); LOAD(obs_data_set_string); LOAD(obs_data_set_int); LOAD(obs_data_release);
        if (!obs_startup_fn("en-US", nullptr, nullptr)) throw std::runtime_error("libobs startup failed");
        obs_audio_info info{48000, SPEAKERS_STEREO};
        if (!obs_reset_audio_fn(&info)) throw std::runtime_error("libobs audio initialization failed");
        obs_module_t *plugin = nullptr;
        if (obs_open_module_fn(&plugin, argv[2], ".") != 0 || !obs_init_module_fn(plugin))
            throw std::runtime_error("OBS rejected the alignment plugin");
        obs_source_info generator{};
        generator.id = "vaobs_test_audio"; generator.type = OBS_SOURCE_TYPE_INPUT;
        generator.output_flags = OBS_SOURCE_AUDIO;
        generator.get_name = [](void *) { return "VaOBS test signal"; };
        generator.create = [](obs_data_t *, obs_source_t *) -> void * { return new int(1); };
        generator.destroy = [](void *p) { delete static_cast<int *>(p); };
        obsapi::obs_register_source_s(&generator, sizeof(generator));
        auto *reference = obs_source_create_fn(generator.id, "VRChatAudio", nullptr, nullptr);
        auto *feed = obs_source_create_fn(generator.id, "VRChatFeed", nullptr, nullptr);
        obs_source_set_muted_fn(feed, true); // Capture must still expose the signal.
        auto *settings = obs_data_create_fn();
        obs_data_set_string_fn(settings, "reference_source", "VRChatAudio");
        obs_data_set_string_fn(settings, "feed_source", "VRChatFeed");
        obs_data_set_int_fn(settings, "stream_generation", 1);
        constexpr size_t rate = 48000, block = 480, delay = 1015152;
        std::vector<float> music(60 * rate);
        std::mt19937 rng(1234); std::normal_distribution<float> random(0, .25f);
        float smooth = 0;
        for (float &sample : music) { smooth = .7f * smooth + .3f * random(rng); sample = smooth; }
        Probe probe; probe.music = &music;
        auto *aligned = obs_source_create_fn("vaobs_auto_align_source", "VaOBSAlignedAudio", settings, nullptr);
        if (!reference || !feed || !aligned) throw std::runtime_error("Could not create test audio sources");
        obsapi::obs_source_add_audio_capture_callback(aligned, capture, &probe);
        obs_set_output_source_fn(0, aligned);
        float left[block], right[block], reference_left[block], reference_right[block];
        obs_source_audio audio{};
        audio.format = AUDIO_FORMAT_FLOAT_PLANAR; audio.samples_per_sec = rate;
        audio.speakers = SPEAKERS_STEREO; audio.frames = block;
        const auto start = std::chrono::steady_clock::now();
        const int64_t origin_frame = vaobs::TimedBuffer::frame_at(obsapi::os_gettime_ns(), rate);
        probe.origin_frame = origin_frame;
        for (size_t packet = 0; packet < 5000; ++packet) {
            std::this_thread::sleep_until(start + std::chrono::milliseconds(packet * 10));
            const size_t offset = packet * block;
            for (size_t i = 0; i < block; ++i) {
                reference_left[i] = music[offset + i] / 17; reference_right[i] = reference_left[i] * .7f;
                left[i] = packet < 2800 ? music[offset + i + delay] : 0;
                right[i] = left[i] * .7f;
            }
            const int64_t position = origin_frame + int64_t(offset);
            audio.timestamp = uint64_t(position / rate) * 1000000000ULL + uint64_t(position % rate) * 1000000000ULL / rate;
            audio.data[0] = reinterpret_cast<uint8_t *>(reference_left); audio.data[1] = reinterpret_cast<uint8_t *>(reference_right);
            obsapi::obs_source_output_audio(reference, &audio);
            if (packet < 2800) {
                audio.data[0] = reinterpret_cast<uint8_t *>(left); audio.data[1] = reinterpret_cast<uint8_t *>(right);
                obsapi::obs_source_output_audio(feed, &audio);
            }
        }
        obs_data_set_int_fn(settings, "stream_generation", 2);
        obs_source_update_fn(aligned, settings);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        probe.should_be_silent = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        obsapi::obs_source_remove_audio_capture_callback(aligned, capture, &probe);
        const double error = probe.error / std::max(probe.energy, 1e-20);
        const bool pass = probe.good_frames >= rate && probe.tail_frames >= 19 * rate && error < 1e-4 && probe.reset_silent;
        std::cout << "Aligned frames=" << probe.good_frames << ", frames after feed stopped=" << probe.tail_frames << ", relative error=" << error << '\n';
        obs_set_output_source_fn(0, nullptr);
        obsapi::obs_source_release(aligned); obsapi::obs_source_release(feed); obsapi::obs_source_release(reference);
        obs_data_release_fn(settings); obs_shutdown_fn();
        if (!pass) throw std::runtime_error("Live alignment, independent tail playback, or reset failed");
        std::cout << "PASS: OBS aligned 21,149 ms, continued after feed packets stopped, and reset/muted correctly\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
