// SPDX-License-Identifier: GPL-2.0-or-later
// Real libobs, isolated from OBS Studio's profiles and scene collections.
#include "obs-api.hpp"
#include "alignment.hpp"
#include "logs.hpp"
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
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

struct TestClient {
    HANDLE process = nullptr;
    std::filesystem::path directory;
    explicit TestClient(const std::filesystem::path &profile) : directory(profile)
    {
        wchar_t self[32768]{};
        GetModuleFileNameW(nullptr, self, DWORD(std::size(self)));
        const auto binary = profile / "client" / "VRChat.exe";
        std::filesystem::create_directories(binary.parent_path());
        std::filesystem::copy_file(self, binary, std::filesystem::copy_options::overwrite_existing);
        std::wstring command = L"\"" + binary.wstring() + L"\" --client";
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        if (!CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
                nullptr, nullptr, &startup, &info)) throw std::runtime_error("Start isolated client");
        process = info.hProcess; CloseHandle(info.hThread);
    }
    ~TestClient()
    {
        TerminateProcess(process, 0); WaitForSingleObject(process, 5000); CloseHandle(process);
        std::error_code error;
        std::filesystem::remove_all(directory, error);
    }
};

struct Probe {
    const std::vector<float> *music;
    std::atomic<int64_t> origin_frame{0};
    size_t good_frames = 0, tail_frames = 0, active_blocks = 0;
    double error = 0, energy = 0;
    std::atomic<bool> should_be_silent{false};
    bool reset_silent = true;
};

