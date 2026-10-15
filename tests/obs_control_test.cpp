// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise native source management and VRChat log playback in isolated libobs.
#include "obs-api.hpp"
#include "logs.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

template<typename T> T load(HMODULE module, const char *name)
{
    auto fn = reinterpret_cast<T>(GetProcAddress(module, name));
    if (!fn) throw std::runtime_error(std::string("Missing API: ") + name);
    return fn;
}
#define LOAD(name) auto name = load<decltype(&::name)>(module, #name)
void require(bool good, const char *message) { if (!good) throw std::runtime_error(message); }
template<typename F> void wait_for(F predicate, const char *message)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    while (!predicate()) {
        require(std::chrono::steady_clock::now() < end, message);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}
void append(const vaobs::fs::path &path, const char *text) { std::ofstream(path, std::ios::app) << text; }
std::string input(obs_source_t *source)
{
    auto *data = obsapi::obs_source_get_settings(source);
    std::string result = obsapi::obs_data_get_string(data, "input");
    obsapi::obs_data_release(data); return result;
}
std::atomic<int> restarts{0}, updates{0};

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    const auto profile = vaobs::fs::temp_directory_path() / ("vaobs-control-test-" + std::to_string(GetCurrentProcessId()));
    try {
        SetEnvironmentVariableW(L"USERPROFILE", profile.c_str());
        HMODULE module = LoadLibraryExW((vaobs::fs::path(argv[1]) / L"obs.dll").c_str(), nullptr,
            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        require(module && obsapi::load(), "Load real libobs");
        LOAD(obs_startup); LOAD(obs_shutdown); LOAD(obs_reset_audio); LOAD(obs_reset_video);
        LOAD(obs_open_module); LOAD(obs_init_module); LOAD(obs_source_create);
        LOAD(obs_source_set_name); LOAD(obs_source_remove);
        LOAD(obs_source_properties); LOAD(obs_properties_first); LOAD(obs_property_next);
        LOAD(obs_property_get_type); LOAD(obs_properties_destroy); LOAD(obs_property_button_clicked);
        LOAD(obs_property_description);
        require(obs_startup("en-US", nullptr, nullptr), "Start libobs");
        const auto bin = vaobs::fs::absolute(argv[1]);
        vaobs::fs::current_path(bin); // libobs resolves its shader data relative to bin/64bit.
        const auto graphics = (bin / "libobs-d3d11.dll").u8string();
        obs_video_info video{};
        video.graphics_module = graphics.c_str();
        video.fps_num = 30; video.fps_den = 1;
        video.base_width = video.output_width = 128; video.base_height = video.output_height = 128;
        video.output_format = VIDEO_FORMAT_RGBA;
        require(obs_reset_video(&video) == OBS_VIDEO_SUCCESS, "Initialize headless video ticks for media commands");
        obs_audio_info audio{48000, SPEAKERS_STEREO};
        require(obs_reset_audio(&audio), "Initialize audio");
        obs_module_t *plugin = nullptr;
        require(obs_open_module(&plugin, argv[2], ".") == 0 && obs_init_module(plugin), "Load plugin");
        obs_source_info media{};
        media.id = "ffmpeg_source"; media.type = OBS_SOURCE_TYPE_INPUT;
        media.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_CONTROLLABLE_MEDIA;
        media.get_name = [](void *) { return "Test media"; };
        media.create = [](obs_data_t *, obs_source_t *) -> void * { return new int(1); };
        media.destroy = [](void *data) { delete static_cast<int *>(data); };
        media.update = [](void *, obs_data_t *) { ++updates; };
        media.media_restart = [](void *) { ++restarts; };
        obsapi::obs_register_source_s(&media, sizeof(media));
        auto *settings = obsapi::obs_data_create();
        auto *aligned = obs_source_create("vaobs_auto_align_source", "VaOBS", settings, nullptr);
        require(aligned, "Create VaOBS before its inputs exist");
        auto *reference = obs_source_create("ffmpeg_source", "VRChatAudio", nullptr, nullptr);
        auto *feed_settings = obsapi::obs_data_create();
        obsapi::obs_data_set_string(feed_settings, "custom", "preserved");
        auto *feed = obs_source_create("ffmpeg_source", "VRChatFeed", feed_settings, nullptr);
        auto *second = obs_source_create("ffmpeg_source", "SecondFeed", nullptr, nullptr);
        obsapi::obs_source_set_audio_mixers(feed, 2);
        obsapi::obs_source_set_volume(feed, .42f);
        wait_for([&] { return obsapi::obs_source_muted(feed); }, "Bind late-created inputs and mute original feed");
        require(!obsapi::obs_source_muted(reference), "Reference must keep its mute state");
        require(obsapi::obs_source_get_audio_mixers(aligned) == 2, "Copy media recording tracks");
        require(std::abs(obsapi::obs_source_get_volume(aligned) - .42f) < .001f, "Copy media volume");

        auto *props = obs_source_properties(aligned);
        int lists = 0;
        for (auto *p = obs_properties_first(props); p; obs_property_next(&p))
            if (obs_property_get_type(p) == OBS_PROPERTY_LIST) ++lists;
        require(lists == 2, "Only two source selections");
        obs_property_button_clicked(obsapi::obs_properties_get(props, "refresh_status"), aligned);
        require(std::string(obs_property_description(obsapi::obs_properties_get(props, "status"))).find("VRChat log") != std::string::npos,
            "Refresh status updates displayed text");
        obs_properties_destroy(props);

        const auto dir = vaobs::vrchat_log_directory();
        vaobs::fs::create_directories(dir);
        const auto log = dir / "output_log_1.txt";
        append(log, "[Video Playback] Resolving URL 'https://example.test/");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(input(feed).empty(), "Incomplete record must not change playback");
        append(log, "first'\n");
        wait_for([&] { return input(feed) == "https://example.test/first"; }, "Start watching after VRChat directory appears");
        auto *actual = obsapi::obs_source_get_settings(feed);
        require(!obsapi::obs_data_get_bool(actual, "seekable") && !obsapi::obs_data_get_bool(actual, "is_local_file"), "Network playback settings");
        require(std::string(obsapi::obs_data_get_string(actual, "custom")) == "preserved", "Preserve other media settings");
        obsapi::obs_data_release(actual);
        append(log, "[Video Playback] Resolving URL 'https://example.test/first'\n");
        wait_for([&] { return restarts.load() > 0; }, "Repeat of same URL restarts media");

        obs_source_set_name(feed, "RenamedFeed");
        wait_for([&] {
            auto *saved = obsapi::obs_source_get_settings(aligned);
            bool good = std::string(obsapi::obs_data_get_string(saved, "feed_source")) == "RenamedFeed";
            obsapi::obs_data_release(saved); return good;
        }, "Source rename persists by UUID");
        append(log, "[Video Playback] URL 'old' resolved to 'rtspt://example.test/live'\n");
        wait_for([&] { return input(feed) == "rtmp://example.test/live"; }, "Resolved URL controls renamed source");

        auto *duplicate = obs_source_create("vaobs_auto_align_source", "DuplicateVaOBS", nullptr, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(600));
        require(obsapi::obs_source_muted(feed), "Duplicate must not release another source's feed");
        obsapi::obs_source_release(duplicate);
        obsapi::obs_source_set_audio_mixers(aligned, 4);
        obsapi::obs_data_set_string(settings, "feed_source", "SecondFeed");
        obsapi::obs_source_update(aligned, settings);
        wait_for([&] { return obsapi::obs_source_muted(second) && input(second) == "rtmp://example.test/live"; }, "Apply new source selection without restarting OBS");
        require(!obsapi::obs_source_muted(feed) && input(feed).empty(), "Release old feed, clear controlled playback and restore mute");
        require(obsapi::obs_source_get_audio_mixers(aligned) == obsapi::obs_source_get_audio_mixers(second), "Copy new feed track assignments");
        append(log, "[Behaviour] Successfully left room\n");
        wait_for([&] { return input(second).empty(); }, "Leave clears playback");
        append(log, "[Video Playback] Resolving URL 'after-leave'\n");
        wait_for([&] { return input(second) == "after-leave"; }, "Playback after leave");
        const auto next = dir / "output_log_2.txt";
        append(next, "New session\n");
        vaobs::fs::last_write_time(next, vaobs::fs::file_time_type::clock::now() + std::chrono::seconds(2));
        wait_for([&] { return input(second).empty(); }, "New VRChat log clears previous session");
        obs_source_remove(second);
        obsapi::obs_source_release(second);
        wait_for([&] {
            auto *old = obsapi::obs_get_source_by_name("SecondFeed");
            if (old) obsapi::obs_source_release(old);
            return old == nullptr;
        }, "Removed source must release its reserved name");
        second = obs_source_create("ffmpeg_source", "SecondFeed", nullptr, nullptr);
        wait_for([&] { return obsapi::obs_source_muted(second); }, "Rebind a removed and recreated media source");

        auto *saved = obsapi::obs_source_get_settings(aligned);
        obsapi::obs_source_set_audio_mixers(aligned, 8);
        obsapi::obs_source_release(aligned);
        wait_for([&] { return !obsapi::obs_source_muted(second); }, "Removing VaOBS restores feed mute");
        // Simulate the media's muted state being saved while OBS closes.
        obsapi::obs_source_set_muted(second, true);
        aligned = obs_source_create("vaobs_auto_align_source", "ReloadedVaOBS", saved, nullptr);
        obsapi::obs_source_set_audio_mixers(aligned, 8);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(obsapi::obs_source_muted(second), "Reload rebinds saved sources");
        require(obsapi::obs_source_get_audio_mixers(aligned) == 8, "Reload preserves user output track choices");
        obs_source_remove(aligned); // Properties may still hold a reference.
        wait_for([&] { return !obsapi::obs_source_muted(second); }, "Removing a retained VaOBS source promptly restores media audio");
        obsapi::obs_source_release(aligned);
        wait_for([&] { return !obsapi::obs_source_muted(second); }, "Reload retains original mute state");
        obsapi::obs_data_release(saved);
        obsapi::obs_data_release(settings); obsapi::obs_data_release(feed_settings);
        obsapi::obs_source_release(second); obsapi::obs_source_release(feed); obsapi::obs_source_release(reference);
        obs_shutdown();
        vaobs::fs::remove_all(profile);
        std::cout << "PASS: native log playback, live settings, track routing, rename, repeat, duplicate ownership, log switching, reload and mute restoration\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
