// SPDX-License-Identifier: GPL-2.0-or-later
#include "obs-api.hpp"
#include "alignment.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

OBS_DECLARE_MODULE()
OBS_MODULE_AUTHOR("VaOBS contributors")

namespace {
using namespace vaobs;
constexpr const char *kind = "vaobs_auto_align_source";

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
    std::atomic<bool> stopping{false}, ready{false}, connected{false};
    std::atomic<double> applied_ms{0};
    std::atomic<uint64_t> audio_generation{0};
    uint64_t generation = 0;
    int64_t stream_generation = -1;
    uint32_t sample_rate = 48000;
    size_t channels = 2;
    double max_delay_ms = 60000, window_ms = 4000, threshold = .75;
    std::string reference_name = "VRChatAudio", feed_name = "VRChatFeed";
    std::string message = "Waiting for matching audio";
    Timeline reference_history, feed_history;
    ClockMapper reference_clock, feed_clock;
    TimedBuffer pcm;
    Lock controller;
    std::thread analyst, output;

    explicit Aligner(obs_source_t *s) : source(s) {}
    ~Aligner()
    {
        stopping = true; wake.notify_all();
        if (analyst.joinable()) analyst.join();
        if (output.joinable()) output.join();
        std::lock_guard<std::mutex> binding(binding_mutex);
        detach();
    }

    void detach()
    {
        connected = false;
        if (reference) {
            obsapi::obs_source_remove_audio_capture_callback(reference, reference_audio, this);
            obsapi::obs_source_release(reference); reference = nullptr;
        }
        if (feed) {
            obsapi::obs_source_remove_audio_capture_callback(feed, feed_audio, this);
            obsapi::obs_source_release(feed); feed = nullptr;
        }
    }

    // Caller holds pcm_mutex and analysis_mutex, in that order.
    void reset()
    {
        reference_history.clear(); feed_history.clear();
        reference_clock.clear(); feed_clock.clear();
        controller.reset(); ready = false; applied_ms = 0;
        pcm.configure(sample_rate, channels, max_delay_ms);
        message = "Calibrating: output is silent until a reliable match is found";
        ++generation; ++audio_generation;
    }

    void update(obs_data_t *settings)
    {
        const std::string ref = obsapi::obs_data_get_string(settings, "reference_source");
        const std::string target = obsapi::obs_data_get_string(settings, "feed_source");
        const double maximum = std::clamp(double(obsapi::obs_data_get_int(settings, "max_delay_ms")), 1000.0, 120000.0);
        const double window = std::clamp(double(obsapi::obs_data_get_int(settings, "window_ms")), 2000.0, 10000.0);
        const double minimum = std::clamp(obsapi::obs_data_get_double(settings, "minimum_correlation"), .5, .99);
        const int64_t stream = obsapi::obs_data_get_int(settings, "stream_generation");
        obs_audio_info info{}; obsapi::obs_get_audio_info(&info);
        const uint32_t rate = info.samples_per_sec ? info.samples_per_sec : 48000;
        const size_t count = std::max(size_t(1), size_t(get_audio_channels(info.speakers)));
        std::lock_guard<std::mutex> binding(binding_mutex);
        if (ref != reference_name || target != feed_name) detach();
        std::lock_guard<std::mutex> audio(pcm_mutex);
        std::lock_guard<std::mutex> analysis(analysis_mutex);
        const bool changed = ref != reference_name || target != feed_name || maximum != max_delay_ms ||
            window != window_ms || minimum != threshold || stream != stream_generation ||
            rate != sample_rate || count != channels;
        reference_name = ref; feed_name = target; max_delay_ms = maximum;
        window_ms = window; threshold = minimum; stream_generation = stream;
        sample_rate = rate; channels = count;
        if (changed) reset();
    }

    bool acceptable(obs_source_t *candidate)
    {
        return candidate && candidate != source &&
            std::string(obsapi::obs_source_get_id(candidate)) != kind &&
            (obsapi::obs_source_get_output_flags(candidate) & OBS_SOURCE_AUDIO);
    }