struct Generator {
    obs_source_t *source;
    bool check;
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
        // Feed packets stop at 42 s; its final received content ends at
        // reference time 42 s + 21.149 s. The output must then become silence.
        const int64_t available_end = 42 * 48000 + 1015152;
        const double expected = index < available_end ? (*probe.music)[size_t(index)] / 17 : 0;
        const double delta = samples[i] / 17 - expected;
        probe.error += delta * delta; probe.energy += expected * expected; ++probe.good_frames;
        if (index >= 43 * 48000 && index < available_end) ++probe.tail_frames;
    }
}

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--client") { for (;;) Sleep(1000); }
    if (argc != 3) { std::cerr << "Usage: obs-integration-test.exe OBS-bin-directory plugin-dll\n"; return 2; }
    try {
        const auto isolated_profile = std::filesystem::temp_directory_path() /
            ("vaobs-audio-test-" + std::to_string(GetCurrentProcessId()));
        SetEnvironmentVariableW(L"USERPROFILE", isolated_profile.c_str());
        TestClient client(isolated_profile);
        const auto logs = vaobs::vrchat_log_directory();
        std::filesystem::create_directories(logs);
        std::ofstream(logs / "output_log_signal.txt") << "[Video Playback] Resolving URL 'test://signal'\n";
        const std::string binary = std::string(argv[1]) + "\\obs.dll";
        HMODULE module = LoadLibraryExA(binary.c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!module || !obsapi::load()) throw std::runtime_error("Could not load installed libobs");
        LOAD(obs_startup); LOAD(obs_shutdown); LOAD(obs_reset_audio); LOAD(obs_reset_video);
        LOAD(obs_source_create); LOAD(obs_source_update);
        LOAD(obs_open_module); LOAD(obs_init_module); LOAD(obs_set_output_source);
        LOAD(obs_data_create); LOAD(obs_data_set_string); LOAD(obs_data_set_int); LOAD(obs_data_release);
        LOAD(obs_source_properties); LOAD(obs_property_description); LOAD(obs_properties_destroy);
        LOAD(obs_property_list_item_count); LOAD(obs_property_list_item_disabled);
        LOAD(obs_source_get_signal_handler); LOAD(signal_handler_connect); LOAD(signal_handler_disconnect);
        if (!obs_startup_fn("en-US", nullptr, nullptr)) throw std::runtime_error("libobs startup failed");
        const auto bin = std::filesystem::absolute(argv[1]);
        std::filesystem::current_path(bin);
        const auto graphics = (bin / "libobs-d3d11.dll").u8string();
        obs_video_info video{};
        video.graphics_module = graphics.c_str(); video.fps_num = 30; video.fps_den = 1;
        video.base_width = video.output_width = video.base_height = video.output_height = 128;
        video.output_format = VIDEO_FORMAT_RGBA;
        if (obs_reset_video_fn(&video) != OBS_VIDEO_SUCCESS) throw std::runtime_error("libobs video initialization failed");
        obs_audio_info info{48000, SPEAKERS_STEREO};
        if (!obs_reset_audio_fn(&info)) throw std::runtime_error("libobs audio initialization failed");
        obs_module_t *plugin = nullptr;
        if (obs_open_module_fn(&plugin, argv[2], ".") != 0 || !obs_init_module_fn(plugin))
            throw std::runtime_error("OBS rejected the alignment plugin");
        obs_source_info generator{};
        generator.id = "ffmpeg_source"; generator.type = OBS_SOURCE_TYPE_INPUT;
        generator.output_flags = OBS_SOURCE_AUDIO;
        generator.get_name = [](void *) { return "VaOBS test signal"; };
        generator.create = [](obs_data_t *, obs_source_t *source) -> void * {
            return new Generator{source, std::string(obsapi::obs_source_get_name(source)) == "VaOBS availability check"};
        };
        generator.destroy = [](void *p) { delete static_cast<Generator *>(p); };
        generator.video_tick = [](void *data, float) {
            auto &item = *static_cast<Generator *>(data);
            if (!item.check) return;
            float silence[480]{};
            obs_source_audio audio{}; audio.data[0] = audio.data[1] = reinterpret_cast<uint8_t *>(silence);
            audio.frames = 480; audio.samples_per_sec = 48000; audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
            audio.speakers = SPEAKERS_STEREO; audio.timestamp = obsapi::os_gettime_ns();
            obsapi::obs_source_output_audio(item.source, &audio);
        };
        obsapi::obs_register_source_s(&generator, sizeof(generator));
        auto *reference = obs_source_create_fn(generator.id, "VRChatAudio", nullptr, nullptr);
        auto *settings = obs_data_create_fn();
        obs_data_set_string_fn(settings, "reference_source", "VRChatAudio");
        obs_data_set_int_fn(settings, "stream_generation", 1);
        constexpr size_t rate = 48000, block = 480, delay = 1015152;
        std::vector<float> music(90 * rate);
        std::mt19937 rng(1234); std::normal_distribution<float> random(0, .25f);
        float smooth = 0;
        for (float &sample : music) { smooth = .7f * smooth + .3f * random(rng); sample = smooth; }
        Probe probe; probe.music = &music;
        auto *aligned = obs_source_create_fn("vaobs_auto_align_source", "VaOBSAlignedAudio", settings, nullptr);
        obs_source_t *feed = nullptr;
        obsapi::obs_source_enum_active_sources(aligned, [](obs_source_t *, obs_source_t *child, void *data) {
            *static_cast<obs_source_t **>(data) = child;
        }, &feed);
        if (!reference || !feed || !aligned) throw std::runtime_error("Could not create test audio sources");
        const auto bound_by = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        for (;;) {
            auto *current = obsapi::obs_source_get_settings(feed);
            const bool bound = std::string(obsapi::obs_data_get_string(current, "input")) == "test://signal";
            obsapi::obs_data_release(current);
            if (bound) break;
            if (std::chrono::steady_clock::now() > bound_by) throw std::runtime_error("Private player did not discover video URL");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        std::atomic<int> refreshes{0};
        auto status_changed = [](void *data, calldata_t *) { ++*static_cast<std::atomic<int> *>(data); };
        auto *signals = obs_source_get_signal_handler_fn(aligned);
        signal_handler_connect_fn(signals, "update_properties", status_changed, &refreshes);
        obsapi::obs_source_add_audio_capture_callback(aligned, capture, &probe);
        obs_set_output_source_fn(0, aligned);
        float left[block], right[block], reference_left[block], reference_right[block];
        obs_source_audio audio{};
        audio.format = AUDIO_FORMAT_FLOAT_PLANAR; audio.samples_per_sec = rate;
        audio.speakers = SPEAKERS_STEREO; audio.frames = block;
        const auto start = std::chrono::steady_clock::now();
        const int64_t origin_frame = vaobs::TimedBuffer::frame_at(obsapi::os_gettime_ns(), rate);
        probe.origin_frame = origin_frame;
        for (size_t packet = 0; packet < 6500; ++packet) {
            std::this_thread::sleep_until(start + std::chrono::milliseconds(packet * 10));
            const size_t offset = packet * block;
            for (size_t i = 0; i < block; ++i) {
                // Silence for long enough that the old ten-failure policy
                // would interrupt the otherwise valid buffered audio.
                reference_left[i] = packet >= 3000 && packet < 4500 ? 0 : music[offset + i] / 17;
                reference_right[i] = reference_left[i] * .7f;
                left[i] = packet < 4200 ? music[offset + i + delay] : 0;
                right[i] = left[i] * .7f;
            }
            const int64_t position = origin_frame + int64_t(offset);
            audio.timestamp = uint64_t(position / rate) * 1000000000ULL + uint64_t(position % rate) * 1000000000ULL / rate;
            audio.data[0] = reinterpret_cast<uint8_t *>(reference_left); audio.data[1] = reinterpret_cast<uint8_t *>(reference_right);
            obsapi::obs_source_output_audio(reference, &audio);
            if (packet < 4200) {
                audio.data[0] = reinterpret_cast<uint8_t *>(left); audio.data[1] = reinterpret_cast<uint8_t *>(right);
                obsapi::obs_source_output_audio(feed, &audio);
            }
        }
        auto *props = obs_source_properties_fn(aligned);
        const std::string status = obs_property_description_fn(obsapi::obs_properties_get(props, "status"));
        obs_properties_destroy_fn(props);
        const bool aligned_status = status.find("Aligned -") != std::string::npos;
        if (refreshes < 1 || !aligned_status || status.find("21149") == std::string::npos)
            throw std::runtime_error("Aligned status did not update automatically: " + status +
                " (refreshes: " + std::to_string(refreshes.load()) + ")");
        signal_handler_disconnect_fn(signals, "update_properties", status_changed, &refreshes);
        obs_data_set_int_fn(settings, "stream_generation", 2);
        obs_source_update_fn(aligned, settings);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        probe.should_be_silent = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        obsapi::obs_source_remove_audio_capture_callback(aligned, capture, &probe);
        const double error = probe.error / std::max(probe.energy, 1e-20);
        const bool pass = probe.good_frames >= rate && probe.tail_frames >= 19 * rate && error < 1e-4 && probe.reset_silent;
        std::cout << "Aligned frames=" << probe.good_frames << ", frames after feed stopped=" << probe.tail_frames << ", relative error=" << error << '\n';
        if (!pass) throw std::runtime_error("Live alignment, independent tail playback, or reset failed");
        std::cout << "PASS: private player aligned 21,149 ms, held through reference silence, drained its audio tail, and reset/muted correctly\n";

        // Three online players: the newest is wrong, the second matches, and
        // later the reference moves to the third. Exercise the real analyst,
        // watcher and PCM callbacks together, rather than injecting feedback.
        std::ofstream(logs / "output_log_signal.txt", std::ios::app)
            << "[Behaviour] Joining wrld_search:1\n"
               "[Video Playback] Resolving URL 'test://two'\n"
               "[Video Playback] Resolving URL 'test://one'\n"
               "[Video Playback] Resolving URL 'test://wrong'\n";
        obs_data_set_string_fn(settings, "video_selection", "test://wrong");
        obs_data_set_int_fn(settings, "max_delay_ms", 1000);
        obs_data_set_int_fn(settings, "window_ms", 2000);
        obs_source_update_fn(aligned, settings);
        auto current_url = [&] {
            auto *data = obsapi::obs_source_get_settings(feed);
            const std::string url = obsapi::obs_data_get_string(data, "input");
            obsapi::obs_data_release(data); return url;
        };
        const auto checked_by = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        for (;;) {
            auto *p = obs_source_properties_fn(aligned);
            auto *list = obsapi::obs_properties_get(p, "video_selection");
            const auto count = obs_property_list_item_count_fn(list);
            bool checked = count == 4;
            for (size_t i = 0; i < count; ++i) checked = checked && !obs_property_list_item_disabled_fn(list, i);
            obs_properties_destroy_fn(p);
            if (checked && current_url() == "test://wrong") break;
            if (std::chrono::steady_clock::now() > checked_by) throw std::runtime_error("Search candidates were not checked");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        obs_data_set_string_fn(settings, "video_selection", "latest");
        obs_data_set_int_fn(settings, "stream_generation", 3);
        obs_source_update_fn(aligned, settings);
        std::vector<float> other(music.size()), unrelated(music.size());
        for (float &v : other) v = random(rng);
        for (float &v : unrelated) v = random(rng);
        bool first_match = false, second_match = false;
        struct SearchOutput {
            std::atomic<bool> expect_silence{false}, silent{true};
            static void capture(void *data, obs_source_t *, const audio_data *audio, bool) {
                auto &self = *static_cast<SearchOutput *>(data);
                if (!self.expect_silence) return;
                const auto *samples = reinterpret_cast<const float *>(audio->data[0]);
                for (size_t i = 0; i < audio->frames; ++i) if (std::abs(samples[i]) > 1e-6f) self.silent = false;
            }
        } search_output;
        obsapi::obs_source_add_audio_capture_callback(aligned, SearchOutput::capture, &search_output);
        const auto search_start = std::chrono::steady_clock::now();
        const int64_t search_origin = vaobs::TimedBuffer::frame_at(obsapi::os_gettime_ns(), rate);
        for (size_t packet = 0; packet < 6200; ++packet) {
            std::this_thread::sleep_until(search_start + std::chrono::milliseconds(packet * 10));
            const auto url = current_url();
            if (packet < 300 && url != "test://wrong")
                throw std::runtime_error("Automatic search abandoned a candidate before testing the full delay range");
            if (packet == 1200)
                std::ofstream(logs / "output_log_signal.txt", std::ios::app)
                    << "[Video Playback] Resolving URL 'test://wrong'\n";
            if (packet == 1500 && (!first_match || url != "test://one"))
                throw std::runtime_error("Automatic search did not retain the matching player through another player's log update");
            if (packet == 5500) {
                if (!second_match) throw std::runtime_error("Sustained loss did not find the other matching player");
                obs_data_set_string_fn(settings, "video_selection", "test://wrong");
                obs_source_update_fn(aligned, settings);
            }
            if (packet > 5600 && url != "test://wrong") throw std::runtime_error("Manual URL cycled despite being pinned");
            if (packet > 5600) search_output.expect_silence = true;
            if (packet % 100 == 0) {
                auto *p = obs_source_properties_fn(aligned);
                const std::string text = obs_property_description_fn(obsapi::obs_properties_get(p, "status"));
                obs_properties_destroy_fn(p);
                if (text.find("Aligned - 800 ms") != std::string::npos) {
                    if (url == "test://one" && packet < 1600) first_match = true;
                    if (url == "test://two" && packet > 1600) second_match = true;
                }
            }
            const size_t offset = packet * block;
            const auto &reference_music = packet < 1600 ? music : other;
            const auto &feed_music = url == "test://one" ? music : url == "test://two" ? other : unrelated;
            for (size_t i = 0; i < block; ++i) {
                reference_left[i] = reference_music[offset + i] / 17;
                reference_right[i] = reference_left[i] * .7f;
                left[i] = feed_music[offset + i + 38400]; right[i] = left[i] * .7f;
            }
            const int64_t position = search_origin + int64_t(offset);
            audio.timestamp = uint64_t(position / rate) * 1000000000ULL + uint64_t(position % rate) * 1000000000ULL / rate;
            audio.data[0] = reinterpret_cast<uint8_t *>(reference_left); audio.data[1] = reinterpret_cast<uint8_t *>(reference_right);
            obsapi::obs_source_output_audio(reference, &audio);
            audio.data[0] = reinterpret_cast<uint8_t *>(left); audio.data[1] = reinterpret_cast<uint8_t *>(right);
            obsapi::obs_source_output_audio(feed, &audio);
        }
        obsapi::obs_source_remove_audio_capture_callback(aligned, SearchOutput::capture, &search_output);
        if (!first_match || !second_match || !search_output.silent)
            throw std::runtime_error("Automatic stream selection or unmatched output muting failed");
        std::cout << "PASS: automatic search skips wrong audio, retains a match, recovers after matching loss, and leaves manual URLs pinned and silent\n";

        // Quiet music mixed with other game audio must get longer confirmation,
        // without cycling away or releasing unconfirmed audio into the mixer.
        std::ofstream(logs / "output_log_signal.txt", std::ios::app)
            << "[Behaviour] Joining wrld_weak:1\n"
               "[Video Playback] Resolving URL 'test://fallback'\n"
               "[Video Playback] Resolving URL 'test://weak'\n";
        obs_data_set_string_fn(settings, "video_selection", "test://weak");
        obs_source_update_fn(aligned, settings);
        const auto weak_checked_by = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        for (;;) {
            auto *p = obs_source_properties_fn(aligned);
            auto *list = obsapi::obs_properties_get(p, "video_selection");
            const auto count = obs_property_list_item_count_fn(list);
            bool checked = count == 3;
            for (size_t i = 0; i < count; ++i) checked = checked && !obs_property_list_item_disabled_fn(list, i);
            obs_properties_destroy_fn(p);
            if (checked && current_url() == "test://weak") break;
            if (std::chrono::steady_clock::now() > weak_checked_by) throw std::runtime_error("Weak-match candidates were not checked");
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        obs_data_set_string_fn(settings, "video_selection", "latest");
        obs_data_set_int_fn(settings, "stream_generation", 4);
        obs_source_update_fn(aligned, settings);
        smooth = 0;
        for (float &v : unrelated) { smooth = .7f * smooth + .3f * v; v = smooth; }
        struct WeakOutput {
            std::atomic<bool> early{true}, silent{true}, heard{false};
            static void capture(void *data, obs_source_t *, const audio_data *audio, bool) {
                auto &self = *static_cast<WeakOutput *>(data);
                const auto *samples = reinterpret_cast<const float *>(audio->data[0]);
                for (size_t i = 0; i < audio->frames; ++i) if (std::abs(samples[i]) > 1e-6f) {
                    if (self.early) self.silent = false; else self.heard = true;
                }
            }
        } weak_output;
        obsapi::obs_source_add_audio_capture_callback(aligned, WeakOutput::capture, &weak_output);
        bool weak_synced = false, weak_aligned = false;
        const auto weak_start = std::chrono::steady_clock::now();
        const int64_t weak_origin = vaobs::TimedBuffer::frame_at(obsapi::os_gettime_ns(), rate);
        for (size_t packet = 0; packet < 4400; ++packet) {
            std::this_thread::sleep_until(weak_start + std::chrono::milliseconds(packet * 10));
            if (current_url() != "test://weak") throw std::runtime_error("Automatic search abandoned a weak match during confirmation");
            if (packet == 3300) weak_output.early = false;
            if (packet % 100 == 0) {
                auto *p = obs_source_properties_fn(aligned);
                const std::string text = obs_property_description_fn(obsapi::obs_properties_get(p, "status"));
                obs_properties_destroy_fn(p);
                weak_synced = weak_synced || text == "Syncing...";
                if (text.find("Aligned - 800 ms") != std::string::npos) {
                    if (packet < 3300) throw std::runtime_error("Weak audio aligned before three independent passages");
                    weak_aligned = true;
                }
            }
            const size_t offset = packet * block;
            for (size_t i = 0; i < block; ++i) {
                reference_left[i] = (music[offset + i] + .4f * unrelated[offset + i]) / 17;
                reference_right[i] = reference_left[i] * .7f;
                left[i] = music[offset + i + 38400]; right[i] = left[i] * .7f;
            }
            const int64_t position = weak_origin + int64_t(offset);
            audio.timestamp = uint64_t(position / rate) * 1000000000ULL + uint64_t(position % rate) * 1000000000ULL / rate;
            audio.data[0] = reinterpret_cast<uint8_t *>(reference_left); audio.data[1] = reinterpret_cast<uint8_t *>(reference_right);
            obsapi::obs_source_output_audio(reference, &audio);
            audio.data[0] = reinterpret_cast<uint8_t *>(left); audio.data[1] = reinterpret_cast<uint8_t *>(right);
            obsapi::obs_source_output_audio(feed, &audio);
        }
        obsapi::obs_source_remove_audio_capture_callback(aligned, WeakOutput::capture, &weak_output);
        if (!weak_synced || !weak_aligned || !weak_output.silent || !weak_output.heard)
            throw std::runtime_error("Weak-match confirmation, output muting, or delayed playback failed");
        std::cout << "PASS: weak matches stay selected, confirm distinct passages, and release audio only after alignment\n";
        obs_set_output_source_fn(0, nullptr);
        obsapi::obs_source_release(aligned); obsapi::obs_source_release(reference);
        obs_data_release_fn(settings); obs_shutdown_fn();
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
