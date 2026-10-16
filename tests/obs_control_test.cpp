// SPDX-License-Identifier: GPL-2.0-or-later
// Exercise native source management and VRChat log playback in isolated libobs.
#include "obs-api.hpp"
#include "logs.hpp"
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
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
template<typename F> void wait_for(F predicate, const char *message, int seconds = 4)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
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
std::atomic<int> restarts{0}, updates{0}, property_updates{0};
std::atomic<int> seeks{0};
std::atomic<int64_t> seek_time{0};
std::atomic<bool> media_paused{false};
std::atomic<bool> checks_isolated{true};
std::mutex media_mutex;
std::string played_url;

struct Media {
    obs_source_t *source;
    bool check;
    std::atomic<obs_media_state> state{OBS_MEDIA_STATE_NONE};
    void update(obs_data_t *settings) {
        const std::string url = obsapi::obs_data_get_string(settings, "input");
        state = url.empty() ? OBS_MEDIA_STATE_NONE : url.find("offline") != std::string::npos ? OBS_MEDIA_STATE_ERROR : OBS_MEDIA_STATE_PLAYING;
    }
    void tick() {
        if (state != OBS_MEDIA_STATE_PLAYING || (!check && input(source).find("stalled") != std::string::npos)) return;
        if (check && (obsapi::obs_source_get_audio_mixers(source) != 0 || obsapi::obs_source_get_volume(source) != 0 ||
            !obsapi::obs_source_muted(source))) checks_isolated = false;
        float silence[480]{};
        obs_source_audio audio{}; audio.data[0] = audio.data[1] = reinterpret_cast<uint8_t *>(silence);
        audio.frames = 480; audio.samples_per_sec = 48000; audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
        audio.speakers = SPEAKERS_STEREO; audio.timestamp = obsapi::os_gettime_ns();
        obsapi::obs_source_output_audio(source, &audio);
    }
};

bool playing(const std::string &url)
{
    std::lock_guard<std::mutex> lock(media_mutex);
    return played_url == url;
}

obs_source_t *player(obs_source_t *source)
{
    obs_source_t *result = nullptr;
    obsapi::obs_source_enum_active_sources(source, [](obs_source_t *, obs_source_t *child, void *data) {
        *static_cast<obs_source_t **>(data) = child;
    }, &result);
    require(result, "VaOBS owns a media player");
    return result;
}

