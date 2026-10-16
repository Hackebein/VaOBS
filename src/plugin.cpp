// SPDX-License-Identifier: GPL-2.0-or-later
#include "obs-api.hpp"
#include "alignment.hpp"
#include "logs.hpp"
#include "dock.hpp"
#include "build-version.hpp"
#include <objbase.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <stdexcept>
#include <thread>
#include <cstring>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("VaOBS contributors")

namespace {
using namespace vaobs;
constexpr const char *kind = "vaobs_auto_align_source";
std::mutex dock_mutex;
std::map<std::string, std::string> dock_rows;

std::string json_string(const std::string &value)
{
    std::string result = "\"";
    for (const unsigned char ch : value) {
        if (ch == '"' || ch == '\\') { result += '\\'; result += char(ch); }
        else if (ch < 32) {
            char escaped[7]; std::snprintf(escaped, sizeof(escaped), "\\u%04x", ch); result += escaped;
        } else result += char(ch);
    }
    return result + '"';
}

struct Presentation {
    std::string state, title, detail, stream, reference;
    bool can_next = false;
    std::string text() const { return title + "\n" + detail + (stream.empty() ? "" : "\nStream: " + stream); }
    std::string json(const std::string &name, const std::string &uuid) const {
        return "{\"uuid\":" + json_string(uuid) + ",\"name\":" + json_string(name) +
            ",\"state\":" + json_string(state) + ",\"title\":" + json_string(title) +
            ",\"detail\":" + json_string(detail) + ",\"stream\":" + json_string(stream) +
            ",\"reference\":" + json_string(reference) + ",\"can_next\":" + (can_next ? "true" : "false") + "}";
    }
};

struct ClockMapper {
    bool initialized = false;
    int64_t correction = 0;
    void clear() { initialized = false; }
    uint64_t map(uint64_t timestamp, uint64_t now)
    {
        if (!initialized) {
            correction = std::abs(double(timestamp) - double(now)) < 2000000000.0
                ? 0 : int64_t(now) - int64_t(timestamp);
            initialized = true;
        }
        const int64_t mapped = int64_t(timestamp) + correction;
        // A private media clock may restart after a seek/reconnection.
        if (std::abs(double(mapped) - double(now)) > 2000000000.0)
            correction = int64_t(now) - int64_t(timestamp);
        return uint64_t(std::max(int64_t(0), int64_t(timestamp) + correction));
    }
};

struct Aligner {
    obs_source_t *source, *reference = nullptr, *feed = nullptr;
    std::mutex binding_mutex, pcm_mutex, analysis_mutex, sleep_mutex;
    std::condition_variable wake;
    std::atomic<bool> stopping{false}, ready{false}, connected{false}, playing{false};
    std::atomic<bool> search_next{false};
    std::atomic<bool> retry_requested{false};
    std::atomic<unsigned> next_requested{0};
    std::atomic<double> applied_ms{0};
    std::atomic<uint64_t> audio_generation{0};
    std::atomic<uint64_t> last_feed_packet{0};
    std::atomic<uint64_t> last_reference_packet{0}, last_reference_signal{0};
    uint64_t playback_started = 0;
    uint64_t applied_position_revision = 0;
    uint64_t generation = 0;
    int64_t stream_generation = -1;
    int64_t retry_generation = 0;
    int64_t next_generation = 0;
    uint32_t sample_rate = 48000;
    size_t channels = 2;
    double max_delay_ms = 60000, window_ms = 4000, threshold = .75;
    std::string reference_name, reference_uuid;
    std::string client_selection = "first";
    std::string video_selection = "latest";
    std::vector<Client> clients;
    std::vector<Video> videos;
    StreamChecks stream_checks;
    size_t candidate = 1;
    bool match_candidate = false;
    std::string log_status = "Waiting for a VRChat log";
    bool initialized = false, migrated = false;
    std::optional<std::string> desired_url;
    uint64_t playback_revision = 0, applied_revision = 0;
    Timeline reference_history, feed_history;
    ClockMapper reference_clock, feed_clock;
    TimedBuffer pcm;
    Lock controller;
    SearchProgress search_progress;
    std::thread analyst, output, watcher, prober;

    explicit Aligner(obs_source_t *s) : source(s)
    {
        auto *settings = obsapi::obs_data_create();
        obsapi::obs_data_set_bool(settings, "is_local_file", false);
        obsapi::obs_data_set_bool(settings, "seekable", false);
        obsapi::obs_data_set_bool(settings, "restart_on_activate", false);
        obsapi::obs_data_set_bool(settings, "close_when_inactive", false);
        obsapi::obs_data_set_bool(settings, "log_changes", false);
        obsapi::obs_data_set_string(settings, "ffmpeg_options", "rw_timeout=5000000");
        feed = obsapi::obs_source_create_private("ffmpeg_source", "VaOBS player", settings);
        obsapi::obs_data_release(settings);
        if (!feed || !(obsapi::obs_source_get_output_flags(feed) & OBS_SOURCE_AUDIO)) {
            if (feed) obsapi::obs_source_release(feed);
            throw std::runtime_error("OBS Media Source is unavailable");
        }
        // The private player's raw audio is captured before OBS applies mute.
        // Only this source's delayed output may reach the recording mixer.
        obsapi::obs_source_set_audio_mixers(feed, 0);
        obsapi::obs_source_set_muted(feed, true);
        obsapi::obs_source_add_audio_capture_callback(feed, feed_audio, this);
        obsapi::obs_source_add_active_child(source, feed);
    }
    ~Aligner()
    {
        stopping = true; wake.notify_all();
        if (watcher.joinable()) watcher.join();
        if (prober.joinable()) prober.join();
        if (analyst.joinable()) analyst.join();
        if (output.joinable()) output.join();
        std::lock_guard<std::mutex> binding(binding_mutex);
        detach();
        obsapi::obs_source_remove_audio_capture_callback(feed, feed_audio, this);
        obsapi::obs_source_remove_active_child(source, feed);
        obsapi::obs_source_release(feed);
        std::lock_guard<std::mutex> ui(dock_mutex);
        dock_rows.erase(obsapi::obs_source_get_uuid(source));
    }

