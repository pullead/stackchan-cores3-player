#include "media/local/local_playback_controller.h"

#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

namespace media {

LocalPlaybackController::LocalPlaybackController(AudioSink& sink, MediaModeController* mode)
    : sink_(sink), mode_(mode) {
    // Allocated once; the audio path must never allocate per chunk.
    decode_buffer_.allocate(kPlaybackChunkFrames * 2);
    pending_pcm_.allocate(kPlaybackChunkFrames * 2);
}

void LocalPlaybackController::select(std::string title, std::vector<uint8_t> wav_bytes) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    stop();
    title_ = std::move(title);
    selected_bytes_ = std::move(wav_bytes);
    total_frames_ = 0;
    played_frames_ = 0;
    pending_samples_ = 0;
    pending_offset_ = 0;
    error_.clear();
    decoder_eof_ = false;
}

void LocalPlaybackController::select(std::string title, std::unique_ptr<AudioStream> stream,
                                     std::unique_ptr<AudioDecoder> decoder) {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    stop();
    title_ = std::move(title);
    selected_bytes_.clear();
    stream_ = std::move(stream);
    decoder_ = std::move(decoder);
    total_frames_ = 0;
    played_frames_ = 0;
    pending_samples_ = 0;
    pending_offset_ = 0;
    decoder_eof_ = false;
    error_.clear();
}

bool LocalPlaybackController::start() {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (state_machine_.state() != PlaybackState::Idle ||
        (selected_bytes_.empty() && (!stream_ || !decoder_))) {
        error_ = "No local track selected";
        return false;
    }

    error_.clear();
    if (mode_ && !mode_->enter_media({})) {
        error_ = "Media audio ownership unavailable";
        return false;
    }
    total_frames_ = 0;
    played_frames_ = 0;
    pending_samples_ = 0;
    pending_offset_ = 0;
    state_machine_.transition(PlaybackState::Preparing);
    PcmFormat format{};
    PcmFormat sink_format{};
    if (decoder_) {
        if (!stream_->is_open()) {
            fail("Audio stream is not open");
            return false;
        }
        if (decoder_->open(*stream_) != AudioDecodeStatus::Ok) {
            fail("Audio decoder open failed");
            return false;
        }
        // MP3 headers are parsed by the first decode call.  Do not require a
        // valid format here: a freshly opened decoder legitimately reports an
        // empty format until it has produced its first PCM frame.
    } else {
        if (!reader_.open(selected_bytes_)) {
            fail("Invalid or unsupported WAV");
            return false;
        }
        if (!has_supported_format()) {
            fail("Unsupported PCM format");
            return false;
        }
        total_frames_ = reader_.remaining_frames();
        format = reader_.format();
        sink_format = format;
    }
    state_machine_.transition(PlaybackState::Buffering);
    if (!decoder_ && !sink_.open(sink_format)) {
        sink_open_ = false;
        fail("Audio sink open failed");
        return false;
    }
    sink_channels_ = decoder_ ? 0 : sink_format.channels;

    // Compressed streams open the sink after the first decoded frame reveals
    // their PCM format; WAV has already opened it above.
    sink_open_ = !decoder_;
    state_machine_.transition(decoder_ ? PlaybackState::Buffering : PlaybackState::Playing);
    publish_progress();
    return true;
}