struct TestClient {
    HANDLE process = nullptr;
    explicit TestClient(const vaobs::fs::path &binary)
    {
        STARTUPINFOW startup{}; startup.cb = sizeof(startup);
        PROCESS_INFORMATION info{};
        std::wstring command = L"\"" + binary.wstring() + L"\" --client";
        require(CreateProcessW(binary.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW,
            nullptr, nullptr, &startup, &info), "Start isolated simulated client");
        process = info.hProcess; CloseHandle(info.hThread);
    }
    void stop()
    {
        if (!process) return;
        TerminateProcess(process, 0); WaitForSingleObject(process, 5000);
        CloseHandle(process); process = nullptr;
    }
    ~TestClient() { stop(); }
};

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "--client") {
        for (;;) Sleep(1000);
    }
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
        LOAD(obs_property_visible);
        LOAD(obs_property_description); LOAD(obs_property_long_description); LOAD(obs_property_list_item_count);
        LOAD(obs_property_list_item_string); LOAD(obs_property_list_item_name); LOAD(obs_property_list_item_disabled);
        LOAD(obs_source_get_signal_handler); LOAD(signal_handler_connect); LOAD(signal_handler_disconnect);
        LOAD(obs_set_output_source); LOAD(obs_source_get_ref);
        LOAD(obs_data_create_from_json);
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
        const auto binary = GetModuleHandleW(L"vaobs-audio-align.dll");
        const auto read_status = load<size_t (*)(char *, size_t)>(binary, "vaobs_read_status");
        const auto retry_streams = load<bool (*)(const char *)>(binary, "vaobs_retry");
        const auto next_stream = load<bool (*)(const char *)>(binary, "vaobs_next");
        auto snapshot = [&] {
            std::vector<char> text(read_status(nullptr, 0) + 4096);
            require(read_status(text.data(), text.size()) <= text.size(), "Status snapshot buffer");
            auto *json = obs_data_create_from_json(text.data());
            require(json, "Status snapshot must be valid JSON"); obsapi::obs_data_release(json);
            return std::string(text.data());
        };
        obs_source_info media{};
        media.id = "ffmpeg_source"; media.type = OBS_SOURCE_TYPE_INPUT;
        media.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_CONTROLLABLE_MEDIA;
        media.get_name = [](void *) { return "Test media"; };
        media.create = [](obs_data_t *settings, obs_source_t *source) -> void * {
            auto *item = new Media{source, std::string(obsapi::obs_source_get_name(source)) == "VaOBS availability check"};
            item->update(settings); return item;
        };
        media.destroy = [](void *data) { delete static_cast<Media *>(data); };
        media.update = [](void *data, obs_data_t *settings) {
            auto &item = *static_cast<Media *>(data); item.update(settings);
            if (item.check) return;
            std::lock_guard<std::mutex> lock(media_mutex);
            played_url = obsapi::obs_data_get_string(settings, "input"); ++updates;
        };
        media.video_tick = [](void *data, float) { static_cast<Media *>(data)->tick(); };
        media.media_get_state = [](void *data) { return static_cast<Media *>(data)->state.load(); };
        media.media_restart = [](void *) { ++restarts; };
        media.media_get_duration = [](void *data) -> int64_t {
            return input(static_cast<Media *>(data)->source) == "https://example.test/recording.mp4" ? 3600000 : 0;
        };
        media.media_set_time = [](void *data, int64_t time) {
            if (!static_cast<Media *>(data)->check) { seek_time = time; ++seeks; }
        };
        media.media_play_pause = [](void *data, bool pause) {
            auto &item = *static_cast<Media *>(data);
            if (!item.check) media_paused = pause;
            item.state = pause ? OBS_MEDIA_STATE_PAUSED : OBS_MEDIA_STATE_PLAYING;
        };
        obsapi::obs_register_source_s(&media, sizeof(media));
        auto *settings = obsapi::obs_data_create();
        auto *aligned = obs_source_create("vaobs_auto_align_source", "VaOBS", settings, nullptr);
        require(aligned, "Create VaOBS before its reference exists");
        auto *feed = player(aligned);
        require(!obsapi::obs_get_source_by_name("VaOBS player"), "Player must not appear as a public source");
        require(obsapi::obs_source_muted(feed) && obsapi::obs_source_get_audio_mixers(feed) == 0,
            "Raw player audio must never reach recording tracks");
        const auto default_mixers = obsapi::obs_source_get_audio_mixers(aligned);
        obsapi::obs_source_set_audio_mixers(aligned, 4);
        obsapi::obs_source_set_volume(aligned, .42f);
        auto status_changed = [](void *, calldata_t *) { ++property_updates; };
        auto *signals = obs_source_get_signal_handler(aligned);
        signal_handler_connect(signals, "update_properties", status_changed, nullptr);
        obs_set_output_source(0, aligned);
        require((obsapi::obs_source_get_output_flags(aligned) & (OBS_SOURCE_AUDIO | OBS_SOURCE_VIDEO)) == OBS_SOURCE_AUDIO,
            "VaOBS must expose only audio");
        auto *reference = obs_source_create("ffmpeg_source", "VRChatAudio", nullptr, nullptr);
        require((default_mixers & 1) && default_mixers == obsapi::obs_source_get_audio_mixers(reference),
            "New VaOBS follows OBS default audio routing, including track 1");
        auto *second = obs_source_create("ffmpeg_source", "SecondReference", nullptr, nullptr);
        wait_for([&] { return property_updates > 0; }, "Status changes automatically refresh open Properties");
        require(!obsapi::obs_source_muted(reference), "Reference must keep its mute state");
        require(obsapi::obs_source_get_audio_mixers(aligned) == 4, "Preserve user recording tracks");
        require(std::abs(obsapi::obs_source_get_volume(aligned) - .42f) < .001f, "Preserve user volume");

        auto *props = obs_source_properties(aligned);
        require(!obs_property_visible(obsapi::obs_properties_get(props, "next_stream")), "Next is hidden without URLs");
        require(!next_stream(nullptr) && !next_stream("missing-source") && !next_stream(obsapi::obs_source_get_uuid(reference)),
            "Next rejects missing or unrelated sources");
        int lists = 0;
        for (auto *p = obs_properties_first(props); p; obs_property_next(&p))
            if (obs_property_get_type(p) == OBS_PROPERTY_LIST) ++lists;
        require(lists == 3 && !obsapi::obs_properties_get(props, "feed_source"), "One audio reference, client, and video stream selections");
        obs_property_button_clicked(obsapi::obs_properties_get(props, "refresh_status"), aligned);
        auto *status_property = obsapi::obs_properties_get(props, "status");
        require(std::string(obs_property_description(status_property)) == "Waiting..." &&
            std::string(obs_property_long_description(status_property)).find("VRChat") != std::string::npos,
            "Short status keeps its explanation in a tooltip");
        obs_properties_destroy(props);

        const auto dir = vaobs::vrchat_log_directory();
        wchar_t self[32768]{};
        require(GetModuleFileNameW(nullptr, self, DWORD(std::size(self))) > 0, "Find test program");
        const auto helper = profile / "client" / "VRChat.exe";
        vaobs::fs::create_directories(helper.parent_path());
        vaobs::fs::copy_file(self, helper, vaobs::fs::copy_options::overwrite_existing);
        TestClient first_client(helper);
        vaobs::fs::create_directories(dir);
        const auto log = dir / "output_log_1.txt";
        append(log, "[Video Playback] Resolving URL 'https://example.test/");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(input(feed).empty(), "Incomplete record must not change playback");
        append(log, "first'\n");
        wait_for([&] { return playing("https://example.test/first"); }, "Start watching after VRChat directory appears");
        wait_for([&] { return snapshot().find("waiting_reference") != std::string::npos; },
            "A bound but silent timing reference is explained in the dock");
        require(snapshot().find("\"build\":") != std::string::npos, "Dock includes exact build identity");
        const int single_updates = updates.load(), single_restarts = restarts.load();
        require(next_stream(obsapi::obs_source_get_uuid(aligned)), "Next can safely target a source whose alternatives disappeared");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(updates == single_updates && restarts == single_restarts && snapshot().find("\"can_next\":false") != std::string::npos,
            "One URL hides Next and does not restart its player");
        obs_source_set_name(aligned, "VaOBS \"quoted\" \\");
        wait_for([&] { return snapshot().find("quoted") != std::string::npos; }, "Source names remain valid JSON and update in the dock");
        obs_source_set_name(aligned, "VaOBS");
        auto *actual = obsapi::obs_source_get_settings(feed);
        require(!obsapi::obs_data_get_bool(actual, "seekable") && !obsapi::obs_data_get_bool(actual, "is_local_file"), "Network playback settings");
        require(!obsapi::obs_data_get_bool(actual, "restart_on_activate") && !obsapi::obs_data_get_bool(actual, "close_when_inactive"),
            "Scene changes must not restart private playback");
        obsapi::obs_data_release(actual);
        append(log, "[Video Playback] Resolving URL 'https://example.test/first'\n");
        wait_for([&] { return restarts.load() > 0; }, "Repeat of same URL restarts media");

        obsapi::obs_data_set_string(settings, "video_selection", "https://www.twitch.tv/rossmakr");
        obsapi::obs_source_update(aligned, settings);
        append(log,
            "[Video Playback] Attempting to resolve URL 'https://www.twitch.tv/rossmakr'\n"
            "[Video Playback] Attempting to resolve URL 'https://www.twitch.tv/unbeatablemusic'\n"
            "[Video Playback] URL 'https://www.twitch.tv/rossmakr' resolved to 'https://example.test/ross.m3u8'\n"
            "[Video Playback] URL 'https://www.twitch.tv/unbeatablemusic' resolved to 'https://example.test/other.m3u8'\n"
            "[Video Playback] Resolving URL 'third-player'\n"
            "[Video Playback] Resolving URL 'fourth-player'\n");
        wait_for([&] { return playing("https://example.test/ross.m3u8"); }, "Pin Rossmakr instead of the last of four players");
        const int pinned_updates = updates.load(), pinned_restarts = restarts.load();
        append(log, "[Video Playback] URL 'https://www.twitch.tv/unbeatablemusic' resolved to 'https://example.test/other-refreshed.m3u8'\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(updates == pinned_updates && restarts == pinned_restarts && input(feed) == "https://example.test/ross.m3u8",
            "Other players must not restart pinned playback or alignment");
        wait_for([&] {
            auto *p = obs_source_properties(aligned);
            const bool all = obs_property_list_item_count(obsapi::obs_properties_get(p, "video_selection")) == 6;
            obs_properties_destroy(p); return all;
        }, "List includes all confirmed online original URLs, not opaque resolved URLs");
        append(log, "[Video Playback] URL 'offline-player' resolved to 'https://example.test/offline.m3u8'\n");
        obsapi::obs_data_set_string(settings, "video_selection", "offline-player");
        obsapi::obs_source_update(aligned, settings);
        wait_for([&] {
            auto *p = obs_source_properties(aligned);
            auto *list = obsapi::obs_properties_get(p, "video_selection");
            bool unavailable = false;
            for (size_t i = 0; i < obs_property_list_item_count(list); ++i)
                if (std::string(obs_property_list_item_string(list, i)) == "offline-player")
                    unavailable = obs_property_list_item_disabled(list, i) &&
                        std::string(obs_property_list_item_name(list, i)).find("unavailable") != std::string::npos;
            obs_properties_destroy(p); return unavailable;
        }, "A pinned URL is marked unavailable after two failed decoder checks", 12);
        obsapi::obs_data_set_string(settings, "video_selection", "latest");
        obsapi::obs_source_update(aligned, settings);
        wait_for([&] { return playing("https://example.test/other-refreshed.m3u8"); }, "Automatic search starts with an available URL and skips the newer offline URL");
        props = obs_source_properties(aligned);
        auto *stream_list = obsapi::obs_properties_get(props, "video_selection");
        for (size_t i = 0; i < obs_property_list_item_count(stream_list); ++i)
            require(std::string(obs_property_list_item_string(stream_list, i)) != "offline-player", "Unselected offline URLs are hidden");
        obs_properties_destroy(props);
        require(snapshot().find("\"can_next\":true") != std::string::npos, "Dock advertises multiple available URLs");
        for (const auto *url : {"https://example.test/first", "https://example.test/ross.m3u8", "third-player", "fourth-player", "https://example.test/other-refreshed.m3u8"}) {
            require(next_stream(obsapi::obs_source_get_uuid(aligned)), "Dock Next targets the source");
            wait_for([&] { return playing(url); }, "Next cycles available URLs and wraps, skipping offline URLs");
            auto *saved = obsapi::obs_source_get_settings(aligned);
            const bool automatic = std::string(obsapi::obs_data_get_string(saved, "video_selection")) == "latest";
            obsapi::obs_data_release(saved); require(automatic, "Next preserves automatic search mode");
        }
        obsapi::obs_data_set_string(settings, "video_selection", "https://www.twitch.tv/rossmakr");
        obsapi::obs_source_update(aligned, settings);
        wait_for([&] { return playing("https://example.test/ross.m3u8"); }, "Offline filtering preserves the manual stream choice");
        props = obs_source_properties(aligned);
        auto *next_property = obsapi::obs_properties_get(props, "next_stream");
        require(obs_property_visible(next_property), "Properties shows Next for multiple URLs");
        obs_property_button_clicked(next_property, aligned); obs_properties_destroy(props);
        wait_for([&] { return playing("third-player"); }, "Properties Next advances a pinned URL");
        auto *pinned_settings = obsapi::obs_source_get_settings(aligned);
        require(std::string(obsapi::obs_data_get_string(pinned_settings, "video_selection")) == "third-player", "Next saves the new manual selection");
        obsapi::obs_data_release(pinned_settings);
        obsapi::obs_data_set_string(settings, "video_selection", "https://www.twitch.tv/rossmakr");
        obsapi::obs_source_update(aligned, settings);
        wait_for([&] { return playing("https://example.test/ross.m3u8"); }, "Restore manual choice after Next");
        const int before_retry = restarts.load();
        require(!retry_streams("missing-source") && !retry_streams(obsapi::obs_source_get_uuid(reference)), "Retry rejects missing or unrelated sources");
        require(retry_streams(obsapi::obs_source_get_uuid(aligned)), "Dock retry targets a VaOBS source by UUID");
        wait_for([&] { return restarts > before_retry; }, "Retry reconnects the pinned stream without changing selection");
        append(log, "[Video Playback] URL 'offline-player' resolved to 'https://example.test/recovered.m3u8'\n");
        wait_for([&] {
            auto *p = obs_source_properties(aligned);
            auto *list = obsapi::obs_properties_get(p, "video_selection");
            bool recovered = false;
            for (size_t i = 0; i < obs_property_list_item_count(list); ++i)
                if (std::string(obs_property_list_item_string(list, i)) == "offline-player") recovered = !obs_property_list_item_disabled(list, i);
            obs_properties_destroy(p); return recovered;
        }, "A refreshed working URL reappears after decoding audio");
        append(log, "[Video Playback] URL 'https://www.twitch.tv/rossmakr' resolved to 'https://example.test/ross-refreshed.m3u8'\n");
        wait_for([&] { return input(feed) == "https://example.test/ross-refreshed.m3u8"; }, "Pinned original URL follows refreshed stream URL");
        append(log, "[Behaviour] Joining wrld_other:12345\n[Video Playback] Resolving URL 'other-world-video'\n");
        wait_for([&] { return input(feed).empty(); }, "Instance switch clears old videos and waits for the pinned stream");
        obsapi::obs_data_set_string(settings, "video_selection", "https://example.test/first");
        obsapi::obs_source_update(aligned, settings);
        append(log, "[Video Playback] Resolving URL 'https://example.test/first'\n");
        wait_for([&] { return playing("https://example.test/first"); }, "Manual selection resumes playback in the new instance");

        TestClient other_client(helper);
        const auto other_log = dir / "output_log_2.txt";
        append(other_log, "[Video Playback] Resolving URL 'https://example.test/first'\n");
        const int playback_updates = updates.load(), playback_restarts = restarts.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        require(updates == playback_updates && restarts == playback_restarts, "Second client does not interrupt first-client mode");
        props = obs_source_properties(aligned);
        require(obs_property_list_item_count(obsapi::obs_properties_get(props, "client_selection")) == 4,
            "Client list includes first/newest modes and both running clients");
        obs_properties_destroy(props);
        obsapi::obs_data_set_string(settings, "client_selection", "newest");
        obsapi::obs_source_update(aligned, settings);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(updates == playback_updates && restarts == playback_restarts, "Same-video newest-client handover does not restart");
        append(log, "Unrelated message from first client\n");
        vaobs::fs::last_write_time(log, vaobs::fs::file_time_type::clock::now() + std::chrono::hours(1));
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(updates == playback_updates && restarts == playback_restarts, "Unrelated writes never take over from newest client");
        obsapi::obs_data_set_string(settings, "client_selection", "log:output_log_1.txt");
        obsapi::obs_source_update(aligned, settings);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(updates == playback_updates && restarts == playback_restarts, "Same-video manual handover does not restart");
        append(other_log, "[Video Playback] Resolving URL 'other-client-video'\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(updates == playback_updates && restarts == playback_restarts, "Manual client selection ignores the other client's playback");

        obs_source_set_name(reference, "RenamedReference");
        wait_for([&] {
            auto *saved = obsapi::obs_source_get_settings(aligned);
            bool good = std::string(obsapi::obs_data_get_string(saved, "reference_source")) == "RenamedReference";
            obsapi::obs_data_release(saved); return good;
        }, "Reference rename persists by UUID");
        append(log, "[Video Playback] URL 'old' resolved to 'rtspt://example.test/live'\n");
        obsapi::obs_data_set_string(settings, "video_selection", "old");
        obsapi::obs_source_update(aligned, settings);
        wait_for([&] { return playing("rtmp://example.test/live"); }, "Resolved URL controls renamed source");

        const int before_reference_change = restarts.load(), updates_before_reference = updates.load();
        obsapi::obs_data_set_string(settings, "reference_source", "SecondReference");
        obsapi::obs_source_update(aligned, settings);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(restarts == before_reference_change && updates == updates_before_reference && input(feed) == "rtmp://example.test/live",
            "Changing timing reference recalibrates without restarting video");
        require(!obsapi::obs_source_muted(second) && obsapi::obs_source_get_audio_mixers(aligned) == 4,
            "Reference selection preserves routing and mute states");
        require(seeks == 0, "Live streams are not treated as seekable files");
        auto timestamp = [](int64_t milliseconds) {
            const std::time_t seconds = milliseconds / 1000;
            const std::tm local = *std::localtime(&seconds);
            char text[32]; std::strftime(text, sizeof(text), "%Y.%m.%d %H:%M:%S", &local);
            return std::string(text) + " Debug - ";
        };
        const auto old_stamp = timestamp(vaobs::wall_time_ms() - 600000);
        const std::string file_log = old_stamp + "[VideoTXL:AVPro-0] Load Url: https://example.test/recording.mp4\n" +
            old_stamp + "[Video Playback] Resolving URL 'https://example.test/recording.mp4'\n" +
            old_stamp + "[VideoTXL:AVPro-0] Set time: 0\n";
        append(log, file_log.c_str());
        obsapi::obs_data_set_string(settings, "video_selection", "https://example.test/recording.mp4");
        obsapi::obs_source_update(aligned, settings);
        wait_for([&] { return seeks > 0; }, "Late file playback seeks to the logged VRChat position", 8);
        require(seek_time >= 600000 && seek_time <= 625000, "Seek catches up ten minutes before measuring the remaining delay");
        actual = obsapi::obs_source_get_settings(feed);
        require(obsapi::obs_data_get_bool(actual, "seekable"), "Finite media enables seeking");
        obsapi::obs_data_release(actual);
        const int caught_up = seeks.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(seeks == caught_up, "An unchanged position does not repeatedly seek or reset alignment");
        append(log, (timestamp(vaobs::wall_time_ms()) + "[VideoTXL:AVPro-0] Pause\n").c_str());
        wait_for([&] { return media_paused.load(); }, "Logged pause pauses the private file player");
        append(log, (timestamp(vaobs::wall_time_ms()) + "[VideoTXL:AVPro-0] Play\n").c_str());
        wait_for([&] { return !media_paused.load(); }, "Logged resume resumes and recalibrates the private player");
        append(log, "[Video Playback] Resolving URL 'https://example.test/stalled.mp4'\n");
        obsapi::obs_data_set_string(settings, "video_selection", "https://example.test/stalled.mp4");
        obsapi::obs_source_update(aligned, settings);
        wait_for([&] { return input(feed) == "https://example.test/stalled.mp4"; }, "Start a connection whose probe succeeds but player delivers no audio");
        wait_for([&] { return input(feed).empty(); }, "A player without audio times out instead of connecting forever", 24);
        const int timed_out_updates = updates.load();
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        require(input(feed).empty() && updates == timed_out_updates, "Probe success does not cause an immediate retry loop");
        auto *choice = obsapi::obs_source_get_settings(aligned);
        require(std::string(obsapi::obs_data_get_string(choice, "video_selection")) == "https://example.test/stalled.mp4",
            "A failed manual stream stays selected while its connection waits for retry");
        obsapi::obs_data_release(choice);
        require(retry_streams(obsapi::obs_source_get_uuid(aligned)), "Retry a stalled player immediately");
        wait_for([&] { return input(feed) == "https://example.test/stalled.mp4"; }, "Manual retry reopens the same URL");
        append(log, "[Behaviour] Successfully left room\n");
        wait_for([&] { return input(feed).empty(); }, "Leave clears playback");
        append(log, "[Video Playback] Resolving URL 'after-leave'\n");
        obsapi::obs_data_set_string(settings, "video_selection", "after-leave");
        obsapi::obs_source_update(aligned, settings);
        wait_for([&] { return input(feed) == "after-leave"; }, "Playback after leave");
        append(other_log, "[Video Playback] Resolving URL 'after-leave'\n");
        const int before_exit = restarts.load();
        obsapi::obs_data_set_string(settings, "client_selection", "first");
        obsapi::obs_source_update(aligned, settings);
        first_client.stop();
        wait_for([&] {
            return vaobs::running_clients(dir).size() == 1;
        }, "Exited client is removed from discovery");
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        require(input(feed) == "after-leave" && restarts == before_exit, "First-client exit hands over same video without restart");
        obsapi::obs_data_set_string(settings, "client_selection", "log:output_log_1.txt");
        obsapi::obs_source_update(aligned, settings);
        wait_for([&] { return input(feed).empty(); }, "Missing manual client waits instead of following another client");
        wait_for([&] { return snapshot().find("waiting_client") != std::string::npos; }, "Missing manual client is explained even while another client is running");
        obsapi::obs_data_set_string(settings, "client_selection", "newest");
        obsapi::obs_source_update(aligned, settings);
        wait_for([&] { return input(feed) == "after-leave"; }, "Newest client remains available");
        append(other_log, "VRCApplication: HandleApplicationQuit\n");
        other_client.stop();
        wait_for([&] { return input(feed).empty(); }, "Last client exit clears playback");
        obs_source_remove(second);
        obsapi::obs_source_release(second);
        wait_for([&] {
            auto *old = obsapi::obs_get_source_by_name("SecondReference");
            if (old) obsapi::obs_source_release(old);
            return old == nullptr;
        }, "Removed source must release its reserved name");
        second = obs_source_create("ffmpeg_source", "SecondReference", nullptr, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        auto *bound = obsapi::obs_source_get_settings(aligned);
        require(std::string(obsapi::obs_data_get_string(bound, "reference_uuid")) == obsapi::obs_source_get_uuid(second),
            "Rebind a removed and recreated reference");
        obsapi::obs_data_release(bound);

        auto *saved = obsapi::obs_source_get_settings(aligned);
        obsapi::obs_source_set_audio_mixers(aligned, 8);
        signal_handler_disconnect(signals, "update_properties", status_changed, nullptr);
        obs_set_output_source(0, nullptr);
        obsapi::obs_source_release(aligned);
        // Migrate a scene saved by the earlier external-feed implementation.
        auto *legacy_settings = obsapi::obs_data_create();
        obsapi::obs_data_set_string(legacy_settings, "input", "old-controlled-video");
        auto *legacy = obs_source_create("ffmpeg_source", "VRChatFeed", legacy_settings, nullptr);
        obsapi::obs_data_release(legacy_settings);
        obsapi::obs_source_set_muted(legacy, true);
        obsapi::obs_data_set_string(saved, "managed_feed_uuid", obsapi::obs_source_get_uuid(legacy));
        obsapi::obs_data_set_bool(saved, "original_feed_muted", false);
        aligned = obs_source_create("vaobs_auto_align_source", "ReloadedVaOBS", saved, nullptr);
        feed = player(aligned);
        obsapi::obs_source_set_audio_mixers(aligned, 8);
        wait_for([&] { return !obsapi::obs_source_muted(legacy) && input(legacy).empty(); },
            "Migration clears old controlled playback and restores original mute");
        require(obsapi::obs_source_get_audio_mixers(aligned) == 8, "Reload preserves user output track choices");
        auto *retained_player = obs_source_get_ref(feed);
        obs_source_remove(aligned); // Properties may still hold a reference.
        wait_for([&] { return input(feed).empty(); }, "Removing a retained VaOBS source stops private playback");
        wait_for([&] { return snapshot().find("ReloadedVaOBS") == std::string::npos; }, "Removed sources disappear from the dock even when retained by Properties");
        obsapi::obs_source_release(aligned);
        obsapi::obs_source_release(retained_player);
        obsapi::obs_data_release(saved);
        obsapi::obs_data_release(settings);
        obsapi::obs_source_release(second); obsapi::obs_source_release(legacy); obsapi::obs_source_release(reference);
        require(checks_isolated, "Availability checks must stay muted and outside recording tracks");
        obs_shutdown();
        vaobs::fs::remove_all(profile);
        std::cout << "PASS: audio-only source, private player, stream selection, short status, file catch-up, pause/resume, clients, migration and reload\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << "FAIL: " << e.what() << '\n'; return 1;
    }
}
