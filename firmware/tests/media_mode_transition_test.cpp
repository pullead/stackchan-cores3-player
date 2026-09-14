#include "media/media_mode_controller.h"
#include <cassert>

int main() {
    struct Ownership : media::MediaAudioOwnership {
        int stopped = 0;
        int muted = 0;
        media::AiModeSnapshot restored{};
        void stop_ai_audio_and_release() noexcept override { ++stopped; }
        void restore_ai_state(const media::AiModeSnapshot& state) noexcept override { restored = state; }
        void force_output_muted() noexcept override { ++muted; }
        media::AiModeSnapshot capture_current_ai_state() const noexcept { return {true, true, true, true}; }
    } ownership;
    media::MediaModeController mode;
    mode.set_ownership(&ownership);
    const media::AiModeSnapshot before{true, true, true, true};
    assert(mode.enter_media(before));
    assert(mode.media_owned());
    assert(ownership.stopped == 1 && ownership.muted == 1);
    assert(!mode.enter_media(before));
    const auto restored = mode.leave_media();
    assert(restored.active && restored.speaking);
    assert(ownership.restored.active && ownership.restored.speaking);
    assert(!mode.media_owned());
    mode.leave_media();
    assert(ownership.stopped == 1);
}
