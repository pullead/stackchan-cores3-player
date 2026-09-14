#include "media/audio/volume_policy.h"

#include <cstdio>

static_assert(media::kMutedVolumePercent == 0);

namespace {

bool check_equal(const char* expression, uint8_t actual, uint8_t expected) {
    if (actual == expected) {
        return true;
    }

    std::fprintf(stderr, "check failed: %s (expected %u, got %u)\n", expression,
                 static_cast<unsigned>(expected), static_cast<unsigned>(actual));
    return false;
}

}  // namespace

int main() {
    // Volatile expected values keep the Release build's checks observable.
    volatile uint8_t muted = 0;
    volatile uint8_t normal_volume = 42;
    volatile uint8_t maximum_volume = 100;

    int failures = 0;
    failures += !check_equal("startup_volume(0)", media::startup_volume(0), muted);
    failures += !check_equal("startup_volume(70)", media::startup_volume(70), muted);
    failures += !check_equal("test_volume(100)", media::test_volume(100), muted);
    failures += !check_equal("clamp_user_volume(-1)", media::clamp_user_volume(-1), muted);
    failures += !check_equal("clamp_user_volume(0)", media::clamp_user_volume(0), muted);
    failures += !check_equal("clamp_user_volume(42)", media::clamp_user_volume(42), normal_volume);
    failures += !check_equal("clamp_user_volume(100)", media::clamp_user_volume(100), maximum_volume);
    failures += !check_equal("clamp_user_volume(101)", media::clamp_user_volume(101), maximum_volume);
    failures += !check_equal("restore_persisted_volume(-1)", media::restore_persisted_volume(-1), muted);
    failures += !check_equal("restore_persisted_volume(0)", media::restore_persisted_volume(0), muted);
    failures += !check_equal("restore_persisted_volume(42)", media::restore_persisted_volume(42), normal_volume);
    failures += !check_equal("restore_persisted_volume(101)", media::restore_persisted_volume(101), maximum_volume);
    return failures == 0 ? 0 : 1;
}
