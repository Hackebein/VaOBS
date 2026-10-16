// SPDX-License-Identifier: GPL-2.0-or-later
// Real libobs lifecycle: a hidden/muted capture must remain active while its
// VaOBS parent is active, including after switching scenes and references.
#include "obs-api.hpp"
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <thread>

template<typename T> T load(HMODULE module, const char *name) {
    const auto result = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!result) throw std::runtime_error(name);
    return result;
}
#define LOAD(name) auto name = load<decltype(&::name)>(module, #name)
void require(bool good, const char *message) { if (!good) throw std::runtime_error(message); }
template<typename F> void wait_for(F test, const char *message) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!test()) {
        require(std::chrono::steady_clock::now() < deadline, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
int main(int argc, char **argv) {
    if (argc != 3) return 2;
    try {
        const auto profile = std::filesystem::temp_directory_path() /
            ("vaobs-reference-test-" + std::to_string(GetCurrentProcessId()));
        SetEnvironmentVariableW(L"USERPROFILE", profile.c_str());
        HMODULE module = LoadLibraryExW((std::filesystem::path(argv[1]) / L"obs.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(module && obsapi::load(), "Load installed libobs");
        LOAD(obs_startup); LOAD(obs_shutdown); LOAD(obs_reset_audio); LOAD(obs_reset_video);
        LOAD(obs_open_module); LOAD(obs_init_module); LOAD(obs_source_create);
        LOAD(obs_set_output_source); LOAD(obs_source_active); LOAD(obs_source_showing);
        require(obs_startup("en-US", nullptr, nullptr), "Start libobs");
        const auto bin = std::filesystem::absolute(argv[1]);
        std::filesystem::current_path(bin);
        const auto graphics = (bin / "libobs-d3d11.dll").u8string();
        obs_video_info video{};
        video.graphics_module = graphics.c_str(); video.fps_num = 30; video.fps_den = 1;
        video.base_width = video.output_width = video.base_height = video.output_height = 128;
        video.output_format = VIDEO_FORMAT_RGBA;
        require(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "Initialize video ticks");
        obs_audio_info audio{48000, SPEAKERS_STEREO};
        require(obs_reset_audio(&audio), "Initialize audio");
        obs_module_t *plugin = nullptr;
        require(obs_open_module(&plugin, argv[2], ".") == 0 && obs_init_module(plugin), "Load VaOBS");
        obs_source_info fake{};
        fake.id = "ffmpeg_source"; fake.type = OBS_SOURCE_TYPE_INPUT; fake.output_flags = OBS_SOURCE_AUDIO;
        fake.get_name = [](void *) { return "Inactive capture"; };
        fake.create = [](obs_data_t *, obs_source_t *source) -> void * { return source; };
        fake.destroy = [](void *) {};
        obsapi::obs_register_source_s(&fake, sizeof(fake));
        auto *reference = obs_source_create(fake.id, "VRChatAudio", nullptr, nullptr);
        auto *second = obs_source_create(fake.id, "OtherReference", nullptr, nullptr);
        obsapi::obs_source_set_muted(reference, true);
        obsapi::obs_source_set_volume(reference, .37f);
        obsapi::obs_source_set_audio_mixers(reference, 0);
        require(!obs_source_active(reference) && !obs_source_showing(reference), "Reference starts hidden and inactive");
        auto *settings = obsapi::obs_data_create();
        obsapi::obs_data_set_string(settings, "reference_source", "VRChatAudio");
        auto *aligned = obs_source_create("vaobs_auto_align_source", "VaOBS", settings, nullptr);
        require(aligned, "Create VaOBS");
        obs_set_output_source(0, aligned);
        wait_for([&] { return obs_source_active(reference) && obs_source_showing(reference); },
            "VaOBS must activate a hidden timing reference");
        require(obsapi::obs_source_muted(reference) && obsapi::obs_source_get_audio_mixers(reference) == 0 &&
            std::abs(obsapi::obs_source_get_volume(reference) - .37f) < 1e-6,
            "Activation preserves user mute, volume, and recording tracks");
        obs_set_output_source(0, nullptr);
        wait_for([&] { return !obs_source_active(reference) && !obs_source_showing(reference); }, "Inactive VaOBS releases reference activity");
        obs_set_output_source(0, aligned);
        wait_for([&] { return obs_source_active(reference); }, "Returning to VaOBS reactivates its reference");
        obsapi::obs_data_set_string(settings, "reference_source", "OtherReference");
        obsapi::obs_source_update(aligned, settings);
        wait_for([&] { return obs_source_active(second) && !obs_source_active(reference); }, "Changing reference transfers activity cleanly");
        obs_set_output_source(0, nullptr);
        wait_for([&] { return !obs_source_active(second); }, "Stopping VaOBS releases the replacement reference");
        obsapi::obs_source_release(aligned);
        require(!obs_source_showing(second) && !obs_source_showing(reference), "Destroying VaOBS leaves no capture active");
        obsapi::obs_source_release(reference); obsapi::obs_source_release(second); obsapi::obs_data_release(settings);
        obs_shutdown();
        std::cout << "PASS: hidden timing reference activates, follows scene changes, preserves mute/routing, switches, and releases cleanly\n";
        return 0;
    } catch (const std::exception &e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
