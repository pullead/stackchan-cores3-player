#include <cstdio>

#include "media/media_state_machine.h"
#include "media/media_types.h"

namespace {

constexpr media::PlaybackState kStates[] = {
    media::PlaybackState::Idle,
    media::PlaybackState::Preparing,
    media::PlaybackState::Buffering,
    media::PlaybackState::Playing,
    media::PlaybackState::Paused,
    media::PlaybackState::Error,
    media::PlaybackState::Stopping,
    media::PlaybackState::PreparingForAi,
};

constexpr bool kAllowedTransitions[][sizeof(kStates) / sizeof(kStates[0])] = {
    // To: Idle, Preparing, Buffering, Playing, Paused, Error, Stopping, PreparingForAi
    {false, true, false, false, false, false, false, false},  // Idle
    {false, false, true, false, false, true, true, false},    // Preparing
    {false, false, false, true, false, true, true, false},    // Buffering
    {false, false, true, false, true, true, true, true},      // Playing
    {false, false, false, true, false, false, true, true},    // Paused
    {false, false, false, false, false, false, true, false},  // Error
    {true, false, false, false, false, false, false, false},  // Stopping
    {false, false, false, false, false, false, true, false},  // PreparingForAi
};

bool Check(bool condition, const char* description) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", description);
        return false;
    }
    return true;
}

bool CheckTransition(media::MediaStateMachine& machine, media::PlaybackState next,
                     const char* description) {
    return Check(machine.transition(next), description) && Check(machine.state() == next, description);
}

bool DriveTo(media::MediaStateMachine& machine, media::PlaybackState target) {
    switch (target) {
        case media::PlaybackState::Idle:
            return true;
        case media::PlaybackState::Preparing:
            return CheckTransition(machine, media::PlaybackState::Preparing,
                                   "route to Preparing is legal");
        case media::PlaybackState::Buffering:
            return DriveTo(machine, media::PlaybackState::Preparing) &&
                   CheckTransition(machine, media::PlaybackState::Buffering,
                                   "route to Buffering is legal");
        case media::PlaybackState::Playing:
            return DriveTo(machine, media::PlaybackState::Buffering) &&
                   CheckTransition(machine, media::PlaybackState::Playing,
                                   "route to Playing is legal");
        case media::PlaybackState::Paused:
            return DriveTo(machine, media::PlaybackState::Playing) &&
                   CheckTransition(machine, media::PlaybackState::Paused, "route to Paused is legal");
        case media::PlaybackState::Error:
            return DriveTo(machine, media::PlaybackState::Preparing) &&
                   CheckTransition(machine, media::PlaybackState::Error, "route to Error is legal");
        case media::PlaybackState::Stopping:
            return DriveTo(machine, media::PlaybackState::Preparing) &&
                   CheckTransition(machine, media::PlaybackState::Stopping,
                                   "route to Stopping is legal");
        case media::PlaybackState::PreparingForAi:
            return DriveTo(machine, media::PlaybackState::Playing) &&
                   CheckTransition(machine, media::PlaybackState::PreparingForAi,
                                   "route to PreparingForAi is legal");
    }
    return false;
}

bool TestTransitionTable() {
    for (size_t source_index = 0; source_index < sizeof(kStates) / sizeof(kStates[0]); ++source_index) {
        for (size_t next_index = 0; next_index < sizeof(kStates) / sizeof(kStates[0]); ++next_index) {
            const media::PlaybackState source = kStates[source_index];
            const media::PlaybackState next = kStates[next_index];
            media::MediaStateMachine machine;
            if (!DriveTo(machine, source) || !Check(machine.state() == source, "route reaches source state")) {
                return false;
            }

            const bool allowed = kAllowedTransitions[source_index][next_index];
            const bool transitioned = machine.transition(next);
            if (!Check(transitioned == allowed, "transition result matches the allowed table")) {
                return false;
            }
            if (!Check(machine.state() == (allowed ? next : source),
                       "transition leaves the expected state")) {
                return false;
            }
        }
    }
    return true;
}

bool TestPcmFormatValidation() {
    const media::PcmFormat valid_mono{16000, 1, 16};
    const media::PcmFormat valid_stereo{44100, 2, 16};
    const media::PcmFormat invalid_rate{0, 1, 16};
    const media::PcmFormat invalid_zero_channels{16000, 0, 16};
    const media::PcmFormat invalid_channels{16000, 3, 16};
    const media::PcmFormat invalid_bits{16000, 1, 24};

    return Check(valid_mono.valid(), "16-bit mono PCM is valid") &&
           Check(valid_stereo.valid(), "16-bit stereo PCM is valid") &&
           Check(!invalid_rate.valid(), "zero PCM sample rate is invalid") &&
           Check(!invalid_zero_channels.valid(), "zero-channel PCM is invalid") &&
           Check(!invalid_channels.valid(), "three-channel PCM is invalid") &&
           Check(!invalid_bits.valid(), "24-bit PCM is invalid");
}

}  // namespace

int main() {
    return TestTransitionTable() && TestPcmFormatValidation()
               ? 0
               : 1;
}
