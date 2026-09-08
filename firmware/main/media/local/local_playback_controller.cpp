#include "media/local/local_playback_controller.h"

#include <algorithm>
#include <array>
#include <utility>

namespace media {

LocalPlaybackController::LocalPlaybackController(AudioSink& sink) : sink_(sink) {}

void LocalPlaybackController::select(std::string title, std::vector<uint8_t> wav_bytes) {
    stop();
    title_ = std::move(title);
    selected_bytes_ = std::move(wav_bytes);
    total_frames_ = 0;
    played_frames_ = 0;
    error_.clear();
}

bool LocalPlaybackController::start() {
    if (state_machine_.state() != PlaybackState::Idle || selected_bytes_.empty()) {
        error_ = "No local WAV selected";
        return false;
    }

    error_.clear();
    total_frames_ = 0;
    played_frames_ = 0;
    state_machine_.transition(PlaybackState::Preparing);
    if (!reader_.open(selected_bytes_)) {
        fail("Invalid or unsupported WAV");
        return false;
    }
    if (!has_supported_format()) {
        fail("Unsupported PCM format");
        return false;
    }

    total_frames_ = reader_.remaining_frames();
    state_machine_.transition(PlaybackState::Buffering);
    if (!sink_.open(reader_.format())) {
        sink_open_ = true;
        fail("Audio sink open failed");
        return false;
    }

    sink_open_ = true;
    state_machine_.transition(PlaybackState::Playing);
    return true;
}

void LocalPlaybackController::pump() {
    if (state_machine_.state() != PlaybackState::Playing) {
        return;
    }

    std::array<int16_t, kPlaybackChunkFrames> frames{};
    const size_t frame_count = reader_.read_frames(frames.data(), frames.size());
    if (frame_count == 0) {
        stop_pipeline();
        return;
    }

    const size_t written = sink_.write(frames.data(), frame_count);
    played_frames_ += std::min(written, frame_count);
    if (written != frame_count) {
        fail("Audio sink write failed");
        return;
    }
    if (reader_.remaining_frames() == 0) {
        stop_pipeline();
    }
}

void LocalPlaybackController::stop() {
    stop_pipeline();
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
}

void LocalPlaybackController::stop_pipeline() {
    if (state_machine_.state() == PlaybackState::Idle) {
        return;
    }
    if (state_machine_.state() != PlaybackState::Stopping) {
        state_machine_.transition(PlaybackState::Stopping);
    }
    close_sink();
    state_machine_.transition(PlaybackState::Idle);
}

void LocalPlaybackController::close_sink() {
    if (!sink_open_) {
        return;
    }
    sink_.flush();
    sink_.close();
    sink_open_ = false;
}

}  // namespace media
