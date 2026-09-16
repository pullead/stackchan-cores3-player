#include "media/storage/byte_ring.h"

#include <cstdio>
#include <cstring>
#include <numeric>
#include <vector>

namespace {

bool check(bool condition, const char* what) {
    if (!condition) std::fprintf(stderr, "check failed: %s\n", what);
    return condition;
}

bool test_capacity_is_rounded_up() {
    media::ByteRing ring;
    return check(ring.allocate(1000), "allocation succeeds") &&
           check(ring.capacity() == 1024, "capacity rounds up to a power of two") &&
           check(ring.available() == 0, "a fresh ring is empty") &&
           check(ring.free_space() == 1024, "a fresh ring is all free");
}

bool test_round_trip_preserves_bytes() {
    media::ByteRing ring;
    ring.allocate(16);
    const uint8_t input[] = {1, 2, 3, 4, 5};
    uint8_t output[5] = {};

    return check(ring.write(input, 5) == 5, "write accepts all bytes") &&
           check(ring.available() == 5, "written bytes are visible") &&
           check(ring.read(output, 5) == 5, "read returns all bytes") &&
           check(std::memcmp(input, output, 5) == 0, "bytes survive the round trip") &&
           check(ring.available() == 0, "the ring empties");
}

bool test_writes_stop_at_capacity() {
    media::ByteRing ring;
    ring.allocate(8);
    const std::vector<uint8_t> input(16, 7);

    return check(ring.write(input.data(), 16) == 8, "write is capped at capacity") &&
           check(ring.free_space() == 0, "a full ring has no free space") &&
           check(ring.write(input.data(), 1) == 0, "a full ring accepts nothing");
}

bool test_reads_stop_at_available() {
    media::ByteRing ring;
    ring.allocate(8);
    const uint8_t input[] = {9, 9, 9};
    uint8_t output[8] = {};
    ring.write(input, 3);

    return check(ring.read(output, 8) == 3, "read is capped at what is available") &&
           check(ring.read(output, 1) == 0, "an empty ring yields nothing");
}

bool test_wrapping_keeps_byte_order() {
    media::ByteRing ring;
    ring.allocate(8);
    std::vector<uint8_t> input(8);
    std::iota(input.begin(), input.end(), 0);
    uint8_t output[8] = {};

    // Fill, drain most of it, then write across the wrap point.
    ring.write(input.data(), 8);
    ring.read(output, 6);
    if (!check(ring.write(input.data(), 5) == 5, "space freed by reading is reusable")) return false;

    uint8_t drained[8] = {};
    const std::size_t count = ring.read(drained, 8);
    // Remaining from the first fill (6, 7) followed by the wrapped write.
    const uint8_t expected[] = {6, 7, 0, 1, 2, 3, 4};
    return check(count == 7, "everything written is readable") &&
           check(std::memcmp(drained, expected, 7) == 0, "wrapped data keeps its order");
}

bool test_unallocated_ring_is_inert() {
    media::ByteRing ring;
    const uint8_t input[] = {1};
    uint8_t output[1] = {};
    return check(ring.write(input, 1) == 0, "writing without a buffer does nothing") &&
           check(ring.read(output, 1) == 0, "reading without a buffer does nothing") &&
           check(ring.capacity() == 0, "an unallocated ring has no capacity");
}

}  // namespace

int main() {
    const bool ok = test_capacity_is_rounded_up() && test_round_trip_preserves_bytes() &&
                    test_writes_stop_at_capacity() && test_reads_stop_at_available() &&
                    test_wrapping_keeps_byte_order() && test_unallocated_ring_is_inert();
    return ok ? 0 : 1;
}