    void detach()
    {
        const bool was_connected = connected.exchange(false);
        if (reference) {
            obsapi::obs_source_remove_audio_capture_callback(reference, reference_audio, this);
            obsapi::obs_source_release(reference); reference = nullptr;
        }
        if (was_connected) {
            std::lock_guard<std::mutex> audio(pcm_mutex);
            std::lock_guard<std::mutex> analysis(analysis_mutex);
            reset();
        }
    }

    // Caller holds pcm_mutex and analysis_mutex, in that order.
    void reset()
    {
        reference_history.clear(); feed_history.clear();
        reference_clock.clear(); feed_clock.clear();
        controller.reset(); ready = false; applied_ms = 0;
        search_progress.reset(); search_next = false;
        match_candidate = false; last_reference_packet = 0; last_reference_signal = 0;
        pcm.configure(sample_rate, channels, max_delay_ms);
        ++generation; ++audio_generation;
    }

    void update(obs_data_t *settings)
    {
        const std::string ref = obsapi::obs_data_get_string(settings, "reference_source");
        const std::string selected_client = obsapi::obs_data_get_string(settings, "client_selection");
        const std::string selected_video = obsapi::obs_data_get_string(settings, "video_selection");
        const double maximum = std::clamp(double(obsapi::obs_data_get_int(settings, "max_delay_ms")), 1000.0, 120000.0);
        const double window = std::clamp(double(obsapi::obs_data_get_int(settings, "window_ms")), 2000.0, 10000.0);
        const double minimum = std::clamp(obsapi::obs_data_get_double(settings, "minimum_correlation"), .5, .99);
        const int64_t stream = obsapi::obs_data_get_int(settings, "stream_generation");
        const int64_t retry = obsapi::obs_data_get_int(settings, "retry_generation");
        const int64_t next = obsapi::obs_data_get_int(settings, "next_generation");
        obs_audio_info info{}; obsapi::obs_get_audio_info(&info);
        const uint32_t rate = info.samples_per_sec ? info.samples_per_sec : 48000;
        const size_t count = std::max(size_t(1), size_t(get_audio_channels(info.speakers)));
        std::lock_guard<std::mutex> binding(binding_mutex);
        const bool first_update = !initialized;
        if (ref != reference_name) detach();
        if (!initialized) {
            reference_uuid = obsapi::obs_data_get_string(settings, "reference_uuid");
            initialized = true;
        } else {
            if (ref != reference_name) {
                reference_uuid.clear(); obsapi::obs_data_set_string(settings, "reference_uuid", "");
            }
        }
        std::lock_guard<std::mutex> audio(pcm_mutex);
        std::lock_guard<std::mutex> analysis(analysis_mutex);
        const bool changed = ref != reference_name || maximum != max_delay_ms ||
            window != window_ms || minimum != threshold || stream != stream_generation ||
            rate != sample_rate || count != channels;
        if (selected_video != video_selection || selected_client != client_selection) {
            search_progress.reset(); search_next = false;
        }
        reference_name = ref; max_delay_ms = maximum;
        client_selection = selected_client.empty() ? "first" : selected_client;
        video_selection = selected_video.empty() ? "latest" : selected_video;
        window_ms = window; threshold = minimum; stream_generation = stream;
        sample_rate = rate; channels = count;
        if (retry != retry_generation) { retry_requested = true; retry_generation = retry; }
        if (next != next_generation) {
            if (!first_update) ++next_requested;
            next_generation = next;
        }
        if (changed) reset();
    }

    bool acceptable(obs_source_t *candidate)
    {
        return candidate && candidate != source && !obsapi::obs_source_removed(candidate) &&
            std::string(obsapi::obs_source_get_id(candidate)) != kind &&
            candidate != feed && (obsapi::obs_source_get_output_flags(candidate) & OBS_SOURCE_AUDIO);
    }

    // Caller holds binding_mutex; OBS owns and saves this source's settings.
    void remember_sources()
    {
        auto *settings = obsapi::obs_source_get_settings(source);
        obsapi::obs_data_set_string(settings, "reference_source", reference_name.c_str());
        obsapi::obs_data_set_string(settings, "reference_uuid", reference_uuid.c_str());
        obsapi::obs_data_release(settings);
    }

    void bind_locked()
    {
        if (reference && !obsapi::obs_source_removed(reference)) {
            if (reference_name != obsapi::obs_source_get_name(reference)) {
                reference_name = obsapi::obs_source_get_name(reference);
                remember_sources();
            }
            return;
        }
        detach();
        auto *ref = reference_uuid.empty() ? obsapi::obs_get_source_by_name(reference_name.c_str()) :
            obsapi::obs_get_source_by_uuid(reference_uuid.c_str());
        if (!ref && !reference_uuid.empty()) ref = obsapi::obs_get_source_by_name(reference_name.c_str());
        if (!acceptable(ref)) {
            if (ref) obsapi::obs_source_release(ref);
            return;
        }
        reference = ref;
        reference_name = obsapi::obs_source_get_name(reference);
        reference_uuid = obsapi::obs_source_get_uuid(reference);
        remember_sources();
        {
            std::lock_guard<std::mutex> audio(pcm_mutex);
            std::lock_guard<std::mutex> analysis(analysis_mutex);
            reset();
        }
        obsapi::obs_source_add_audio_capture_callback(reference, reference_audio, this);
        connected = true;
        obsapi::blog(LOG_INFO, "[VaOBS] Aligning internal player against '%s'", reference_name.c_str());
    }

