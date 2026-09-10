#pragma once

#include "local_music_presenter.h"

#include <array>
#include <memory>

#include <mooncake.h>
#include <smooth_lvgl.hpp>

class AppLocalMusic : public mooncake::AppAbility {
public:
    AppLocalMusic();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    void create_view();
    void render(const local_music::BrowseView& view);

    media::SdCardPort sd_card_;

    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> panel_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> title_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> heading_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> detail_;
    std::array<std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label>, local_music::kVisibleTrackRows> track_rows_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> back_;
};
