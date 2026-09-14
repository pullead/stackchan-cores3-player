#pragma once

#include "media/audio/audio_sink.h"
#include "media/decoder/audio_decoder.h"
#include "media/decoder/audio_stream.h"
#include "media/local/wav_reader.h"
#include "media/media_state_machine.h"
#include "media/audio/pcm_tap.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <memory>

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
};

class LocalPlaybackController {
public:
    static constexpr size_t kPlaybackChunkFrames = 1024;

    explicit LocalPlaybackController(AudioSink& sink);
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
    LocalPlaybackSnapshot snapshot() const;

private:
    bool has_supported_format() const noexcept;
    void fail(std::string error);
    void stop_pipeline();
    void close_sink();

    AudioSink& sink_;
    MediaStateMachine state_machine_;
    WavReader reader_;
    std::unique_ptr<AudioStream> stream_;
    std::unique_ptr<AudioDecoder> decoder_;
    std::string title_;
    std::vector<uint8_t> selected_bytes_;
    size_t total_frames_ = 0;
    size_t played_frames_ = 0;
    std::vector<int16_t> pending_pcm_;
    size_t pending_offset_ = 0;
    bool decoder_eof_ = false;
    std::string error_;
    bool sink_open_ = false;
    PcmTap* pcm_tap_ = nullptr;
};

}  // namespace media
