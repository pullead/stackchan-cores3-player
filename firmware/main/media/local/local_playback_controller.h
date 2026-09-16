#pragma once

#include "media/audio/audio_sink.h"
#include "media/decoder/audio_decoder.h"
#include "media/decoder/audio_stream.h"
#include "media/local/wav_reader.h"
#include "media/media_state_machine.h"
#include "media/audio/pcm_tap.h"
#include "media/media_mode_controller.h"

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
    void stop();
    void stop_for_ai();
    void set_pcm_tap(PcmTap* tap) noexcept { pcm_tap_ = tap; }

    // Allocation-free state query for the audio loop.  snapshot() copies two
    // std::strings, which has no place in a loop that runs per PCM chunk.
    PlaybackState state() const;
    size_t played_frames() const;
    LocalPlaybackSnapshot snapshot() const;

private:
    bool has_supported_format() const noexcept;
    mutable std::recursive_mutex mutex_;
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
    // Interleaved worst case is two samples per frame.  Heap, not stack: 8 KB
    // of stereo PCM would not fit comfortably in the audio task's stack.
    std::vector<int16_t> decode_buffer_;
    std::vector<int16_t> pending_pcm_;
    size_t pending_offset_ = 0;
    bool decoder_eof_ = false;
    std::string error_;
    bool sink_open_ = false;
    // Channel count the sink was opened with; pending PCM is interleaved by it.
    uint8_t sink_channels_ = 0;
    PcmTap* pcm_tap_ = nullptr;
};

}  // namespace media
