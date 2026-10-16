// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "alignment.hpp"
#include <rubberband/RubberBandStretcher.h>
#include <memory>

namespace vaobs {

// Keep OBS's output clock fixed. Only the amount of buffered source audio
// consumed per output packet changes; the media player is never reconfigured.
class ElasticBuffer {
    using Stretcher = RubberBand::RubberBandStretcher;
    TimedBuffer pcm_;
    std::unique_ptr<Stretcher> stretch_;
    uint32_t rate_ = 0;
    size_t channels_ = 0, skip_ = 0, fade_ = 0, fade_length_ = 0;
    bool active_ = false, elastic_ = false;
    int64_t input_frame_ = 0, previous_end_ = -1;
    double playhead_ = 0, speed_ = 1, wet_ = 0, old_head_ = -1;
    double actual_delay_ms_ = 0;
    std::vector<std::vector<float>> input_;
    std::vector<const float *> input_planes_;
    std::vector<float *> output_planes_;
    static constexpr size_t process_size = 2048;

    void process(size_t frames, bool pad = false)
    {
        for (size_t ch = 0; ch < channels_; ++ch) {
            for (size_t i = 0; i < frames; ++i)
                input_[ch][i] = pad ? 0 : pcm_.sample(ch, double(input_frame_ + int64_t(i)));
            input_planes_[ch] = input_[ch].data();
        }
        stretch_->process(input_planes_.data(), frames, false);
        if (!pad) input_frame_ += int64_t(frames);
    }
    void start(double position, bool crossfade)
    {
        old_head_ = crossfade ? playhead_ : -1;
        fade_ = crossfade ? fade_length_ : 0;
        // Whole-sample positioning avoids accumulating a fractional timestamp
        // error at every new packet. Ordinary correction never resets this.
        playhead_ = double(std::llround(position));
        input_frame_ = int64_t(playhead_); speed_ = 1;
        stretch_->reset(); stretch_->setTimeRatio(1);
        skip_ = stretch_->getStartDelay();
        size_t pad = stretch_->getPreferredStartPad();
        while (pad) { const size_t n = std::min(pad, process_size); process(n, true); pad -= n; }
        active_ = true;
    }
    void finish(float *const *planes, size_t frames, size_t produced, bool locked)
    {
        for (size_t i = 0; i < frames; ++i) {
            wet_ += std::clamp((locked ? 1.0 : 0.0) - wet_, -1.0 / fade_length_, 1.0 / fade_length_);
            const double blend = fade_ ? 1 - double(fade_) / fade_length_ : 1;
            for (size_t ch = 0; ch < channels_; ++ch) {
                const float current = i < produced ? planes[ch][i] : 0;
                const float old = fade_ ? pcm_.sample(ch, old_head_) : current;
                planes[ch][i] = float((old * (1 - blend) + current * blend) * wet_);
            }
            if (fade_) { --fade_; old_head_ += speed_; }
        }
    }
public:
    static int64_t frame_at(uint64_t timestamp, uint32_t rate) { return TimedBuffer::frame_at(timestamp, rate); }
    void configure(uint32_t rate, size_t channels, double max_delay_ms)
    {
        pcm_.configure(rate, channels, max_delay_ms);
        rate_ = rate; channels_ = channels;
        stretch_ = std::make_unique<Stretcher>(rate, channels,
            Stretcher::OptionProcessRealTime | Stretcher::OptionEngineFaster |
            Stretcher::OptionWindowShort | Stretcher::OptionChannelsTogether |
            Stretcher::OptionThreadingNever);
        stretch_->setMaxProcessSize(process_size);
        input_.assign(channels, std::vector<float>(process_size));
        input_planes_.resize(channels); output_planes_.resize(channels);
        fade_length_ = std::max(size_t(1), size_t(rate * .04));
        active_ = elastic_ = false; previous_end_ = -1;
        playhead_ = wet_ = actual_delay_ms_ = 0; speed_ = 1; old_head_ = -1; fade_ = 0;
    }
    void write(const float *const *planes, size_t frames, uint64_t timestamp) { pcm_.write(planes, frames, timestamp); }
    double speed() const { return speed_; }
    double delay_ms() const { return actual_delay_ms_; }
    void render(float *const *planes, size_t frames, uint64_t timestamp, bool locked,
                double delay_ms, double delay_drift_ms_per_second = 0)
    {
        if (!stretch_) return;
        const int64_t first = frame_at(timestamp, rate_);
        if (!locked && wet_ == 0) {
            for (size_t ch = 0; ch < channels_; ++ch)
                if (planes[ch]) std::fill_n(planes[ch], frames, 0.0f);
            active_ = false; speed_ = 1; return;
        }
        const double wanted = double(first) - delay_ms * rate_ / 1000;
        // A stretcher needs some lookahead. With almost no feed lead, retain
        // exact direct playback instead of manufacturing future audio.
        const bool can_stretch = delay_ms >= 80 || (elastic_ && delay_ms >= 40);
        if (!can_stretch) {
            if (active_ && (elastic_ || std::abs(wanted - playhead_) >= rate_ * .0005) && !fade_) {
                old_head_ = playhead_; fade_ = fade_length_;
            }
            for (size_t ch = 0; ch < channels_; ++ch)
                for (size_t i = 0; i < frames; ++i) planes[ch][i] = pcm_.sample(ch, wanted + i);
            speed_ = 1; finish(planes, frames, frames, locked);
            active_ = true; elastic_ = false; actual_delay_ms_ = delay_ms;
            playhead_ = wanted + frames; previous_end_ = first + int64_t(frames); return;
        }
        if (!active_ || (previous_end_ >= 0 && first != previous_end_)) {
            start(wanted, false); elastic_ = true; wet_ = 0;
        } else if (!elastic_) {
            start(wanted, true); elastic_ = true;
        } else if (locked && std::abs(wanted - playhead_) > rate_) {
            // A confirmed seek/buffer jump would take over 50 seconds to
            // repair at 2%. Keep that exceptional recovery short and bounded.
            start(wanted, true);
        }
        const double seconds = double(frames) / rate_;
        const double error_ms = (wanted - playhead_) * 1000 / rate_;
        const double limit = std::abs(error_ms) > 80 ? .02 : .005;
        const double correction = std::abs(error_ms) <= 2 ? 0 : error_ms / 8000;
        const double command = 1 + std::clamp(correction - delay_drift_ms_per_second / 1000, -limit, limit);
        speed_ += std::clamp(command - speed_, -.004 * seconds, .004 * seconds);
        stretch_->setTimeRatio(1 / speed_);

        size_t produced = 0;
        unsigned work = 0;
        while (produced < frames && work++ < 64) {
            const int available = stretch_->available();
            if (available > 0) {
                const size_t n = std::min(size_t(available), skip_ ? std::min(skip_, process_size) : frames - produced);
                for (size_t ch = 0; ch < channels_; ++ch)
                    output_planes_[ch] = skip_ ? input_[ch].data() : planes[ch] + produced;
                const size_t got = stretch_->retrieve(output_planes_.data(), n);
                if (skip_) skip_ -= got; else produced += got;
            } else {
                const size_t n = std::clamp(stretch_->getSamplesRequired(), size_t(64), process_size);
                process(n);
            }
        }
        finish(planes, frames, produced, locked);
        playhead_ += frames * speed_;
        previous_end_ = first + int64_t(frames);
        actual_delay_ms_ = (double(previous_end_) - playhead_) * 1000 / rate_;
    }
};
} // namespace vaobs