void LocalPlaybackController::pump() {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    if (state_machine_.state() != PlaybackState::Playing &&
        state_machine_.state() != PlaybackState::Buffering) {
        return;
    }

    if (pending_samples_ == 0) {
        // Decoder contract: PcmBlock capacity is frames, while this backing
        // buffer deliberately reserves up to two interleaved int16 samples per
        // frame until the compressed header reveals mono versus stereo.
        AudioBuffer& frames = decode_buffer_;
        size_t frame_count = 0;
        if (decoder_) {
            const size_t capacity = kPlaybackChunkFrames;
            PcmBlock block{frames.data(), capacity, 0};
            const AudioDecodeStatus result = decoder_->decode(block);
            if (block.samples != frames.data() || block.capacity_frames != capacity ||
                !block.valid() || block.frames > capacity) {
                fail("Audio decoder returned invalid PCM block");
                return;
            }
            if (result == AudioDecodeStatus::Eof && block.frames == 0) {
                decoder_eof_ = true;
                stop_pipeline();
                return;
            }
            if ((result != AudioDecodeStatus::Ok &&
                 !(result == AudioDecodeStatus::Eof && block.frames > 0)) ||
                block.frames > frames.size()) {
                fail("Audio decoder failed");
                return;
            }
            if (!decoder_->format().valid()) {
                fail("Unsupported decoded PCM format");
                return;
            }
            if (!sink_open_) {
                // The sink retimes the shared I2S channel to the decoded
                // format, so stereo music plays as stereo instead of being
                // folded down to the AI path's mono channel.
                const PcmFormat sink_format = decoder_->format();
                if (!sink_.open(sink_format)) {
                    fail("Audio sink open failed");
                    return;
                }
                sink_open_ = true;
                sink_channels_ = sink_format.channels;
                state_machine_.transition(PlaybackState::Playing);
                publish_progress();
            }
            frame_count = block.frames;
            if (result == AudioDecodeStatus::Eof) decoder_eof_ = true;
        } else {
            // frames[] holds two int16 per frame for stereo compressed audio,
            // but read_frames() counts frames: keep the bounded chunk size.
            frame_count = reader_.read_frames(frames.data(), kPlaybackChunkFrames);
            if (frame_count == 0) {
                stop_pipeline();
                return;
            }
        }
        const size_t channels = sink_channels_ == 0 ? 1 : sink_channels_;
        // Copy into the pending buffer rather than reallocating it each chunk.
        pending_samples_ = frame_count * channels;
        std::memcpy(pending_pcm_.data(), frames.data(), pending_samples_ * sizeof(int16_t));
        pending_offset_ = 0;
    }

    const size_t channels = sink_channels_ == 0 ? 1 : sink_channels_;
    const size_t pending_frames = (pending_samples_ - pending_offset_) / channels;
    const size_t written = sink_.write(pending_pcm_.data() + pending_offset_, pending_frames);
    if (written == 0 || written > pending_frames) {
        fail("Audio sink write failed");
        return;
    }
    played_frames_ += written;
    publish_progress();
    if (pcm_tap_) {
        // Observation is deliberately after sink admission and never gates
        // playback; a full tap only increments its drop counter.  The tap
        // counts interleaved samples, not frames.
        pcm_tap_->push(pending_pcm_.data() + pending_offset_, written * channels);
    }
    pending_offset_ += written * channels;
    if (pending_offset_ != pending_samples_) {
        return;
    }

    pending_samples_ = 0;
    pending_offset_ = 0;
    if ((!decoder_ && reader_.remaining_frames() == 0) || (decoder_ && decoder_eof_)) {
        stop_pipeline();
    }
}

void LocalPlaybackController::stop() {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    stop_pipeline();
    error_.clear();
}

void LocalPlaybackController::stop_for_ai() {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    const PlaybackState state = state_machine_.state();
    if (state == PlaybackState::Playing || state == PlaybackState::Paused) {
        state_machine_.transition(PlaybackState::PreparingForAi);
    }
    stop_pipeline();
}

void LocalPlaybackController::publish_progress() noexcept {
    state_atomic_.store(state_machine_.state(), std::memory_order_relaxed);
    played_atomic_.store(played_frames_, std::memory_order_relaxed);
    const PcmFormat& format = decoder_ ? decoder_->format() : reader_.format();
    rate_atomic_.store(format.sample_rate, std::memory_order_relaxed);
}

LocalPlaybackSnapshot LocalPlaybackController::snapshot() const {
    std::lock_guard<std::recursive_mutex> guard(mutex_);
    const PcmFormat& format = decoder_ ? decoder_->format() : reader_.format();
    return {
        state_machine_.state(),
        title_,
        total_frames_,
        played_frames_,
        error_,
        true,
        format.sample_rate,
    };
}

bool LocalPlaybackController::has_supported_format() const noexcept {
    // The sink owns the clock whitelist; this only rejects structurally broken
    // WAV headers before the sink is touched.
    return reader_.format().valid();
}

void LocalPlaybackController::fail(std::string error) {
    error_ = std::move(error);
    state_machine_.transition(PlaybackState::Error);
    stop_pipeline();
    // stop_pipeline() deliberately releases the sink and media ownership;
    // retain the diagnostic state after cleanup for the UI and caller.
    state_machine_.mark_error();
    publish_progress();
}

void LocalPlaybackController::stop_pipeline() {
    pending_samples_ = 0;
    pending_offset_ = 0;
    if (state_machine_.state() == PlaybackState::Idle) {
        if (stream_) stream_->close();
        stream_.reset();
        decoder_.reset();
        return;
    }
    if (state_machine_.state() != PlaybackState::Stopping) {
        state_machine_.transition(PlaybackState::Stopping);
    }
    close_sink();
    if (mode_ && mode_->media_owned()) mode_->leave_media();
    state_machine_.transition(PlaybackState::Idle);
    publish_progress();
}

void LocalPlaybackController::close_sink() {
    sink_channels_ = 0;
    if (sink_open_) {
        sink_.flush();
        sink_.close();
        sink_open_ = false;
    }
    if (stream_) {
        stream_->close();
        stream_.reset();
    }
    decoder_.reset();
}

}  // namespace media
