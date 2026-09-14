#include "media/media_mode_controller.h"
#include <cassert>

int main() {
    media::MediaModeController mode;
    const media::AiModeSnapshot before{true, true, true, true};
    assert(mode.enter_media(before));
    assert(mode.media_owned());
    assert(!mode.enter_media(before));
    const auto restored = mode.leave_media();
    assert(restored.active && restored.speaking);
    assert(!mode.media_owned());
}
