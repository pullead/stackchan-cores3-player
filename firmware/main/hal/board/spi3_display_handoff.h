#pragma once

#include <string>

#ifdef ESP_PLATFORM
#include "esp_lcd_panel_io.h"
#endif

namespace board {

struct Spi3DisplayHandoffOperations {
    void* context = nullptr;
    void (*lock_display)(void* context) = nullptr;
    bool (*drain_display)(void* context, std::string& error) = nullptr;
    bool (*set_shared_pin_input)(void* context, std::string& error) = nullptr;
    bool (*set_sd_chip_select_high)(void* context, std::string& error) = nullptr;
    void (*unlock_display)(void* context) = nullptr;
};

class Spi3DisplayHandoff {
public:
    Spi3DisplayHandoff() = default;
    explicit Spi3DisplayHandoff(Spi3DisplayHandoffOperations operations) noexcept;

    Spi3DisplayHandoff(const Spi3DisplayHandoff&) = delete;
    Spi3DisplayHandoff& operator=(const Spi3DisplayHandoff&) = delete;

    bool configure(Spi3DisplayHandoffOperations operations) noexcept;
    bool acquire();
    bool release() noexcept;
    bool is_acquired() const noexcept;
    const std::string& last_error() const noexcept;

private:
    bool is_configured() const noexcept;
    void fail_and_unlock(bool leave_sd_deselected) noexcept;

    Spi3DisplayHandoffOperations operations_;
    bool acquired_ = false;
    std::string last_error_;
};

class Spi3DisplayHandoffGuard {
public:
    explicit Spi3DisplayHandoffGuard(Spi3DisplayHandoff& handoff);
    ~Spi3DisplayHandoffGuard();

    Spi3DisplayHandoffGuard(const Spi3DisplayHandoffGuard&) = delete;
    Spi3DisplayHandoffGuard& operator=(const Spi3DisplayHandoffGuard&) = delete;
    Spi3DisplayHandoffGuard(Spi3DisplayHandoffGuard&& other) noexcept;
    Spi3DisplayHandoffGuard& operator=(Spi3DisplayHandoffGuard&& other) noexcept;

    bool acquired() const noexcept;
    bool release() noexcept;

private:
    Spi3DisplayHandoff* handoff_ = nullptr;
    bool acquired_ = false;
};

Spi3DisplayHandoff& get_spi3_display_handoff();

#ifdef ESP_PLATFORM
bool initialize_spi3_display_handoff(esp_lcd_panel_io_handle_t panel_io);
#endif

}  // namespace board
