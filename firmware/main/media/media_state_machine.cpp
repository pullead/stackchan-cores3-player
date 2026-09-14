#include "media/media_state_machine.h"

namespace media {

PlaybackState MediaStateMachine::state() const noexcept {
    return state_;
}

bool MediaStateMachine::transition(PlaybackState next) noexcept {
    bool legal = false;

    switch (state_) {
        case PlaybackState::Idle:
            legal = next == PlaybackState::Preparing;
            break;
        case PlaybackState::Preparing:
            legal = next == PlaybackState::Buffering || next == PlaybackState::Error ||
                    next == PlaybackState::Stopping;
            break;
        case PlaybackState::Buffering:
            legal = next == PlaybackState::Playing || next == PlaybackState::Error ||
                    next == PlaybackState::Stopping;
            break;
        case PlaybackState::Playing:
            legal = next == PlaybackState::Paused || next == PlaybackState::Buffering ||
                    next == PlaybackState::Error || next == PlaybackState::Stopping ||
                    next == PlaybackState::PreparingForAi;
            break;
        case PlaybackState::Paused:
            legal = next == PlaybackState::Playing || next == PlaybackState::Stopping ||
                    next == PlaybackState::PreparingForAi;
            break;
        case PlaybackState::Error:
        case PlaybackState::PreparingForAi:
            legal = next == PlaybackState::Stopping;
            break;
        case PlaybackState::Stopping:
            legal = next == PlaybackState::Idle;
            break;
    }

    if (legal) {
        state_ = next;
    }
    return legal;
}

}  // namespace media
