#include "app_local_music.h"

#include <assets/assets.h>
#include <audio/audio_codec.h>
#include <board.h>
#include <hal/hal.h>
#include <media/audio/volume_policy.h>
#include <media/decoder/hifi_decoder_adapter.h>
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

    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();
    if (codec != nullptr) {
        codec_port_ = std::make_unique<media::BoardAudioCodecPort>(*codec);
        speaker_sink_ = std::make_unique<media::CoreS3SpeakerSink>(*codec_port_);
        playback_ = std::make_unique<media::LocalPlaybackController>(*speaker_sink_);
    } else {
        mclog::tagError(getAppInfo().name, "audio codec unavailable");
    }

    {
        LvglLockGuard lock;
        create_view();
        render({"SCANNING SD CARD", "BROWSE ONLY - MUTED", {}});
    }

    // browse_tracks() owns the display/SD handoff and therefore runs without an
    // application-level LVGL lock. It also unmounts before returning.
    tracks_ = sd_card_.browse_tracks();
    const auto view = local_music::make_browse_view(tracks_, sd_card_.last_error());

    LvglLockGuard lock;
    render(view);
}

void AppLocalMusic::onRunning() {
    if (!playback_) {
        return;
    }

    playback_->pump();
    if (playback_view_ && detail_) {
        LvglLockGuard lock;
        detail_->setText(playback_status());
    }
}

void AppLocalMusic::onClose() {
    mclog::tagInfo(getAppInfo().name, "on close");
    if (playback_) {
        playback_->stop();
    }
    GetHAL().setSpeakerVolume(media::kMutedVolumePercent, false);

    LvglLockGuard lock;
    back_.reset();
    playback_note_.reset();
    playback_progress_.reset();
    playback_name_.reset();
    track_rows_.clear();
    track_list_.reset();
    tracks_.clear();
    detail_.reset();
    heading_.reset();
    title_.reset();
    panel_.reset();
    playback_view_ = false;
    selected_title_.clear();
    playback_.reset();
    speaker_sink_.reset();
    codec_port_.reset();
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
    playback_view_ = false;
    heading_->setText(view.heading);
    detail_->setText(view.detail);

    track_rows_.clear();
    for (std::size_t index = 0; index < view.rows.size(); ++index) {
        auto row = std::make_unique<Button>(*track_list_);
        row->label().setText(std::to_string(index + 1) + ".  " + view.rows[index]);
        row->label().setTextFont(&font_puhui_14_1);
        row->label().setTextColor(lv_color_hex(kPrimary));
        row->setBgColor(lv_color_hex(kBackground));
        row->setBorderWidth(0);
        row->setShadowWidth(0);
        row->setRadius(4);
        row->setWidth(276);
        row->setHeight(22);
        row->onClick().connect([this, index]() {
            const auto selected = local_music::select_track(tracks_, index);
            if (!selected.accepted || index >= tracks_.size()) {
                return;
            }

            selected_title_ = tracks_[index].title;
            // Always enter the player page so a selected track has a clear
            // destination.  The page reports backend availability honestly;
            // it must never show a fake Playing state.
            render_playback(selected_title_, "PREPARING / MUTED");

            // The SD stream is opened read-only.  Do not claim playback when
            // the optional compressed-audio backend is absent.
            auto stream = sd_card_.open_track(tracks_[index]);
            auto* backend = media::create_hifi_decoder_backend();
            if (!stream || backend == nullptr || !playback_) {
                render_playback(selected_title_, "DECODER UNAVAILABLE / MUTED");
                return;
            }

            auto decoder = std::make_unique<media::HifiDecoderAdapter>(*backend);
            playback_->select(selected_title_, std::move(stream), std::move(decoder));
            if (!playback_->start()) {
                render_playback(selected_title_, playback_status());
                return;
            }

            render_playback(selected_title_, playback_status());
        });
        track_rows_.push_back(std::move(row));
    }
}

void AppLocalMusic::render_playback(const std::string& title, const std::string& status) {
    playback_view_ = true;
    playback_note_.reset();
    playback_progress_.reset();
    playback_name_.reset();
    track_rows_.clear();
    track_list_.reset();
    heading_->setText("NOW PLAYING");
    detail_->setText(status);

    playback_name_ = std::make_unique<Label>(*panel_);
    playback_name_->setText(title);
    playback_name_->setTextFont(&font_puhui_14_1);
    playback_name_->setTextColor(lv_color_hex(kPrimary));
    playback_name_->setWidth(286);
    playback_name_->setLongMode(LV_LABEL_LONG_SCROLL_CIRCULAR);
    playback_name_->setTextAlign(LV_TEXT_ALIGN_CENTER);
    playback_name_->align(LV_ALIGN_TOP_MID, 0, 78);

    playback_progress_ = std::make_unique<Label>(*panel_);
    playback_progress_->setText("--:-- / --:--");
    playback_progress_->setTextFont(&lv_font_montserrat_16);
    playback_progress_->setTextColor(lv_color_hex(kSecondary));
    playback_progress_->align(LV_ALIGN_TOP_MID, 0, 112);

    playback_note_ = std::make_unique<Label>(*panel_);
    playback_note_->setText("COVER  /  LYRICS  /  SPECTRUM PENDING");
    playback_note_->setTextFont(&lv_font_montserrat_12);
    playback_note_->setTextColor(lv_color_hex(kSecondary));
    playback_note_->align(LV_ALIGN_TOP_MID, 0, 142);

    auto back = std::make_unique<Button>(*panel_);
    back->setSize(120, 34);
    back->align(LV_ALIGN_BOTTOM_MID, 0, -6);
    back->setBgColor(lv_color_hex(kAccent));
    back->setBorderWidth(0);
    back->setShadowWidth(0);
    back->setRadius(12);
    back->label().setText("BACK TO LIST");
    back->label().setTextFont(&lv_font_montserrat_12);
    back->label().setTextColor(lv_color_hex(kPrimary));
    back->onClick().connect([this]() { show_list(); });

    back_ = std::move(back);
}

void AppLocalMusic::show_list() {
    if (!panel_) return;
    if (playback_) {
        playback_->stop();
    }
    back_.reset();
    playback_note_.reset();
    playback_progress_.reset();
    playback_name_.reset();
    // Recreate the list container and buttons using the already scanned tracks.
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
    back_->label().setText("BACK");
    back_->onClick().connect([this]() { close(); });
    render(local_music::make_browse_view(tracks_, sd_card_.last_error()));
}

std::string AppLocalMusic::playback_status() const {
    if (!playback_) {
        return "AUDIO UNAVAILABLE / MUTED";
    }

    const auto snapshot = playback_->snapshot();
    if (!snapshot.error.empty()) {
        return snapshot.error + " / MUTED";
    }

    switch (snapshot.state) {
        case media::PlaybackState::Preparing:
            return "PREPARING / MUTED";
        case media::PlaybackState::Buffering:
            return "BUFFERING / MUTED";
        case media::PlaybackState::Playing:
            return "PLAYING / MUTED";
        case media::PlaybackState::Paused:
            return "PAUSED / MUTED";
        case media::PlaybackState::PreparingForAi:
            return "AI HANDOFF / MUTED";
        case media::PlaybackState::Stopping:
            return "STOPPING / MUTED";
        case media::PlaybackState::Error:
            return "PLAYBACK ERROR / MUTED";
        case media::PlaybackState::Idle:
        default:
            return "IDLE / MUTED";
    }
}
