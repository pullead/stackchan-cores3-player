#include "hal/board/spi3_display_handoff.h"

#include <utility>

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "hal/hal.h"
#endif

namespace board {

Spi3DisplayHandoff::Spi3DisplayHandoff(Spi3DisplayHandoffOperations operations) noexcept
    : operations_(operations) {}

bool Spi3DisplayHandoff::configure(Spi3DisplayHandoffOperations operations) noexcept {
    if (acquired_) {
        last_error_ = "cannot configure SPI3 handoff while acquired";
        return false;
    }
    operations_ = operations;
    last_error_.clear();
    return true;
}

bool Spi3DisplayHandoff::acquire() {
    bool release_required = false;
    return acquire(release_required);
}

bool Spi3DisplayHandoff::acquire(bool& release_required) {
    release_required = false;
    if (acquired_) {
        last_error_ = "SPI3 display handoff is already acquired";
        return false;
    }
    if (!is_configured()) {
        last_error_ = "SPI3 display handoff is not initialized";
        return false;
    }

    last_error_.clear();
    operations_.lock_display(operations_.context);
    acquired_ = true;

    if (!operations_.drain_display(operations_.context, last_error_)) {
        if (last_error_.empty()) {
            last_error_ = "failed to drain display transactions";
        }
        fail_and_unlock(false);
        return false;
    }
    if (!operations_.set_shared_pin_input(operations_.context, last_error_)) {
        if (last_error_.empty()) {
            last_error_ = "failed to release the shared display pin";
        }
        fail_and_unlock(true);
        release_required = acquired_;
        return false;
    }

    return true;
}

bool Spi3DisplayHandoff::release() noexcept {
    if (!acquired_) {
        return true;
    }

    std::string release_error;
    const bool cs_high = operations_.set_sd_chip_select_high(operations_.context, release_error);
    if (!cs_high) {
        last_error_ = release_error.empty() ? "failed to deselect the SD card" : std::move(release_error);
        return false;
    }

    operations_.unlock_display(operations_.context);
    acquired_ = false;
    last_error_.clear();
    return true;
}

bool Spi3DisplayHandoff::is_acquired() const noexcept {
    return acquired_;
}

const std::string& Spi3DisplayHandoff::last_error() const noexcept {
    return last_error_;
}

bool Spi3DisplayHandoff::is_configured() const noexcept {
    return operations_.lock_display != nullptr && operations_.drain_display != nullptr &&
           operations_.set_shared_pin_input != nullptr && operations_.set_sd_chip_select_high != nullptr &&
           operations_.unlock_display != nullptr;
}

void Spi3DisplayHandoff::fail_and_unlock(bool leave_sd_deselected) {
    if (leave_sd_deselected) {
        std::string release_error;
        if (!operations_.set_sd_chip_select_high(operations_.context, release_error)) {
            if (!last_error_.empty()) {
                last_error_ += "; ";
            }
            last_error_ += release_error.empty() ? "failed to deselect the SD card" : release_error;
            return;
        }
    }
    operations_.unlock_display(operations_.context);
    acquired_ = false;
}

Spi3DisplayHandoffGuard::Spi3DisplayHandoffGuard(Spi3DisplayHandoff& handoff)
    : handoff_(&handoff) {
    bool release_required = false;
    acquired_ = handoff.acquire(release_required);
    if (!acquired_ && !release_required) {
        handoff_ = nullptr;
    }
}

Spi3DisplayHandoffGuard::~Spi3DisplayHandoffGuard() {
    release();
}

Spi3DisplayHandoffGuard::Spi3DisplayHandoffGuard(Spi3DisplayHandoffGuard&& other) noexcept
    : handoff_(other.handoff_), acquired_(other.acquired_) {
    other.handoff_ = nullptr;
    other.acquired_ = false;
}

Spi3DisplayHandoffGuard& Spi3DisplayHandoffGuard::operator=(Spi3DisplayHandoffGuard&& other) noexcept {
    if (this != &other) {
        if (!release()) {
            return *this;
        }
        handoff_ = other.handoff_;
        acquired_ = other.acquired_;
        other.handoff_ = nullptr;
        other.acquired_ = false;
    }
    return *this;
}

bool Spi3DisplayHandoffGuard::acquired() const noexcept {
    return acquired_;
}

bool Spi3DisplayHandoffGuard::release() noexcept {
    bool result = true;
    if (handoff_ != nullptr && handoff_->is_acquired()) {
        result = handoff_->release();
    }
    if (result) {
        acquired_ = false;
        handoff_ = nullptr;
    }
    return result;
}

Spi3DisplayHandoff& get_spi3_display_handoff() {
    static Spi3DisplayHandoff handoff;
    return handoff;
}

#ifdef ESP_PLATFORM
namespace {

struct EspSpi3DisplayHandoffContext {
    esp_lcd_panel_io_handle_t panel_io = nullptr;
};

void lock_display(void*) {
    GetHAL().lvglLock();
}

bool drain_display(void* raw_context, std::string& error) {
    const auto& context = *static_cast<EspSpi3DisplayHandoffContext*>(raw_context);
    if (context.panel_io == nullptr) {
        error = "LCD panel IO is not initialized";
        return false;
    }
    const esp_err_t result = esp_lcd_panel_io_tx_param(context.panel_io, -1, nullptr, 0);
    if (result != ESP_OK) {
        error = std::string("LCD transaction drain failed: ") + esp_err_to_name(result);
        return false;
    }
    return true;
}

bool set_shared_pin_input(void*, std::string& error) {
    const esp_err_t result = gpio_set_direction(GPIO_NUM_35, GPIO_MODE_INPUT);
    if (result != ESP_OK) {
        error = std::string("GPIO35 input handoff failed: ") + esp_err_to_name(result);
        return false;
    }
    return true;
}

bool set_sd_chip_select_high(void*, std::string& error) {
    esp_err_t result = gpio_set_direction(GPIO_NUM_4, GPIO_MODE_OUTPUT);
    if (result == ESP_OK) {
        result = gpio_set_level(GPIO_NUM_4, 1);
    }
    if (result != ESP_OK) {
        error = std::string("SD chip-select restore failed: ") + esp_err_to_name(result);
        return false;
    }
    return true;
}

void unlock_display(void*) {
    GetHAL().lvglUnlock();
}

}  // namespace

bool initialize_spi3_display_handoff(esp_lcd_panel_io_handle_t panel_io) {
    static EspSpi3DisplayHandoffContext context;
    context.panel_io = panel_io;
    return get_spi3_display_handoff().configure(
        {&context, lock_display, drain_display, set_shared_pin_input, set_sd_chip_select_high, unlock_display});
}
#endif

}  // namespace board
