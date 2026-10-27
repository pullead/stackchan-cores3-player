#pragma once

#include <cstdint>

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace media {

// 阻塞至少 ms 毫秒，并且至少一个节拍。
//
// 本固件 CONFIG_FREERTOS_HZ=100，一个节拍 10 ms，pdMS_TO_TICKS(1) 与
// pdMS_TO_TICKS(5) 都等于 0，而 vTaskDelay(0) 只是让给同级或更高优先级的
// 任务，根本不阻塞。2026-10-27 实测：播放任务（优先级 4）在这样的循环里
// 等预读任务（同核、优先级 3）退出，后者永远轮不到运行，整机卡死。
inline void sleep_at_least_ms(uint32_t ms) {
#ifdef ESP_PLATFORM
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    vTaskDelay(ticks > 0 ? ticks : 1);
#else
    (void)ms;
#endif
}

// sleep_at_least_ms(ms) 实际阻塞的毫秒数，供等待超时计数使用。
inline uint32_t slept_ms(uint32_t ms) {
#ifdef ESP_PLATFORM
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    return static_cast<uint32_t>((ticks > 0 ? ticks : 1) * portTICK_PERIOD_MS);
#else
    return ms;
#endif
}

}  // namespace media
