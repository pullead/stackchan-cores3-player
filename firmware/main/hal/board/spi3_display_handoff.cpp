#include "hal/board/spi3_display_handoff.h"

#include <utility>

#ifdef ESP_PLATFORM
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "hal/board/hal_bridge.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/soc.h"
#endif

namespace board {

Spi3DisplayHandoff::Spi3DisplayHandoff(Spi3DisplayHandoffOperations operations) noexcept
    : operations_(operations) {}

bool Spi3DisplayHandoff::configure(Spi3DisplayHandoffOperations operations) noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (acquired_) {
        last_error_ = "cannot configure SPI3 handoff while acquired";
        return false;
    }
    operations_ = operations;
    ++configuration_generation_;
    last_error_.clear();
    return true;
}

bool Spi3DisplayHandoff::acquire() {
    bool release_required = false;
    return acquire(release_required);
}

bool Spi3DisplayHandoff::acquire(bool& release_required) {
    release_required = false;
    Spi3DisplayHandoffOperations operations;
    std::uint64_t configuration_generation = 0;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        if (!is_configured_locked()) {
            last_error_ = "SPI3 display handoff is not initialized";
            return false;
        }
        operations = operations_;
        configuration_generation = configuration_generation_;
    }

    std::string lock_error;
    if (!operations.lock_display(operations.context, lock_error)) {
        std::lock_guard<std::mutex> lock(state_mutex_);
        last_error_ = lock_error.empty() ? "failed to lock the display" : std::move(lock_error);
        return false;
    }

    std::lock_guard<std::mutex> lock(state_mutex_);
    if (configuration_generation != configuration_generation_) {
        last_error_ = "SPI3 display handoff configuration changed while locking";
        operations.unlock_display(operations.context);
        return false;
    }
    if (acquired_) {
        last_error_ = "SPI3 display handoff is already acquired";
        operations.unlock_display(operations.context);
        return false;
    }

    last_error_.clear();
    acquired_ = true;

    if (!operations.drain_display(operations.context, last_error_)) {
        if (last_error_.empty()) {
            last_error_ = "failed to drain display transactions";
        }
        fail_and_unlock_locked(false, operations);
        return false;
    }
    if (!operations.set_shared_pin_input(operations.context, last_error_)) {
        if (last_error_.empty()) {
            last_error_ = "failed to release the shared display pin";
        }
        fail_and_unlock_locked(true, operations);
        release_required = acquired_;
        return false;
    }

    return true;
}

bool Spi3DisplayHandoff::release() noexcept {
    std::lock_guard<std::mutex> lock(state_mutex_);
    if (!acquired_) {
        return true;
    }

    std::string release_error;
    const bool cs_high = operations_.set_sd_chip_select_high(operations_.context, release_error);
    if (!cs_high) {
        last_error_ = release_error.empty() ? "failed to deselect the SD card" : std::move(release_error);
        return false;
    }

    if (!operations_.restore_shared_pin_display_output(operations_.context, release_error)) {
        last_error_ = release_error.empty() ? "failed to restore the shared display pin" : std::move(release_error);
        return false;
    }

    operations_.unlock_display(operations_.context);
    acquired_ = false;
    last_error_.clear();
    return true;
}

bool Spi3DisplayHandoff::is_acquired() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return acquired_;
}

std::string Spi3DisplayHandoff::last_error() const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    return last_error_;
}

bool Spi3DisplayHandoff::is_configured_locked() const noexcept {
    return operations_.lock_display != nullptr && operations_.drain_display != nullptr &&
           operations_.set_shared_pin_input != nullptr && operations_.set_sd_chip_select_high != nullptr &&
           operations_.restore_shared_pin_display_output != nullptr && operations_.unlock_display != nullptr;
}

void Spi3DisplayHandoff::fail_and_unlock_locked(bool leave_sd_deselected,
                                                const Spi3DisplayHandoffOperations& operations) {
    if (leave_sd_deselected) {
        std::string release_error;
        if (!operations.set_sd_chip_select_high(operations.context, release_error)) {
            if (!last_error_.empty()) {
                last_error_ += "; ";
            }
            last_error_ += release_error.empty() ? "failed to deselect the SD card" : release_error;
            return;
        }
        if (!operations.restore_shared_pin_display_output(operations.context, release_error)) {
            if (!last_error_.empty()) {
                last_error_ += "; ";
            }
            last_error_ += release_error.empty() ? "failed to restore the shared display pin" : release_error;
            return;
        }
    }
    operations.unlock_display(operations.context);
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
    if (handoff_ != nullptr) {
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

constexpr char kTag[] = "Spi3DisplayHandoff";
constexpr int kDisplayLockTimeoutMs = 30000;

struct EspSpi3DisplayHandoffContext {
    esp_lcd_panel_io_handle_t panel_io = nullptr;
};

bool lock_display(void*, std::string& error) {
    if (!hal_bridge::try_display_lvgl_lock(kDisplayLockTimeoutMs)) {
        error = "LVGL display lock timed out after 30000 ms";
        ESP_LOGE(kTag, "%s", error.c_str());
        return false;
    }
    return true;
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
    // CoreS3 shares GPIO35 between LCD D/C and SPI3 MISO.  Direction alone is
    // insufficient: the GPIO output matrix must be returned to SPI3 Q while SD
    // traffic owns the bus, then the output driver must be disabled. Rebind the
    // input matrix too: panel setup can leave GPIO35 routed only as LCD D/C.
    // This board uses SPI3_HOST, whose MISO signal is SPI3_Q (not FSPIQ).
    esp_rom_gpio_connect_in_signal(GPIO_NUM_35, SPI3_Q_IN_IDX, false);
    REG_WRITE(GPIO_FUNC35_OUT_SEL_CFG_REG, SPI3_Q_OUT_IDX);
    REG_WRITE(GPIO_ENABLE1_W1TC_REG, 1u << (GPIO_NUM_35 & 31));
    const esp_err_t result = gpio_set_direction(GPIO_NUM_35, GPIO_MODE_INPUT);
    if (result != ESP_OK) {
        error = std::string("GPIO35 SD MISO handoff failed: ") + esp_err_to_name(result);
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
        ESP_LOGE(kTag, "%s; display remains locked to prevent SPI3 contention", error.c_str());
        return false;
    }
    return true;
}

bool restore_shared_pin_display_output(void*, std::string& error) {
    // Restore the screen's D/C GPIO route before the LVGL lock is released.
    REG_WRITE(GPIO_FUNC35_OUT_SEL_CFG_REG, SIG_GPIO_OUT_IDX);
    REG_WRITE(GPIO_ENABLE1_W1TS_REG, 1u << (GPIO_NUM_35 & 31));
    const esp_err_t result = gpio_set_direction(GPIO_NUM_35, GPIO_MODE_OUTPUT);
    if (result != ESP_OK) {
        error = std::string("GPIO35 display D/C restore failed: ") + esp_err_to_name(result);
        ESP_LOGE(kTag, "%s; display remains locked to prevent SPI3 contention", error.c_str());
        return false;
    }
    return true;
}

void unlock_display(void*) {
    hal_bridge::disply_lvgl_unlock();
}

}  // namespace

bool initialize_spi3_display_handoff(esp_lcd_panel_io_handle_t panel_io) {
    static EspSpi3DisplayHandoffContext context;
    context.panel_io = panel_io;
    return get_spi3_display_handoff().configure(
        {&context, lock_display, drain_display, set_shared_pin_input, set_sd_chip_select_high,
         restore_shared_pin_display_output, unlock_display});
}
#endif

}  // namespace board
