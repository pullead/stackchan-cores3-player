#include <cstdio>

#include "media/media_state_machine.h"
#include "media/media_types.h"

namespace {

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

bool TestHappyRoute() {
    media::MediaStateMachine machine;
    return Check(machine.state() == media::PlaybackState::Idle, "initial state is Idle") &&
           CheckTransition(machine, media::PlaybackState::Preparing, "Idle transitions to Preparing") &&
           CheckTransition(machine, media::PlaybackState::Buffering, "Preparing transitions to Buffering") &&
           CheckTransition(machine, media::PlaybackState::Playing, "Buffering transitions to Playing") &&
           CheckTransition(machine, media::PlaybackState::Paused, "Playing transitions to Paused") &&
           CheckTransition(machine, media::PlaybackState::Playing, "Paused transitions to Playing") &&
           CheckTransition(machine, media::PlaybackState::PreparingForAi,
                           "Playing transitions to PreparingForAi") &&
           CheckTransition(machine, media::PlaybackState::Stopping,
                           "PreparingForAi transitions to Stopping") &&
           CheckTransition(machine, media::PlaybackState::Idle, "Stopping transitions to Idle");
}

bool TestInvalidTransitionRetainsState() {
    media::MediaStateMachine machine;
    return Check(!machine.transition(media::PlaybackState::Playing), "Idle rejects Playing") &&
           Check(machine.state() == media::PlaybackState::Idle,
                 "illegal Idle to Playing transition retains Idle");
}

bool TestErrorRoute() {
    media::MediaStateMachine machine;
    return CheckTransition(machine, media::PlaybackState::Preparing, "Idle transitions to Preparing") &&
           CheckTransition(machine, media::PlaybackState::Error, "Preparing transitions to Error") &&
           CheckTransition(machine, media::PlaybackState::Stopping, "Error transitions to Stopping") &&
           CheckTransition(machine, media::PlaybackState::Idle, "Stopping transitions to Idle");
}

bool TestPcmFormatValidation() {
    const media::PcmFormat valid_mono{16000, 1, 16};
    const media::PcmFormat valid_stereo{44100, 2, 16};
    const media::PcmFormat invalid_rate{0, 1, 16};
    const media::PcmFormat invalid_channels{16000, 3, 16};
    const media::PcmFormat invalid_bits{16000, 1, 24};

    return Check(valid_mono.valid(), "16-bit mono PCM is valid") &&
           Check(valid_stereo.valid(), "16-bit stereo PCM is valid") &&
           Check(!invalid_rate.valid(), "zero PCM sample rate is invalid") &&
           Check(!invalid_channels.valid(), "three-channel PCM is invalid") &&
           Check(!invalid_bits.valid(), "24-bit PCM is invalid");
}

}  // namespace

int main() {
    return TestHappyRoute() && TestInvalidTransitionRetainsState() && TestErrorRoute() &&
                   TestPcmFormatValidation()
               ? 0
               : 1;
}