    // Release the external feed used by earlier test builds. OBS retains the
    // user's source and scene layout, but VaOBS no longer plays through it.
    void migrate_feed()
    {
        if (migrated) return;
        auto *settings = obsapi::obs_source_get_settings(source);
        const std::string uuid = obsapi::obs_data_get_string(settings, "managed_feed_uuid");
        auto *previous = uuid.empty() ? nullptr : obsapi::obs_get_source_by_uuid(uuid.c_str());
        if (!uuid.empty() && !previous) { obsapi::obs_data_release(settings); return; }
        if (previous) {
            if (std::string(obsapi::obs_source_get_unversioned_id(previous)) == "ffmpeg_source") {
                auto *clear = obsapi::obs_data_create();
                obsapi::obs_data_set_string(clear, "input", "");
                obsapi::obs_source_update(previous, clear);
                obsapi::obs_data_release(clear);
                obsapi::obs_source_set_muted(previous, obsapi::obs_data_get_bool(settings, "original_feed_muted"));
            }
            obsapi::obs_source_release(previous);
        }
        for (const char *key : {"feed_source", "feed_uuid", "managed_feed_uuid", "original_feed_muted", "routing_initialized"})
            obsapi::obs_data_unset_user_value(settings, key);
        obsapi::obs_data_release(settings);
        migrated = true;
    }

    void apply_playback_locked()
    {
        if (!desired_url || applied_revision == playback_revision) return;
        playback_started = obsapi::os_gettime_ns(); last_feed_packet = 0;
        applied_position_revision = 0;
        {
            std::lock_guard<std::mutex> audio(pcm_mutex);
            std::lock_guard<std::mutex> analysis(analysis_mutex);
            playing = !desired_url->empty();
            reset();
        }
        auto *previous = obsapi::obs_source_get_settings(feed);
        const bool repeated = !desired_url->empty() && *desired_url == obsapi::obs_data_get_string(previous, "input");
        obsapi::obs_data_release(previous);
        auto *settings = obsapi::obs_data_create();
        obsapi::obs_data_set_string(settings, "input", desired_url->c_str());
        obsapi::obs_data_set_bool(settings, "is_local_file", false);
        obsapi::obs_data_set_bool(settings, "seekable", false);
        obsapi::obs_source_update(feed, settings);
        obsapi::obs_data_release(settings);
        if (repeated) obsapi::obs_source_media_restart(feed);
        applied_revision = playback_revision;
        obsapi::blog(LOG_INFO, "[VaOBS] %s", desired_url->empty() ? "Playback cleared" : "Playback updated; recalibrating");
    }

    void catch_up_playback_locked()
    {
        if (!desired_url || desired_url->empty() || obsapi::os_gettime_ns() < playback_started + 2000000000ULL) return;
        const auto video = std::find_if(videos.begin(), videos.end(), [&](const Video &v) { return v.url == *desired_url; });
        if (video == videos.end() || !video->position.revision || video->position.revision == applied_position_revision) return;
        const auto duration = obsapi::obs_source_media_get_duration(feed);
        if (duration <= 0) return; // Live streams keep their live playback clock.
        const auto state = obsapi::obs_source_media_get_state(feed);
        if (state != OBS_MEDIA_STATE_PLAYING && state != OBS_MEDIA_STATE_PAUSED) return;
        auto *settings = obsapi::obs_source_get_settings(feed);
        const bool seekable = obsapi::obs_data_get_bool(settings, "seekable");
        if (!seekable) {
            obsapi::obs_data_set_bool(settings, "seekable", true);
            obsapi::obs_source_update(feed, settings);
            playback_started = obsapi::os_gettime_ns();
        }
        obsapi::obs_data_release(settings);
        if (!seekable) return; // Wait until the seekable player has reopened.
        const auto &position = video->position;
        // Start slightly ahead, then measure and buffer the remaining delay.
        // This also covers the log's one-second timestamp resolution.
        const int64_t lead = position.paused ? 0 : int64_t(std::min(10000.0, max_delay_ms / 2));
        const int64_t target = std::clamp(position.at(wall_time_ms()) + lead, int64_t(0), std::max(int64_t(0), duration - 1));
        {
            std::lock_guard<std::mutex> audio(pcm_mutex);
            std::lock_guard<std::mutex> analysis(analysis_mutex);
            reset(); last_feed_packet = 0;
        }
        obsapi::obs_source_media_set_time(feed, target);
        obsapi::obs_source_media_play_pause(feed, position.paused);
        applied_position_revision = position.revision;
        obsapi::blog(LOG_INFO, "[VaOBS] Following logged playback position: %lld ms; recalibrating", static_cast<long long>(target));
    }

    void check_playback_locked()
    {
        if (!playing || !desired_url || desired_url->empty() || ready) return;
        const auto state = obsapi::obs_source_media_get_state(feed);
        if (state == OBS_MEDIA_STATE_PAUSED) return;
        const uint64_t now = obsapi::os_gettime_ns();
        const uint64_t last = std::max(playback_started, last_feed_packet.load());
        // Give slow network opens and seeks time to complete. A successful
        // availability probe does not prove that this player's connection works.
        if (now <= last + 20000000000ULL) return;
        if (stream_checks.playback_blocked(*desired_url)) return;
        stream_checks.fail_playback(*desired_url, now / 1000000);
        obsapi::blog(LOG_WARNING, "[VaOBS] Player stopped delivering audio; trying another stream or retrying after cooldown");
    }