    void bind()
    {
        std::lock_guard<std::mutex> binding(binding_mutex);
        if (reference && feed && !obsapi::obs_source_removed(reference) && !obsapi::obs_source_removed(feed) &&
            reference_name == obsapi::obs_source_get_name(reference) && feed_name == obsapi::obs_source_get_name(feed)) return;
        detach();
        auto *ref = obsapi::obs_get_source_by_name(reference_name.c_str());
        auto *target = obsapi::obs_get_source_by_name(feed_name.c_str());
        if (!acceptable(ref) || !acceptable(target) || ref == target) {
            if (ref) obsapi::obs_source_release(ref);
            if (target) obsapi::obs_source_release(target);
            return;
        }
        reference = ref; feed = target;
        {
            std::lock_guard<std::mutex> audio(pcm_mutex);
            std::lock_guard<std::mutex> analysis(analysis_mutex);
            reset();
        }
        obsapi::obs_source_add_audio_capture_callback(reference, reference_audio, this);
        obsapi::obs_source_add_audio_capture_callback(feed, feed_audio, this);
        connected = true;
        obsapi::blog(LOG_INFO, "[VaOBS] Aligning '%s' against '%s'", feed_name.c_str(), reference_name.c_str());
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
            bind();
            std::vector<float> ref, target;
            std::vector<uint8_t> valid, ref_valid;
            uint64_t epoch; double minimum;
            bool usable = false;
            {
                std::lock_guard<std::mutex> lock(analysis_mutex);
                epoch = generation; minimum = threshold;
                // The feed may already have paused/ended while the reference
                // still plays its buffered tail. Compare against reference time.
                const int64_t end = reference_history.end() - analysis_rate / 5;
                const int64_t n = int64_t(window_ms * analysis_rate / 1000);
                const int64_t maximum = std::min(int64_t(max_delay_ms * analysis_rate / 1000), end - n - feed_history.first());
                const double now = double(obsapi::os_gettime_ns()) * analysis_rate / 1e9;
                if (connected && maximum >= 0 && end - n >= reference_history.first() &&
                    now - double(end) < analysis_rate * 2) {
                    const size_t present = reference_history.read(end - n, size_t(n), ref, ref_valid);
                    feed_history.read(end - n - maximum, size_t(n + maximum), target, valid);
                    usable = present >= size_t(n * .98);
                }
            }
            Match match;
            if (usable) match = estimate(ref, target, valid, minimum);
            {
                std::lock_guard<std::mutex> lock(analysis_mutex);
                if (epoch != generation) continue;
                const bool was_ready = controller.locked;
                const double previous = controller.delay_ms;
                controller.observe(match); ready = controller.locked; applied_ms = controller.delay_ms;
                char text[256];
                if (controller.locked)
                    std::snprintf(text, sizeof(text), "%s: %.2f ms delay (match %.1f%%)",
                        match.reliable ? "Aligned" : "Holding last reliable delay", controller.delay_ms, match.correlation * 100);
                else
                    std::snprintf(text, sizeof(text), "%s; aligned output is silent", connected
                        ? "Calibrating or waiting for matching audio" : "Reference or feed source is unavailable");
                message = text;
                if (controller.locked && (!was_ready || std::abs(previous - controller.delay_ms) >= 2))
                    obsapi::blog(LOG_INFO, "[VaOBS] %s", text);
                else if (was_ready && !controller.locked)
                    obsapi::blog(LOG_WARNING, "[VaOBS] Lost reliable alignment; muting output while recalibrating");
            }
        }
    }

    std::string status() { std::lock_guard<std::mutex> lock(analysis_mutex); return message; }
};

const char *display_name(void *) { return "VaOBS Aligned Audio"; }
void defaults(obs_data_t *data)
{
    obsapi::obs_data_set_default_string(data, "reference_source", "VRChatAudio");
    obsapi::obs_data_set_default_string(data, "feed_source", "VRChatFeed");
    obsapi::obs_data_set_default_int(data, "max_delay_ms", 60000);
    obsapi::obs_data_set_default_int(data, "window_ms", 4000);
    obsapi::obs_data_set_default_double(data, "minimum_correlation", .75);
    obsapi::obs_data_set_default_int(data, "stream_generation", 0);
}
void *create(obs_data_t *settings, obs_source_t *source)
{
    try {
        auto item = std::make_unique<Aligner>(source);
        item->update(settings); item->bind();
        auto *pointer = item.get();
        item->analyst = std::thread([pointer] { pointer->analyze(); });
        item->output = std::thread([pointer] { pointer->render(); });
        return item.release();
    } catch (const std::exception &e) {
        obsapi::blog(LOG_ERROR, "[VaOBS] Could not create aligned audio source: %s", e.what());
        return nullptr;
    }
}
bool enumerate(void *param, obs_source_t *input)
{
    if ((obsapi::obs_source_get_output_flags(input) & OBS_SOURCE_AUDIO) && std::string(obsapi::obs_source_get_id(input)) != kind) {
        const char *name = obsapi::obs_source_get_name(input);
        obsapi::obs_property_list_add_string(static_cast<obs_property_t *>(param), name, name);
    }
    return true;
}
obs_properties_t *properties(void *data)
{
    auto *item = static_cast<Aligner *>(data);
    auto *props = obsapi::obs_properties_create();
    for (const auto &field : {std::pair<const char *, const char *>{"reference_source", "Timing reference"},
                              {"feed_source", "Clean media audio source"}}) {
        auto *list = obsapi::obs_properties_add_list(props, field.first, field.second, OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
        obsapi::obs_enum_sources(enumerate, list);
    }
    obsapi::obs_properties_add_int(props, "max_delay_ms", "Maximum delay (ms)", 1000, 120000, 1000);
    obsapi::obs_properties_add_int(props, "window_ms", "Matching window (ms)", 2000, 10000, 500);
    obsapi::obs_properties_add_float(props, "minimum_correlation", "Minimum match confidence", .5, .99, .01);
    const std::string status = item ? item->status() : "Waiting for audio";
    obsapi::obs_properties_add_text(props, "status", status.c_str(), OBS_TEXT_INFO);
    obsapi::obs_properties_add_button2(props, "refresh_status", "Refresh status",
        [](obs_properties_t *, obs_property_t *, void *) { return true; }, item);
    return props;
}
} // namespace

MODULE_EXPORT const char *obs_module_name() { return "VaOBS Audio Alignment"; }
MODULE_EXPORT const char *obs_module_description() { return "Automatically delays clean media audio to match VRChat audio"; }
bool obs_module_load()
{
    if (!obsapi::load()) return false;
    obs_source_info info{};
    info.id = kind; info.type = OBS_SOURCE_TYPE_INPUT; info.output_flags = OBS_SOURCE_AUDIO;
    info.get_name = display_name; info.create = create;
    info.destroy = [](void *data) { delete static_cast<Aligner *>(data); };
    info.update = [](void *data, obs_data_t *settings) { static_cast<Aligner *>(data)->update(settings); };
    info.get_defaults = defaults; info.get_properties = properties;
    obsapi::obs_register_source_s(&info, sizeof(info));
    obsapi::blog(LOG_INFO, "[VaOBS] Aligned audio source loaded");
    return true;
}
