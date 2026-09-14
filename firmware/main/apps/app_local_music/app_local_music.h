#pragma once

#include "local_music_presenter.h"

#include <memory>
#include <vector>

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
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> track_list_;
    std::vector<std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button>> track_rows_;
    std::vector<media::SdTrack> tracks_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> back_;
};
