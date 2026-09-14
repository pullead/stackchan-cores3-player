#include "media/audio/pcm_tap.h"
#include <cassert>

int main() {
    media::PcmTap tap;
    int16_t input[media::PcmTap::kCapacity + 4]{};
    assert(tap.push(input, sizeof(input) / sizeof(input[0])) == media::PcmTap::kCapacity);
    assert(tap.dropped() == 4);
    int16_t output[media::PcmTap::kCapacity]{};
    assert(tap.pop(output, media::PcmTap::kCapacity) == media::PcmTap::kCapacity);
    assert(tap.available() == 0);
}
