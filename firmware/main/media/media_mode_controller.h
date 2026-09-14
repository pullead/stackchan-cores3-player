#pragma once

namespace media {

// Coordinates ownership only; the AI application remains the owner of its
// actual state and can restore this snapshot when media releases the bus.
struct AiModeSnapshot {
    bool active = false;
    bool speaking = false;
    bool expression_enabled = true;
    bool servos_enabled = true;
};

class MediaAudioOwnership {
public:
    virtual ~MediaAudioOwnership() = default;
    virtual void stop_ai_audio_and_release() noexcept = 0;
    virtual void restore_ai_state(const AiModeSnapshot& state) noexcept = 0;
    virtual void force_output_muted() noexcept = 0;
};

class MediaModeController {
public:
    bool enter_media(const AiModeSnapshot& ai) noexcept;
    AiModeSnapshot leave_media() noexcept;
    bool media_owned() const noexcept { return media_owned_; }
    const AiModeSnapshot& saved_ai() const noexcept { return saved_ai_; }
    void set_ownership(MediaAudioOwnership* ownership) noexcept { ownership_ = ownership; }

private:
    bool media_owned_ = false;
    AiModeSnapshot saved_ai_{};
    MediaAudioOwnership* ownership_ = nullptr;
};

}  // namespace media
