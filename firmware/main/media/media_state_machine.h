#pragma once

#include "media/media_types.h"

namespace media {

class MediaStateMachine {
public:
    PlaybackState state() const noexcept;
    bool transition(PlaybackState next) noexcept;
    // Used after resource cleanup so a failed operation can remain visible
    // without pretending that an Idle->Error transition was a normal flow.
    void mark_error() noexcept { state_ = PlaybackState::Error; }

private:
    PlaybackState state_ = PlaybackState::Idle;
};

}  // namespace media
