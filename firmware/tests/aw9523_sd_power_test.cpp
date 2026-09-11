#include "hal/board/aw9523_sd_power.h"

#include <cstdint>

int main() {
    constexpr uint8_t initial_config = 0b00011000;
    constexpr uint8_t initial_output = 0b00000111;

    static_assert(board::aw9523::sd_enable_config(initial_config) == 0b00001000);
    static_assert(board::aw9523::sd_enable_output(initial_output) == 0b00010111);

    return 0;
}
