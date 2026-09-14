#include "app_local_music.h"

#include <assets/assets.h>
#include <hal/hal.h>
#include <media/audio/volume_policy.h>
#include <mooncake_log.h>

using namespace smooth_ui_toolkit::lvgl_cpp;

// Montserrat is intentionally kept for the compact English chrome, but it has
// no CJK glyphs. The Puhui 14px font is already part of the firmware font
// component and covers the UTF-8 filenames returned by FatFs.
LV_FONT_DECLARE(font_puhui_14_1);

namespace {

constexpr uint32_t kBackground = 0xF1F7F5;
constexpr uint32_t kPrimary = 0x113F3A;
constexpr uint32_t kSecondary = 0x3D665F;
constexpr uint32_t kAccent = 0xA9DDD1;

}  // namespace

AppLocalMusic::AppLocalMusic() {
    setAppInfo().name = "LOCAL MUSIC";
    static auto icon = assets::get_image("local_music_icon.png");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = 0x6FC7B3;
    setAppInfo().userData = &theme_color;
}

void AppLocalMusic::onCreate() {
    mclog::tagInfo(getAppInfo().name, "on create");
}

void AppLocalMusic::onOpen() {
    mclog::tagInfo(getAppInfo().name, "on open");
    GetHAL().setSpeakerVolume(media::kMutedVolumePercent, false);

    {
        LvglLockGuard lock;
        create_view();
        render({"SCANNING SD CARD", "BROWSE ONLY - MUTED", {}});
    }

    // browse_tracks() owns the display/SD handoff and therefore runs without an
    // application-level LVGL lock. It also unmounts before returning.
    const auto tracks = sd_card_.browse_tracks();
    const auto view = local_music::make_browse_view(tracks, sd_card_.last_error());

    LvglLockGuard lock;
    render(view);
}

void AppLocalMusic::onRunning() {}

void AppLocalMusic::onClose() {
    mclog::tagInfo(getAppInfo().name, "on close");
    GetHAL().setSpeakerVolume(media::kMutedVolumePercent, false);

    LvglLockGuard lock;
    back_.reset();
    track_rows_.clear();
    track_list_.reset();
    detail_.reset();
    heading_.reset();
    title_.reset();
    panel_.reset();
}

void AppLocalMusic::create_view() {
    panel_ = std::make_unique<Container>(lv_screen_active());
    panel_->setSize(320, 240);
    panel_->align(LV_ALIGN_CENTER, 0, 0);
    panel_->setBgColor(lv_color_hex(kBackground));
    panel_->setBorderWidth(0);
    panel_->setRadius(0);
    panel_->removeFlag(LV_OBJ_FLAG_SCROLLABLE);

    title_ = std::make_unique<Label>(*panel_);
    title_->setText("LOCAL MUSIC");
    title_->setTextFont(&lv_font_montserrat_20);
    title_->setTextColor(lv_color_hex(kPrimary));
    title_->align(LV_ALIGN_TOP_MID, 0, 4);

    heading_ = std::make_unique<Label>(*panel_);
    heading_->setTextFont(&font_puhui_14_1);
    heading_->setTextColor(lv_color_hex(kPrimary));
    heading_->setWidth(292);
    heading_->setTextAlign(LV_TEXT_ALIGN_CENTER);
    heading_->align(LV_ALIGN_TOP_MID, 0, 28);

    detail_ = std::make_unique<Label>(*panel_);
    detail_->setTextFont(&font_puhui_14_1);
    detail_->setTextColor(lv_color_hex(kSecondary));
    detail_->setWidth(286);
    detail_->setLongMode(LV_LABEL_LONG_SCROLL_CIRCULAR);
    detail_->setTextAlign(LV_TEXT_ALIGN_CENTER);
    detail_->align(LV_ALIGN_TOP_MID, 0, 48);

    track_list_ = std::make_unique<Container>(*panel_);
    track_list_->setSize(292, 122);
    track_list_->align(LV_ALIGN_TOP_MID, 0, 66);
    track_list_->setBgOpa(LV_OPA_TRANSP);
    track_list_->setBorderWidth(0);
    track_list_->setRadius(0);
    track_list_->setPadding(2, 2, 0, 0);
    track_list_->setFlexFlow(LV_FLEX_FLOW_COLUMN);
    track_list_->setFlexAlign(LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    track_list_->setPadRow(4);
    track_list_->setScrollDir(LV_DIR_VER);
    track_list_->setScrollbarMode(LV_SCROLLBAR_MODE_ACTIVE);

    back_ = std::make_unique<Button>(*panel_);
    back_->setSize(104, 34);
    back_->align(LV_ALIGN_BOTTOM_MID, 0, -6);
    back_->setBgColor(lv_color_hex(kAccent));
    back_->setBorderWidth(0);
    back_->setShadowWidth(0);
    back_->setRadius(12);
    back_->label().setText("BACK");
    back_->label().setTextFont(&lv_font_montserrat_16);
    back_->label().setTextColor(lv_color_hex(kPrimary));
    back_->onClick().connect([this]() { close(); });
}

void AppLocalMusic::render(const local_music::BrowseView& view) {
    heading_->setText(view.heading);
    detail_->setText(view.detail);

    track_rows_.clear();
    for (std::size_t index = 0; index < view.rows.size(); ++index) {
        auto row = std::make_unique<Label>(*track_list_);
        row->setText(std::to_string(index + 1) + ".  " + view.rows[index]);
        row->setTextFont(&font_puhui_14_1);
        row->setTextColor(lv_color_hex(kPrimary));
        row->setWidth(276);
        row->setHeight(18);
        row->setLongMode(LV_LABEL_LONG_CLIP);
        track_rows_.push_back(std::move(row));
    }
}
