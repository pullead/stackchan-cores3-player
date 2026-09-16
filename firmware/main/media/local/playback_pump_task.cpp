#include "media/local/playback_pump_task.h"

#ifdef ESP_PLATFORM
#include <esp_log.h>

#define TAG "MediaPump"
#endif

namespace media {

PlaybackPumpTask::~PlaybackPumpTask() { stop(); }

void PlaybackPumpTask::run() noexcept {
    running_.store(true, std::memory_order_release);
    while (true) {
        const PumpAction action =
            next_pump_action(controller_.snapshot().state, stop_requested_.load(std::memory_order_acquire));
        if (action == PumpAction::Exit) {
            break;
        }
        if (action == PumpAction::Pump) {
            // Blocks inside the sink on I2S DMA, which paces the loop.
            controller_.pump();
            continue;
        }
#ifdef ESP_PLATFORM
        vTaskDelay(pdMS_TO_TICKS(kIdleDelayMs));
#else
        break;
#endif
    }
    running_.store(false, std::memory_order_release);
}

#ifdef ESP_PLATFORM

void PlaybackPumpTask::trampoline(void* argument) noexcept {
    auto* task = static_cast<PlaybackPumpTask*>(argument);
    task->run();
    task->handle_ = nullptr;
    vTaskDelete(nullptr);
}

bool PlaybackPumpTask::start() noexcept {
    if (handle_ != nullptr) {
        return true;
    }
    stop_requested_.store(false, std::memory_order_release);
    const BaseType_t created = xTaskCreatePinnedToCore(trampoline, "media_pump", kStackBytes, this,
                                                       kPriority, &handle_, kCoreId);
    if (created != pdPASS) {
        handle_ = nullptr;
        ESP_LOGE(TAG, "Could not create the media pump task");
        return false;
    }
    return true;
}

void PlaybackPumpTask::stop() noexcept {
    if (handle_ == nullptr && !running()) {
        return;
    }
    stop_requested_.store(true, std::memory_order_release);
    // The loop checks the flag at most one idle delay or one PCM chunk away.
    // Waiting is deliberate: the controller must not be torn down underneath a
    // decode in flight.  An overrun means the pump is stuck in an SD borrow or
    // an I2S write, which is worth seeing in the log rather than guessing at.
    uint32_t waited_ms = 0;
    while (running()) {
        vTaskDelay(pdMS_TO_TICKS(1));
        if (++waited_ms % 1000 == 0) {
            ESP_LOGW(TAG, "Still waiting for the media pump to stop (%lu ms)",
                     static_cast<unsigned long>(waited_ms));
        }
    }
}

#else  // Host builds have no FreeRTOS; the loop is exercised through run().

bool PlaybackPumpTask::start() noexcept {
    stop_requested_.store(false, std::memory_order_release);
    run();
    return true;
}

void PlaybackPumpTask::stop() noexcept { stop_requested_.store(true, std::memory_order_release); }

#endif

}  // namespace media
