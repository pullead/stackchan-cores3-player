#pragma once

#include <cstdint>

#include "media/media_types.h"

namespace media {

enum class PumpAction : uint8_t {
    Pump,  // decode and write the next PCM chunk
    Idle,  // nothing to produce: sleep and let other tasks run
    Exit,  // leave the loop
};

// Decides what the audio task should do next.  Kept separate from the FreeRTOS
// task itself so the scheduling rules are testable on the host.
constexpr PumpAction next_pump_action(PlaybackState state, bool stop_requested) noexcept {
    if (stop_requested) {
        return PumpAction::Exit;
    }
    switch (state) {
        case PlaybackState::Playing:
        case PlaybackState::Buffering:
            return PumpAction::Pump;
        default:
            return PumpAction::Idle;
    }
}

}  // namespace media