    void watch()
    {
        LogMonitor logs;
        std::vector<Client> available;
        std::string published, published_text;
        auto next_scan = std::chrono::steady_clock::time_point{};
        auto next_detail = next_scan;
        // Allow OBS to finish restoring the scene collection before migration.
        {
            std::unique_lock<std::mutex> sleep(sleep_mutex);
            wake.wait_for(sleep, std::chrono::milliseconds(500), [&] { return stopping.load(); });
        }
        while (!stopping) {
            if (obsapi::obs_source_removed(source)) {
                stopping = true; wake.notify_all();
                std::lock_guard<std::mutex> binding(binding_mutex);
                detach();
                auto *settings = obsapi::obs_data_create();
                obsapi::obs_data_set_string(settings, "input", "");
                obsapi::obs_source_update(feed, settings);
                obsapi::obs_data_release(settings);
                std::lock_guard<std::mutex> ui(dock_mutex);
                dock_rows.erase(obsapi::obs_source_get_uuid(source));
                break;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= next_scan) {
                available = running_clients();
                next_scan = now + std::chrono::seconds(1);
            }
            std::string stream_status;
            {
                std::lock_guard<std::mutex> binding(binding_mutex);
                if (retry_requested.exchange(false)) { stream_checks.retry(); logs.retry(); }
                check_playback_locked();
                const auto requests = next_requested.exchange(0);
                const bool advance = search_next.exchange(false);
                auto event = logs.poll(available, client_selection, video_selection, &stream_checks, advance && !requests);
                for (unsigned i = 0; i < requests; ++i) {
                    if (const auto next = logs.next(&stream_checks)) {
                        if (video_selection != "latest") {
                            video_selection = next->key;
                            auto *settings = obsapi::obs_source_get_settings(source);
                            obsapi::obs_data_set_string(settings, "video_selection", video_selection.c_str());
                            obsapi::obs_data_release(settings);
                        }
                        event = next->url;
                    }
                }
                clients = available;
                candidate = logs.candidate();
                videos = logs.videos();
                stream_checks.sync(videos);
                for (const auto &stream : videos) {
                    const auto state = stream_checks.state(stream.url);
                    stream_status += "\n" + stream.key + std::to_string(int(state));
                }
                if (event) { desired_url = event; ++playback_revision; }
                migrate_feed();
                bind_locked();
                apply_playback_locked();
                catch_up_playback_locked();
                log_status = logs.status();
            }
            const auto view = presentation();
            const std::string text = view.text();
            {
                std::lock_guard<std::mutex> ui(dock_mutex);
                dock_rows[obsapi::obs_source_get_uuid(source)] = view.json(obsapi::obs_source_get_name(source), obsapi::obs_source_get_uuid(source));
            }
            std::string current = ready ? "aligned" + text.substr(text.find('\n')) : text;
            if (view.can_next) current += "\nnext";
            for (const auto &client : available) current += "\n" + client.label();
            current += stream_status;
            if (current != published || (text != published_text && now >= next_detail)) {
                published = std::move(current); published_text = text;
                // State/client changes refresh immediately; throttle small
                // delay adjustments so the form stays usable while editing.
                next_detail = now + std::chrono::seconds(10);
                // OBS queues its Properties refresh onto the UI thread. Emit
                // outside our locks: Properties reads the same source state.
                obsapi::obs_source_update_properties(source);
            }
            std::unique_lock<std::mutex> sleep(sleep_mutex);
            wake.wait_for(sleep, std::chrono::milliseconds(500), [&] { return stopping.load(); });
        }
    }

    bool probe_url(const std::string &url)
    {
        auto *settings = obsapi::obs_data_create();
        obsapi::obs_data_set_bool(settings, "is_local_file", false);
        obsapi::obs_data_set_bool(settings, "seekable", false);
        obsapi::obs_data_set_bool(settings, "restart_on_activate", false);
        obsapi::obs_data_set_bool(settings, "close_when_inactive", false);
        obsapi::obs_data_set_bool(settings, "log_changes", false);
        obsapi::obs_data_set_string(settings, "ffmpeg_options", "rw_timeout=5000000");
        auto *check = obsapi::obs_source_create_private("ffmpeg_source", "VaOBS availability check", settings);
        obsapi::obs_data_release(settings);
        if (!check) return false;
        if (!(obsapi::obs_source_get_output_flags(check) & OBS_SOURCE_AUDIO)) {
            obsapi::obs_source_release(check); return false;
        }
        obsapi::obs_source_set_audio_mixers(check, 0);
        obsapi::obs_source_set_muted(check, true);
        obsapi::obs_source_set_volume(check, 0);
        // Tap raw audio without inserting a probe into OBS's recording clock.
        // These private sources are never activated or audible in monitoring.
        obsapi::obs_source_set_monitoring_type(check, OBS_MONITORING_TYPE_MONITOR_ONLY);
        struct AudioSeen {
            std::atomic<bool> received{false};
            static void capture(void *data, obs_source_t *, const audio_data *audio, bool)
            {
                if (audio->frames) static_cast<AudioSeen *>(data)->received = true;
            }
        } seen;
        obsapi::obs_source_add_audio_capture_callback(check, AudioSeen::capture, &seen);
        struct Cleanup {
            obs_source_t *source; AudioSeen *seen;
            ~Cleanup() {
                obsapi::obs_source_remove_audio_capture_callback(source, AudioSeen::capture, seen);
                obsapi::obs_source_release(source);
            }
        } cleanup{check, &seen};
        settings = obsapi::obs_data_create();
        obsapi::obs_data_set_string(settings, "input", url.c_str());
        obsapi::obs_source_update(check, settings);
        obsapi::obs_data_release(settings);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        while (!stopping && !seen.received && std::chrono::steady_clock::now() < deadline) {
            const auto state = obsapi::obs_source_media_get_state(check);
            if (state == OBS_MEDIA_STATE_ERROR || state == OBS_MEDIA_STATE_ENDED) break;
            std::unique_lock<std::mutex> sleep(sleep_mutex);
            wake.wait_for(sleep, std::chrono::milliseconds(100), [&] { return stopping.load(); });
        }
        return seen.received;
    }

    void probe_streams()
    {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stopping) {
            std::optional<std::string> url;
            {
                std::lock_guard<std::mutex> binding(binding_mutex);
                const uint64_t now = obsapi::os_gettime_ns(), packet = last_feed_packet.load();
                // An existing live connection can keep working after its
                // signed URL expires for new connections. Fresh decoded audio
                // is stronger evidence than reopening that URL.
                if (desired_url && !desired_url->empty() && now > playback_started + 8000000000ULL &&
                    packet && now < packet + 2000000000ULL)
                    stream_checks.finish(*desired_url, true, now / 1000000);
                url = stream_checks.take(now / 1000000, desired_url.value_or(""));
            }
            if (url) {
                bool success = false;
                try { success = probe_url(*url); } catch (const std::exception &) {}
                std::lock_guard<std::mutex> binding(binding_mutex);
                stream_checks.finish(*url, success, obsapi::os_gettime_ns() / 1000000);
            } else {
                std::unique_lock<std::mutex> sleep(sleep_mutex);
                wake.wait_for(sleep, std::chrono::milliseconds(250), [&] { return stopping.load(); });
            }
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }

