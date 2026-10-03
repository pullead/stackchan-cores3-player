#pragma once

#include "media/audio/audio_buffer.h"
#include "media/audio/audio_sink.h"
#include "media/decoder/audio_decoder.h"
#include "media/decoder/audio_stream.h"
#include "media/local/wav_reader.h"
#include "media/media_state_machine.h"
#include "media/audio/pcm_tap.h"
#include "media/media_mode_controller.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace media {

struct LocalPlaybackSnapshot {
    PlaybackState state = PlaybackState::Idle;
    std::string title;
    // Streaming decoders report 0 because duration is not known without
    // buffering/scanning the compressed source.
    size_t total_frames = 0;
    size_t played_frames = 0;
    std::string error;
    bool muted = true;
    // Needed to turn played_frames into elapsed time on screen; a muted test
    // otherwise gives no way to tell playback from a frozen page.
    uint32_t sample_rate = 0;
};

// Drives one local track from stream to speaker.
//
// The audio task calls pump() while the UI thread calls select/start/stop and
// snapshot(), so every public method is serialised by one recursive mutex.  It
// is recursive because the failure paths call back into stop_pipeline() while
// already holding the lock.
class LocalPlaybackController {
public:
    // An MP3 frame is 1152 samples per channel, so a 1024-frame buffer can
    // never hold one and the decoder only ever reports "buffer too small".
    // 2048 frames covers MP3 and AAC (1024) with room to spare.
    static constexpr size_t kPlaybackChunkFrames = 2048;

    explicit LocalPlaybackController(AudioSink& sink, MediaModeController* mode = nullptr);
    LocalPlaybackController(const LocalPlaybackController&) = delete;
    LocalPlaybackController& operator=(const LocalPlaybackController&) = delete;
    LocalPlaybackController(LocalPlaybackController&&) = delete;
    LocalPlaybackController& operator=(LocalPlaybackController&&) = delete;

    void select(std::string title, std::vector<uint8_t> wav_bytes);
    void select(std::string title, std::unique_ptr<AudioStream> stream,
                std::unique_ptr<AudioDecoder> decoder);
    bool start();
    void pump();
    // Pausing keeps the stream, decoder and sink open so resuming does not
    // re-read the file; the pump simply stops consuming while paused.
    bool pause();
    bool resume();
    void stop();
    void stop_for_ai();
    void set_pcm_tap(PcmTap* tap) noexcept { pcm_tap_ = tap; }

    // Lock-free progress view.  The audio task holds the mutex for the whole
    // of pump() -- including the SD borrow and the blocking I2S write -- so a
    // UI that took the lock to read progress would only get updates in the
    // gaps between chunks, making the elapsed time jump several seconds.
    struct PlaybackTick {
        PlaybackState state = PlaybackState::Idle;
        size_t played_frames = 0;
        uint32_t sample_rate = 0;
        // Interleave of the samples the PCM tap is being fed with.  A consumer
        // that folds the tap without this cannot tell mono from stereo.
        uint8_t channels = 1;
    };
    PlaybackTick tick() const noexcept {
        return {state_atomic_.load(std::memory_order_relaxed),
                played_atomic_.load(std::memory_order_relaxed),
                rate_atomic_.load(std::memory_order_relaxed),
                channels_atomic_.load(std::memory_order_relaxed)};
    }

    PlaybackState state() const noexcept { return state_atomic_.load(std::memory_order_relaxed); }
    size_t played_frames() const noexcept { return played_atomic_.load(std::memory_order_relaxed); }
    // True when the current track ran out on its own.  A stop the user asked for
    // clears it, so "the song ended" stays distinguishable from "the user
    // stopped it" -- which is what decides whether to advance to the next track.
    bool finished() const noexcept { return finished_atomic_.load(std::memory_order_acquire); }
    LocalPlaybackSnapshot snapshot() const;

private:
    bool has_supported_format() const noexcept;
    void publish_progress() noexcept;

    mutable std::recursive_mutex mutex_;
    // Published for readers that must not block on the audio task.
    std::atomic<PlaybackState> state_atomic_{PlaybackState::Idle};
    std::atomic<size_t> played_atomic_{0};
    std::atomic<uint32_t> rate_atomic_{0};
    std::atomic<uint8_t> channels_atomic_{1};
    std::atomic<bool> finished_atomic_{false};
    void fail(std::string error);
    void stop_pipeline();
    void close_sink();

    AudioSink& sink_;
    MediaStateMachine state_machine_;
    MediaModeController* mode_ = nullptr;
    WavReader reader_;
    std::unique_ptr<AudioStream> stream_;
    std::unique_ptr<AudioDecoder> decoder_;
    std::string title_;
    std::vector<uint8_t> selected_bytes_;
    size_t total_frames_ = 0;
    size_t played_frames_ = 0;
    // Interleaved worst case is two samples per frame.  Not the stack (8 KB of
    // stereo PCM does not belong in the audio task's stack) and not PSRAM
    // (too slow for per-frame access; it starved the I2S DMA), so: internal RAM.
    AudioBuffer decode_buffer_;
    AudioBuffer pending_pcm_;
    std::size_t pending_samples_ = 0;
    size_t pending_offset_ = 0;
    bool decoder_eof_ = false;
    std::string error_;
    bool sink_open_ = false;
    // Channel count the sink was opened with; pending PCM is interleaved by it.
    uint8_t sink_channels_ = 0;
    PcmTap* pcm_tap_ = nullptr;
};

}  // namespace media
