#pragma once

#include <atomic>

#include "media/local/local_playback_controller.h"
#include "media/local/playback_pump_policy.h"

#ifdef ESP_PLATFORM
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

namespace media {

// Runs LocalPlaybackController::pump() on its own FreeRTOS task.
//
// Decoding must not sit in the UI loop: the LVGL frame rate would throttle the
// decoder and cause dropouts, which is exactly the coupling the upstream HiFi
// project avoids by giving audio its own task.  Writing to the sink blocks on
// I2S DMA, which paces this loop naturally; when nothing is playing the task
// sleeps instead of spinning so the expression, servo and UI tasks keep their
// CPU share.
class PlaybackPumpTask {
public:
    // Stack has to hold the controller's interleaved PCM chunk (2048 int16)
    // plus the MP3 decoder's frame work area.
    static constexpr uint32_t kStackBytes = 12288;
    // Above the StackChan expression/servo task (3) so audio wins under load,
    // and on core 1 with the other StackChan tasks, keeping core 0 for Wi-Fi.
    static constexpr unsigned kPriority = 4;
    static constexpr int kCoreId = 1;
    static constexpr uint32_t kIdleDelayMs = 10;

    explicit PlaybackPumpTask(LocalPlaybackController& controller) noexcept
        : controller_(controller) {}
    ~PlaybackPumpTask();

    PlaybackPumpTask(const PlaybackPumpTask&) = delete;
    PlaybackPumpTask& operator=(const PlaybackPumpTask&) = delete;

    bool start() noexcept;
    void stop() noexcept;
    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

private:
    void run() noexcept;
#ifdef ESP_PLATFORM
    static void trampoline(void* argument) noexcept;
#endif

    LocalPlaybackController& controller_;
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> running_{false};
#ifdef ESP_PLATFORM
    TaskHandle_t handle_ = nullptr;
#endif
};

}  // namespace media
