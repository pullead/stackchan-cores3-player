#include "media/local/playback_pump_policy.h"

#include <cstdio>

namespace {

using media::PlaybackState;
using media::PumpAction;
using media::next_pump_action;

bool check(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "check failed: %s\n", what);
    }
    return condition;
}

bool test_active_playback_keeps_pumping() {
    return check(next_pump_action(PlaybackState::Playing, false) == PumpAction::Pump,
                 "playing pumps") &&
           check(next_pump_action(PlaybackState::Buffering, false) == PumpAction::Pump,
                 "buffering pumps so the first decoded frame can open the sink");
}

bool test_inactive_states_yield_the_cpu() {
    // Anything that is not producing PCM must sleep instead of spinning, or the
    // audio task would starve the expression, servo and UI tasks.
    return check(next_pump_action(PlaybackState::Idle, false) == PumpAction::Idle, "idle sleeps") &&
           check(next_pump_action(PlaybackState::Preparing, false) == PumpAction::Idle,
                 "preparing sleeps; start() runs on the caller's thread") &&
           check(next_pump_action(PlaybackState::Paused, false) == PumpAction::Idle, "paused sleeps") &&
           check(next_pump_action(PlaybackState::Error, false) == PumpAction::Idle, "error sleeps") &&
           check(next_pump_action(PlaybackState::Stopping, false) == PumpAction::Idle,
                 "stopping sleeps") &&
           check(next_pump_action(PlaybackState::PreparingForAi, false) == PumpAction::Idle,
                 "an AI handoff sleeps");
}

bool test_stop_request_wins_over_every_state() {
    return check(next_pump_action(PlaybackState::Playing, true) == PumpAction::Exit,
                 "a stop request ends the loop even mid-playback") &&
           check(next_pump_action(PlaybackState::Idle, true) == PumpAction::Exit,
                 "a stop request ends an idle loop");
}

}  // namespace

int main() {
    const bool ok = test_active_playback_keeps_pumping() && test_inactive_states_yield_the_cpu() &&
                    test_stop_request_wins_over_every_state();
    return ok ? 0 : 1;
}
