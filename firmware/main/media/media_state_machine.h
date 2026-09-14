#pragma once

#include "media/media_types.h"

namespace media {

class MediaStateMachine {
public:
    PlaybackState state() const noexcept;
    bool transition(PlaybackState next) noexcept;

private:
    PlaybackState state_ = PlaybackState::Idle;
};

}  // namespace media
