#include "media/audio/volume_policy.h"

#include <cassert>

static_assert(media::kMutedVolumePercent == 0);

int main() {
    assert(media::startup_volume(0) == 0);
    assert(media::startup_volume(70) == 0);
    assert(media::test_volume(100) == 0);
    assert(media::clamp_user_volume(-1) == 0);
    assert(media::clamp_user_volume(42) == 42);
    assert(media::clamp_user_volume(101) == 100);
    return 0;
}
