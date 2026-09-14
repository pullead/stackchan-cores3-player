#include "media/media_mode_controller.h"

namespace media {

bool MediaModeController::enter_media(const AiModeSnapshot& ai) noexcept {
    if (media_owned_) return false;
    saved_ai_ = ai;
    media_owned_ = true;
    return true;
}

AiModeSnapshot MediaModeController::leave_media() noexcept {
    const AiModeSnapshot restored = saved_ai_;
    media_owned_ = false;
    saved_ai_ = {};
    return restored;
}

}  // namespace media
