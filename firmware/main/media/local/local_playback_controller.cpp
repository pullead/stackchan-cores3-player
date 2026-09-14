#include "media/local/local_playback_controller.h"

#include <array>
#include <memory>
#include <utility>

namespace media {

LocalPlaybackController::LocalPlaybackController(AudioSink& sink, MediaModeController* mode)
    : sink_(sink), mode_(mode) {}

void LocalPlaybackController::select(std::string title, std::vector<uint8_t> wav_bytes) {
    stop();
    title_ = std::move(title);
    selected_bytes_ = std::move(wav_bytes);
    total_frames_ = 0;
    played_frames_ = 0;
    pending_pcm_.clear();
    pending_offset_ = 0;
    error_.clear();
    decoder_eof_ = false;
}

void LocalPlaybackController::select(std::string title, std::unique_ptr<AudioStream> stream,
                                     std::unique_ptr<AudioDecoder> decoder) {
    stop();
    title_ = std::move(title);
    selected_bytes_.clear();
    stream_ = std::move(stream);
    decoder_ = std::move(decoder);
    total_frames_ = 0;
    played_frames_ = 0;
    pending_pcm_.clear();
    pending_offset_ = 0;
    decoder_eof_ = false;
    error_.clear();
}

bool LocalPlaybackController::start() {
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
    pending_pcm_.clear();
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

    sink_open_ = true;
    state_machine_.transition(decoder_ ? PlaybackState::Buffering : PlaybackState::Playing);
    return true;
}

void LocalPlaybackController::pump() {
    if (state_machine_.state() != PlaybackState::Playing &&
        state_machine_.state() != PlaybackState::Buffering) {
        return;
    }

    if (pending_pcm_.empty()) {
        // Decoder contract: PcmBlock capacity is frames, while this backing
        // array deliberately reserves up to two interleaved int16 samples per
        // frame until the compressed header reveals mono versus stereo.
        std::array<int16_t, kPlaybackChunkFrames * 2> frames{};
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
                PcmFormat sink_format = decoder_->format();
                if (sink_format.channels == 2) sink_format.channels = 1;
                if (!sink_.open(sink_format)) {
                    fail("Audio sink open failed");
                    return;
                }
                sink_open_ = true;
                state_machine_.transition(PlaybackState::Playing);
            }
            frame_count = block.frames;
            if (decoder_->format().channels == 2) {
                for (size_t index = 0; index < frame_count; ++index) {
                    const int32_t left = frames[index * 2];
                    const int32_t right = frames[index * 2 + 1];
                    frames[index] = static_cast<int16_t>((left + right) / 2);
                }
            }
            if (result == AudioDecodeStatus::Eof) decoder_eof_ = true;
        } else {
            frame_count = reader_.read_frames(frames.data(), frames.size());
            if (frame_count == 0) {
                stop_pipeline();
                return;
            }
        }
        pending_pcm_.assign(frames.begin(), frames.begin() + frame_count);
        pending_offset_ = 0;
    }

    const size_t pending_frames = pending_pcm_.size() - pending_offset_;
    const size_t written = sink_.write(pending_pcm_.data() + pending_offset_, pending_frames);
    if (written == 0 || written > pending_frames) {
        fail("Audio sink write failed");
        return;
    }
    played_frames_ += written;
    if (pcm_tap_) {
        // Observation is deliberately after sink admission and never gates
        // playback; a full tap only increments its drop counter.
        pcm_tap_->push(pending_pcm_.data() + pending_offset_, written);
    }
    pending_offset_ += written;
    if (pending_offset_ != pending_pcm_.size()) {
        return;
    }

    pending_pcm_.clear();
    pending_offset_ = 0;
    if ((!decoder_ && reader_.remaining_frames() == 0) || (decoder_ && decoder_eof_)) {
        stop_pipeline();
    }
}

void LocalPlaybackController::stop() {
    stop_pipeline();
    error_.clear();
}

void LocalPlaybackController::stop_for_ai() {
    const PlaybackState state = state_machine_.state();
    if (state == PlaybackState::Playing || state == PlaybackState::Paused) {
        state_machine_.transition(PlaybackState::PreparingForAi);
    }
    stop_pipeline();
}

LocalPlaybackSnapshot LocalPlaybackController::snapshot() const {
    return {
        state_machine_.state(),
        title_,
        total_frames_,
        played_frames_,
        error_,
        true,
    };
}

bool LocalPlaybackController::has_supported_format() const noexcept {
    const PcmFormat& format = reader_.format();
    return format.sample_rate == 24000 && format.channels == 1 && format.bits_per_sample == 16;
}

void LocalPlaybackController::fail(std::string error) {
    error_ = std::move(error);
    state_machine_.transition(PlaybackState::Error);
    stop_pipeline();
    // stop_pipeline() deliberately releases the sink and media ownership;
    // retain the diagnostic state after cleanup for the UI and caller.
    state_machine_.mark_error();
}

void LocalPlaybackController::stop_pipeline() {
    pending_pcm_.clear();
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
}

void LocalPlaybackController::close_sink() {
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
