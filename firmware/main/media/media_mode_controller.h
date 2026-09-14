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

class MediaModeController {
public:
    bool enter_media(const AiModeSnapshot& ai) noexcept;
    AiModeSnapshot leave_media() noexcept;
    bool media_owned() const noexcept { return media_owned_; }
    const AiModeSnapshot& saved_ai() const noexcept { return saved_ai_; }

private:
    bool media_owned_ = false;
    AiModeSnapshot saved_ai_{};
};

}  // namespace media