    static uint64_t timestamp(ClockMapper &clock, obs_source_t *input, const audio_data *audio)
    {
        const uint64_t mapped = clock.map(audio->timestamp, obsapi::os_gettime_ns());
        const int64_t sync = obsapi::obs_source_get_sync_offset(input);
        return uint64_t(std::max(int64_t(0), int64_t(mapped) + sync));
    }

    static void reference_audio(void *param, obs_source_t *input, const audio_data *audio, bool)
    {
        auto &self = *static_cast<Aligner *>(param);
        std::lock_guard<std::mutex> lock(self.analysis_mutex);
        if (audio->frames) self.last_reference_packet = obsapi::os_gettime_ns();
        const auto *samples = reinterpret_cast<const float *>(audio->data[0]);
        if (samples && std::any_of(samples, samples + audio->frames, [](float v) { return std::isfinite(v) && double(v) * v > minimum_energy; }))
            self.last_reference_signal = obsapi::os_gettime_ns();
        const float *planes[MAX_AV_PLANES]{};
        for (size_t ch = 0; ch < self.channels; ++ch) planes[ch] = reinterpret_cast<const float *>(audio->data[ch]);
        self.reference_history.append(planes, self.channels, audio->frames, self.sample_rate,
                                      timestamp(self.reference_clock, input, audio));
    }

    static void feed_audio(void *param, obs_source_t *input, const audio_data *audio, bool)
    {
        // OBS capture callbacks expose signal data even when the original
        // source is muted. Only the separate aligned source is recorded.
        auto &self = *static_cast<Aligner *>(param);
        std::lock_guard<std::mutex> pcm_lock(self.pcm_mutex);
        std::lock_guard<std::mutex> analysis(self.analysis_mutex);
        self.last_feed_packet = obsapi::os_gettime_ns();
        const uint64_t time = timestamp(self.feed_clock, input, audio);
        const float *planes[MAX_AV_PLANES]{};
        for (size_t ch = 0; ch < self.channels; ++ch) planes[ch] = reinterpret_cast<const float *>(audio->data[ch]);
        self.pcm.write(planes, audio->frames, time);
        self.feed_history.append(planes, self.channels, audio->frames, self.sample_rate, time);
    }

    void render()
    {
        uint64_t epoch = ~uint64_t(0), packet = 0;
        int64_t origin_frame = 0;
        uint32_t rate = 0; size_t count = 0, frames = 0;
        std::vector<std::vector<float>> storage;
        auto origin = std::chrono::steady_clock::now();
        while (!stopping) {
            obs_source_audio audio{};
            {
                std::lock_guard<std::mutex> lock(pcm_mutex);
                if (epoch != audio_generation.load()) {
                    epoch = audio_generation.load(); rate = sample_rate; count = channels;
                    frames = rate / 100;
                    storage.assign(count, std::vector<float>(frames));
                    origin_frame = TimedBuffer::frame_at(obsapi::os_gettime_ns(), rate);
                    origin = std::chrono::steady_clock::now(); packet = 0;
                }
                const int64_t position = origin_frame + int64_t(packet * frames);
                audio.timestamp = uint64_t(position / rate) * 1000000000ULL + uint64_t(position % rate) * 1000000000ULL / rate;
                float *planes[MAX_AV_PLANES]{};
                for (size_t ch = 0; ch < count; ++ch) planes[ch] = storage[ch].data();
                pcm.render(planes, frames, audio.timestamp, ready.load(), applied_ms.load());
                for (size_t ch = 0; ch < count; ++ch) audio.data[ch] = reinterpret_cast<uint8_t *>(planes[ch]);
            }
            audio.frames = uint32_t(frames); audio.samples_per_sec = rate;
            audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
            audio.speakers = count == 1 ? SPEAKERS_MONO : count == 2 ? SPEAKERS_STEREO :
                count == 3 ? SPEAKERS_2POINT1 : count == 4 ? SPEAKERS_4POINT0 :
                count == 5 ? SPEAKERS_4POINT1 : count == 6 ? SPEAKERS_5POINT1 : SPEAKERS_7POINT1;
            obsapi::obs_source_output_audio(source, &audio);
            ++packet;
            std::unique_lock<std::mutex> sleep(sleep_mutex);
            wake.wait_until(sleep, origin + std::chrono::milliseconds(packet * 10), [&] { return stopping.load(); });
        }
    }

