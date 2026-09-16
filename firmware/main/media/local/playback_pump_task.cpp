#include "media/local/playback_pump_task.h"

#ifdef ESP_PLATFORM
#include <esp_log.h>
#include <esp_timer.h>

#include <esp_heap_caps.h>
#include <freertos/task.h>

#include <string>

#define TAG "MediaPump"
#endif

namespace media {

PlaybackPumpTask::~PlaybackPumpTask() { stop(); }

void PlaybackPumpTask::run() noexcept {
    running_.store(true, std::memory_order_release);
#ifdef ESP_PLATFORM
    // Delivered frames against wall-clock time is the objective check that the
    // retimed I2S clock is right: a 44.1 kHz track must consume 44100 frames
    // per second.  A drifting ratio means wrong pitch; a stalling one means
    // dropouts.  Both are invisible during a muted test without this.
    int64_t measure_start_us = 0;
    size_t measure_start_frames = 0;
    int64_t last_report_us = 0;
    bool measuring = false;
    std::string reported_error;
#endif
    while (true) {
        // state() does not allocate; snapshot() is only taken when something
        // actually needs the strings.
        const PlaybackState state = controller_.state();
        const PumpAction action =
            next_pump_action(state, stop_requested_.load(std::memory_order_acquire));
        if (action == PumpAction::Exit) {
            break;
        }
#ifdef ESP_PLATFORM
        // The controller keeps its failure reason in the snapshot, which the UI
        // shows but the log never did; without it a failed start is invisible.
        if (state == PlaybackState::Error) {
            const LocalPlaybackSnapshot snapshot = controller_.snapshot();
            if (!snapshot.error.empty() && snapshot.error != reported_error) {
                reported_error = snapshot.error;
                ESP_LOGE(TAG, "Playback error: %s", snapshot.error.c_str());
            }
        }
#endif
        if (action == PumpAction::Pump) {
#ifdef ESP_PLATFORM
            const int64_t now_us = esp_timer_get_time();
            if (!measuring && state == PlaybackState::Playing) {
                measuring = true;
                measure_start_us = now_us;
                measure_start_frames = controller_.played_frames();
                last_report_us = now_us;
            } else if (measuring && now_us - last_report_us >= 5000000) {
                const int64_t elapsed_us = now_us - measure_start_us;
                const size_t frames = controller_.played_frames() - measure_start_frames;
                const int64_t rate = elapsed_us > 0 ? (static_cast<int64_t>(frames) * 1000000) / elapsed_us : 0;
                // Stack and heap headroom: a reboot after a while of playing is
                // most often one of these running out, and the panic itself
                // says nothing about how close it had been creeping.
                ESP_LOGI(TAG, "Delivered %u frames in %u ms -> %u frames/s "
                              "(stack free %u, heap free %u, psram free %u)",
                         static_cast<unsigned>(frames), static_cast<unsigned>(elapsed_us / 1000),
                         static_cast<unsigned>(rate),
                         static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                         static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
                last_report_us = now_us;
            }
#endif
            // Blocks inside the sink on I2S DMA, which paces the loop.
            controller_.pump();
            continue;
        }
#ifdef ESP_PLATFORM
        if (measuring) {
            const int64_t elapsed_us = esp_timer_get_time() - measure_start_us;
            const size_t frames = controller_.played_frames() - measure_start_frames;
            const int64_t rate = elapsed_us > 0 ? (static_cast<int64_t>(frames) * 1000000) / elapsed_us : 0;
            ESP_LOGI(TAG, "Playback ended: %u frames in %u ms -> %u frames/s",
                     static_cast<unsigned>(frames), static_cast<unsigned>(elapsed_us / 1000),
                     static_cast<unsigned>(rate));
            measuring = false;
        }
#endif
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