    void analyze()
    {
        while (!stopping) {
            {
                std::unique_lock<std::mutex> lock(sleep_mutex);
                wake.wait_for(lock, std::chrono::seconds(1), [&] { return stopping.load(); });
            }
            if (stopping) break;
            std::vector<float> ref, target;
            std::vector<uint8_t> valid, ref_valid;
            uint64_t epoch; double minimum;
            int64_t window_end_ms = 0;
            int analysis_window_ms = 0;
            bool usable = false, full_range = false;
            {
                std::lock_guard<std::mutex> lock(analysis_mutex);
                if (!connected || !playing) continue;
                epoch = generation; minimum = threshold;
                // The feed may already have paused/ended while the reference
                // still plays its buffered tail. Compare against reference time.
                const int64_t end = reference_history.end() - analysis_rate / 5;
                analysis_window_ms = std::max(int(window_ms), controller.window_ms());
                window_end_ms = end * 1000 / analysis_rate;
                const int64_t n = int64_t(analysis_window_ms) * analysis_rate / 1000;
                const int64_t maximum = std::min(int64_t(max_delay_ms * analysis_rate / 1000), end - n - feed_history.first());
                const double now = double(obsapi::os_gettime_ns()) * analysis_rate / 1e9;
                if (connected && maximum >= 0 && end - n >= reference_history.first() &&
                    now - double(end) < analysis_rate * 2) {
                    const size_t present = reference_history.read(end - n, size_t(n), ref, ref_valid);
                    feed_history.read(end - n - maximum, size_t(n + maximum), target, valid);
                    usable = present >= size_t(n * .98);
                    // If the player never delivers a packet, allow a network
                    // startup grace period before trying another online URL.
                    const bool started = feed_history.end() > 0 ||
                        end - n - reference_history.first() >= int64_t((max_delay_ms + 8000) * analysis_rate / 1000);
                    full_range = started && maximum >= int64_t(max_delay_ms * analysis_rate / 1000);
                }
            }
            Match match;
            if (usable) match = estimate(ref, target, valid, minimum);
            double reference_energy = 0;
            for (const float sample : ref) reference_energy += double(sample) * sample;
            const bool reference_active = usable && reference_energy > ref.size() * minimum_energy;
            {
                std::lock_guard<std::mutex> lock(analysis_mutex);
                if (epoch != generation) continue;
                const bool was_ready = controller.locked;
                const double previous = controller.delay_ms;
                controller.observe(match, window_end_ms, analysis_window_ms, full_range);
                ready = controller.locked; applied_ms = controller.delay_ms;
                match_candidate = controller.confirming() || (controller.locked && match.reliable);
                if (search_progress.observe(controller.locked, match_candidate,
                                            reference_active, full_range)) search_next = true;
                if (controller.locked && (!was_ready || std::abs(previous - controller.delay_ms) >= 2))
                    obsapi::blog(LOG_INFO, "[VaOBS] %s: %.2f ms delay (match %.1f%%)",
                        match.reliable ? "Aligned" : "Holding last reliable delay", controller.delay_ms, match.correlation * 100);
                else if (was_ready && !controller.locked)
                    obsapi::blog(LOG_WARNING, "[VaOBS] Lost reliable alignment; muting output while recalibrating");
            }
        }
    }

    Presentation presentation()
    {
        std::lock_guard<std::mutex> binding(binding_mutex);
        std::lock_guard<std::mutex> analysis(analysis_mutex);
        const uint64_t now = obsapi::os_gettime_ns();
        const auto fresh = [&](uint64_t time) { return time && now < time + 2000000000ULL; };
        size_t checking = 0, online = 0;
        std::string failed;
        Presentation view; view.reference = reference_name;
        for (const auto &video : videos) {
            const auto state = stream_checks.state(video.url);
            checking += state == StreamState::checking; online += state == StreamState::online;
            if (state == StreamState::unavailable && failed.empty()) failed = video.url;
            if (desired_url && video.url == *desired_url) view.stream = video.key;
        }
        if (view.stream.empty() && video_selection != "latest") view.stream = video_selection;
        const bool unavailable = !failed.empty() && ((!playing && !checking) ||
            (desired_url && stream_checks.state(*desired_url) == StreamState::unavailable && !fresh(last_feed_packet)));
        const bool client_available = client_selection == "first" || client_selection == "newest" ? !clients.empty() :
            std::any_of(clients.begin(), clients.end(), [&](const Client &client) { return client.key() == client_selection; });
        const bool paused = playing && obsapi::obs_source_media_get_state(feed) == OBS_MEDIA_STATE_PAUSED;
        if (!client_available) {
            view = {"waiting_client", "Waiting...", "Start the selected VRChat client.", view.stream, reference_name};
        } else if (log_status.find("Cannot read") != std::string::npos) {
            view = {"waiting_client", "Unavailable", "Check access to the selected client's log file.", view.stream, reference_name};
        } else if (paused) {
            view.state = "paused"; view.title = "Paused";
        } else if (!connected || !fresh(last_reference_packet) || !fresh(last_reference_signal)) {
            view.state = "waiting_reference"; view.title = "Listening...";
            view.detail = "No reference audio from '" + reference_name + "'.";
            if (ready) view.detail += " Holding the last delay briefly.";
        } else if (unavailable) {
            const auto url = desired_url && !desired_url->empty() ? *desired_url : failed;
            view.state = "unavailable"; view.title = "Unavailable";
            view.detail = "Retrying in " + std::to_string(stream_checks.retry_in(url, now / 1000000)) +
                " s. Reload the VRChat player if the URL has expired.";
        } else if (!playing && checking) {
            view.state = "checking"; view.title = "Scanning...";
            view.detail = std::to_string(checking) + " stream(s) to check.";
        } else if (!playing) {
            view.state = "waiting_stream"; view.title = "Waiting...";
            view.detail = video_selection == "latest" ? "No video URL detected." : "The selected URL is missing from this instance.";
        } else if (ready) {
            char text[80]; std::snprintf(text, sizeof(text), "Aligned - %.0f ms", applied_ms.load());
            view.state = "aligned"; view.title = text;
            view.detail = match_candidate ? "Reference: '" + reference_name + "'." : "Holding the last reliable delay.";
        } else if (!fresh(last_feed_packet)) {
            view.state = "checking"; view.title = "Connecting...";
            view.detail = "The stream has not delivered audio.";
        } else if (match_candidate || video_selection != "latest") {
            view.state = "syncing"; view.title = "Syncing...";
            view.detail = "Reference: '" + reference_name + "'.";
        } else {
            view.state = "searching";
            view.title = "Searching...";
            view.detail = "Trying stream " + std::to_string(std::min(candidate, std::max(size_t(1), online))) + "/" + std::to_string(online) +
                "; up to " + std::to_string(int(max_delay_ms / 1000)) + " s per stream.";
        }
        view.can_next = available_videos(videos, &stream_checks).size() > 1;
        if (obsapi::obs_source_muted(source) || obsapi::obs_source_get_volume(source) == 0)
            view.detail += " VaOBS is muted in OBS.";
        else if (!obsapi::obs_source_get_audio_mixers(source)) view.detail += " VaOBS is not assigned to a recording track.";
        return view;
    }
    void describe_status(obs_property_t *property)
    {
        const auto view = presentation();
        obsapi::obs_property_set_description(property, view.title.c_str());
        obsapi::obs_property_set_long_description(property, view.text().c_str());
    }

    bool can_next()
    {
        std::lock_guard<std::mutex> binding(binding_mutex);
        return available_videos(videos, &stream_checks).size() > 1;
    }

    void client_choices(obs_property_t *list)
    {
        std::lock_guard<std::mutex> binding(binding_mutex);
        obsapi::obs_property_list_clear(list);
        obsapi::obs_property_list_add_string(list, "First client", "first");
        obsapi::obs_property_list_add_string(list, "Newest client", "newest");
        bool found = client_selection == "first" || client_selection == "newest";
        for (const auto &client : clients) {
            obsapi::obs_property_list_add_string(list, client.label().c_str(), client.key().c_str());
            found = found || client.key() == client_selection;
        }
        if (!found)
            obsapi::obs_property_list_add_string(list, "Selected client (not running)", client_selection.c_str());
    }

    void video_choices(obs_property_t *list)
    {
        std::lock_guard<std::mutex> binding(binding_mutex);
        obsapi::obs_property_list_clear(list);
        obsapi::obs_property_list_add_string(list, "Automatic (match audio)", "latest");
        bool found = video_selection == "latest";
        for (const auto &video : videos) {
            const auto state = stream_checks.state(video.url);
            if (state != StreamState::online && video.key != video_selection) continue;
            const std::string label = video.label() + (state == StreamState::online ? "" :
                state == StreamState::checking ? " (checking)" : " (unavailable; retrying)");
            const auto index = obsapi::obs_property_list_add_string(list, label.c_str(), video.key.c_str());
            obsapi::obs_property_list_item_disable(list, index, state != StreamState::online);
            found = found || video.key == video_selection;
        }
        if (!found) {
            const std::string label = Video{video_selection, {}, 0}.label() + " (not detected)";
            const auto index = obsapi::obs_property_list_add_string(list, label.c_str(), video_selection.c_str());
            obsapi::obs_property_list_item_disable(list, index, true);
        }
    }
};

const char *display_name(void *) { return "VaOBS"; }
void defaults(obs_data_t *data)
{
    obsapi::obs_data_set_default_string(data, "reference_source", "VRChatAudio");
    obsapi::obs_data_set_default_string(data, "client_selection", "first");
    obsapi::obs_data_set_default_string(data, "video_selection", "latest");
    obsapi::obs_data_set_default_int(data, "max_delay_ms", 60000);
    obsapi::obs_data_set_default_int(data, "window_ms", 4000);
    obsapi::obs_data_set_default_double(data, "minimum_correlation", .75);
    obsapi::obs_data_set_default_int(data, "stream_generation", 0);
}
void *create(obs_data_t *settings, obs_source_t *source)
{
    try {
        auto item = std::make_unique<Aligner>(source);
        item->update(settings);
        auto *pointer = item.get();
        item->analyst = std::thread([pointer] { pointer->analyze(); });
        item->output = std::thread([pointer] { pointer->render(); });
        item->watcher = std::thread([pointer] { pointer->watch(); });
        item->prober = std::thread([pointer] { pointer->probe_streams(); });
        return item.release();
    } catch (const std::exception &e) {
        obsapi::blog(LOG_ERROR, "[VaOBS] Could not create aligned audio source: %s", e.what());
        return nullptr;
    }
}
bool enumerate(void *param, obs_source_t *input)
{
    auto *list = static_cast<obs_property_t *>(param);
    if ((obsapi::obs_source_get_output_flags(input) & OBS_SOURCE_AUDIO) && std::string(obsapi::obs_source_get_id(input)) != kind) {
        const char *name = obsapi::obs_source_get_name(input);
        obsapi::obs_property_list_add_string(list, name, name);
    }
    return true;
}
obs_properties_t *properties(void *data)
{
    auto *item = static_cast<Aligner *>(data);
    auto *props = obsapi::obs_properties_create();
    auto *client = obsapi::obs_properties_add_list(props, "client_selection", "VRChat client", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    if (item) item->client_choices(client);
    else {
        obsapi::obs_property_list_add_string(client, "First client", "first");
        obsapi::obs_property_list_add_string(client, "Newest client", "newest");
    }
    auto *video = obsapi::obs_properties_add_list(props, "video_selection", "Video stream", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
    if (item) item->video_choices(video);
    else obsapi::obs_property_list_add_string(video, "Automatic (match audio)", "latest");
    auto *next = obsapi::obs_properties_add_button2(props, "next_stream", "Next stream",
        [](obs_properties_t *, obs_property_t *, void *data) {
            if (auto *item = static_cast<Aligner *>(data)) { ++item->next_requested; item->wake.notify_all(); }
            return true;
        }, item);
    obsapi::obs_property_set_visible(next, item && item->can_next());
    auto *reference = obsapi::obs_properties_add_list(props, "reference_source", "Timing reference", OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
    obsapi::obs_enum_sources(enumerate, reference);
    obsapi::obs_properties_add_int(props, "max_delay_ms", "Maximum delay (ms)", 1000, 120000, 1000);
    auto *status = obsapi::obs_properties_add_text(props, "status", "Waiting...", OBS_TEXT_INFO);
    if (item) item->describe_status(status);
    obsapi::obs_properties_add_button2(props, "retry_streams", "Retry streams",
        [](obs_properties_t *, obs_property_t *, void *data) {
            if (auto *item = static_cast<Aligner *>(data)) { item->retry_requested = true; item->wake.notify_all(); }
            return true;
        }, item);
    obsapi::obs_properties_add_text(props, "version", "VaOBS " VAOBS_VERSION " (" VAOBS_BUILD_ID ")", OBS_TEXT_INFO);
    obsapi::obs_properties_add_button2(props, "refresh_status", "Refresh sources",
        [](obs_properties_t *props, obs_property_t *, void *data) {
            auto *reference = obsapi::obs_properties_get(props, "reference_source");
            obsapi::obs_property_list_clear(reference);
            obsapi::obs_enum_sources(enumerate, reference);
            if (auto *item = static_cast<Aligner *>(data)) {
                item->client_choices(obsapi::obs_properties_get(props, "client_selection"));
                item->video_choices(obsapi::obs_properties_get(props, "video_selection"));
                obsapi::obs_property_set_visible(obsapi::obs_properties_get(props, "next_stream"), item->can_next());
                item->describe_status(obsapi::obs_properties_get(props, "status"));
            }
            return true;
        }, item);
    return props;
}
} // namespace

MODULE_EXPORT const char *obs_module_name() { return "VaOBS"; }
MODULE_EXPORT const char *obs_module_description() { return "Captures clean audio from VRChat videos and aligns it automatically"; }
bool obs_module_load()
{
    if (!obsapi::load()) return false;
    obs_source_info info{};
    info.id = kind; info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
    info.get_name = display_name; info.create = create;
    info.destroy = [](void *data) { delete static_cast<Aligner *>(data); };
    info.update = [](void *data, obs_data_t *settings) { static_cast<Aligner *>(data)->update(settings); };
    info.get_defaults = defaults; info.get_properties = properties;
    info.enum_active_sources = [](void *data, obs_source_enum_proc_t callback, void *param) {
        auto &item = *static_cast<Aligner *>(data);
        callback(item.source, item.feed, param);
    };
    obsapi::obs_register_source_s(&info, sizeof(info));
    obsapi::blog(LOG_INFO, "[VaOBS] Plugin loaded: %s (%s)", VAOBS_VERSION, VAOBS_BUILD_ID);
    return true;
}

MODULE_EXPORT size_t vaobs_read_status(char *buffer, size_t capacity)
{
    std::lock_guard<std::mutex> ui(dock_mutex);
    std::string json = "{\"version\":" + json_string(VAOBS_VERSION) + ",\"build\":" + json_string(VAOBS_BUILD_ID) + ",\"sources\":[";
    for (const auto &row : dock_rows) { if (json.back() != '[') json += ','; json += row.second; }
    json += "]}";
    const size_t needed = json.size() + 1;
    if (buffer && capacity >= needed) std::memcpy(buffer, json.c_str(), needed);
    return needed;
}

MODULE_EXPORT bool vaobs_retry(const char *uuid)
{
    auto *source = uuid ? obsapi::obs_get_source_by_uuid(uuid) : nullptr;
    if (!source) return false;
    const bool valid = std::string(obsapi::obs_source_get_id(source)) == kind && !obsapi::obs_source_removed(source);
    if (valid) {
        auto *settings = obsapi::obs_source_get_settings(source);
        obsapi::obs_data_set_int(settings, "retry_generation", obsapi::obs_data_get_int(settings, "retry_generation") + 1);
        obsapi::obs_source_update(source, settings); obsapi::obs_data_release(settings);
    }
    obsapi::obs_source_release(source); return valid;
}

MODULE_EXPORT bool vaobs_next(const char *uuid)
{
    auto *source = uuid ? obsapi::obs_get_source_by_uuid(uuid) : nullptr;
    if (!source) return false;
    const bool valid = std::string(obsapi::obs_source_get_id(source)) == kind && !obsapi::obs_source_removed(source);
    if (valid) {
        auto *settings = obsapi::obs_source_get_settings(source);
        obsapi::obs_data_set_int(settings, "next_generation", obsapi::obs_data_get_int(settings, "next_generation") + 1);
        obsapi::obs_source_update(source, settings); obsapi::obs_data_release(settings);
    }
    obsapi::obs_source_release(source); return valid;
}

namespace {
void (*stop_ui)() = nullptr;
void open_properties(const char *uuid)
{
    const auto frontend = GetModuleHandleW(L"obs-frontend-api.dll");
    const auto open = frontend ? reinterpret_cast<void (*)(obs_source_t *)>(GetProcAddress(frontend, "obs_frontend_open_source_properties")) : nullptr;
    auto *source = uuid ? obsapi::obs_get_source_by_uuid(uuid) : nullptr;
    if (source) { if (open) open(source); obsapi::obs_source_release(source); }
}
}

MODULE_EXPORT void obs_module_post_load()
{
    const auto frontend = GetModuleHandleW(L"obs-frontend-api.dll");
    if (!frontend) return; // libobs-only integration tests have no frontend.
    const auto add = reinterpret_cast<bool (*)(const char *, const char *, void *)>(GetProcAddress(frontend, "obs_frontend_add_dock_by_id"));
    const auto remove = reinterpret_cast<void (*)(const char *)>(GetProcAddress(frontend, "obs_frontend_remove_dock"));
    if (!add || !remove) { obsapi::blog(LOG_WARNING, "[VaOBS] This OBS version does not support the status dock"); return; }
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&obs_module_post_load), &module);
    wchar_t path[32768]{};
    if (!GetModuleFileNameW(module, path, DWORD(std::size(path)))) return;
    const auto library = fs::path(path).parent_path() / L"vaobs-dock.dll";
    const auto ui = LoadLibraryExW(library.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!ui) { obsapi::blog(LOG_WARNING, "[VaOBS] Could not load the status dock (Windows error %lu); reinstall the complete bundle", GetLastError()); return; }
    const auto start = reinterpret_cast<bool (*)(const VaobsDockHost *)>(GetProcAddress(ui, "vaobs_dock_start"));
    stop_ui = reinterpret_cast<void (*)()>(GetProcAddress(ui, "vaobs_dock_stop"));
    const VaobsDockHost host{2, vaobs_read_status, vaobs_retry, open_properties, add, remove, vaobs_next};
    if (!start || !stop_ui || !start(&host)) obsapi::blog(LOG_WARNING, "[VaOBS] Could not initialize the status dock");
    // Qt may defer deleting a closed dock. Keep its code mapped until process
    // exit; unload stops all callbacks before the audio module is released.
}

MODULE_EXPORT void obs_module_unload()
{
    if (stop_ui) { stop_ui(); stop_ui = nullptr; }
}
