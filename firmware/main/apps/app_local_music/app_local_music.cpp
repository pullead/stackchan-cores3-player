#include "app_local_music.h"

#include "hifi_theme.h"

#include <assets/assets.h>
#include <audio/audio_codec.h>
#include <board.h>
#include <hal/board/cores3_audio_codec.h>
#include <hal/hal.h>
#include <media/audio/volume_policy.h>
#include <media/decoder/hifi_decoder_adapter.h>
#include <mooncake_log.h>

#include <cstdio>

using namespace smooth_ui_toolkit::lvgl_cpp;

// The Puhui font ships with this firmware and covers the UTF-8 filenames FatFs
// returns.  Upstream uses a 13px Noto Sans SC subset, which is an LVGL 8 font
// binary and cannot be loaded here; 14px is the closest available match.
LV_FONT_DECLARE(font_puhui_14_1);

namespace {

constexpr const char* kTabLabels[hifi_theme::kTabCount] = {"歌曲", "歌手", "专辑", "今日", "★"};

const lv_font_t* body_font() { return &font_puhui_14_1; }

lv_obj_t* make_text(lv_obj_t* parent, const char* text, lv_color_t colour, lv_align_t align,
                    int32_t x, int32_t y) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, body_font(), 0);
    lv_obj_set_style_text_color(label, colour, 0);
    lv_obj_align(label, align, x, y);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

// Chips, rows and buttons all share this flat, borderless treatment.
void style_flat(lv_obj_t* object, lv_color_t colour, int32_t radius) {
    lv_obj_set_style_radius(object, radius, 0);
    lv_obj_set_style_bg_color(object, colour, 0);
    lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(object, 0, 0);
    lv_obj_set_style_shadow_width(object, 0, 0);
    lv_obj_set_style_pad_all(object, 0, 0);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

}  // namespace

AppLocalMusic::AppLocalMusic() {
    setAppInfo().name = "LOCAL MUSIC";
    static auto icon = assets::get_image("local_music_icon.png");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = 0x6FC7B3;
    setAppInfo().userData = &theme_color;
}

void AppLocalMusic::onCreate() { mclog::tagInfo(getAppInfo().name, "on create"); }

void AppLocalMusic::onOpen() {
    mclog::tagInfo(getAppInfo().name, "on open");
    GetHAL().setSpeakerVolume(media::kMutedVolumePercent, false);

    auto& board = Board::GetInstance();
    auto* codec = board.GetAudioCodec();
    if (codec != nullptr) {
        codec_port_ = std::make_unique<media::BoardAudioCodecPort>(*codec);
        // Only the CoreS3 codec can be retimed away from the AI path's 24 kHz
        // mono channel.  Without it the sink still works, but music would be
        // limited to whatever rate the AI path is running at.
        if (auto* cores3 = dynamic_cast<CoreS3AudioCodec*>(codec); cores3 != nullptr) {
            session_port_ = std::make_unique<media::BoardAudioSessionPort>(*cores3);
            audio_session_ = std::make_unique<media::MediaAudioSession>(*session_port_);
        } else {
            mclog::tagWarn(getAppInfo().name, "codec cannot be retimed for media playback");
        }
        speaker_sink_ =
            std::make_unique<media::CoreS3SpeakerSink>(*codec_port_, audio_session_.get());
        playback_ = std::make_unique<media::LocalPlaybackController>(*speaker_sink_);
        pump_task_ = std::make_unique<media::PlaybackPumpTask>(*playback_);
        if (!pump_task_->start()) {
            mclog::tagError(getAppInfo().name, "media pump task unavailable");
            pump_task_.reset();
        }
    } else {
        mclog::tagError(getAppInfo().name, "audio codec unavailable");
    }

    {
        LvglLockGuard lock;
        build_list_page();
    }

    // browse_tracks() owns the display/SD handoff and therefore runs without an
    // application-level LVGL lock.  It also unmounts before returning.
    tracks_ = sd_card_.browse_tracks();

    LvglLockGuard lock;
    build_list_page();
}

void AppLocalMusic::onRunning() {
    // Runs outside LVGL event dispatch, so rebuilding the page is safe here.
    apply_pending_action();
    refresh_player_page();
}

void AppLocalMusic::onClose() {
    mclog::tagInfo(getAppInfo().name, "on close");
    // Stop the pump before the controller so no decode is in flight while the
    // stream, sink and audio session are torn down.
    if (pump_task_) {
        pump_task_->stop();
    }
    if (playback_) {
        playback_->stop();
    }
    GetHAL().setSpeakerVolume(media::kMutedVolumePercent, false);

    {
        LvglLockGuard lock;
        destroy_page();
    }
    tracks_.clear();
    player_page_ = false;
    pending_action_ = PendingAction::None;
    selected_title_.clear();
    shown_status_.clear();
    shown_elapsed_.clear();
    pump_task_.reset();
    playback_.reset();
    speaker_sink_.reset();
    // Release the audio channel last: the sink hands it back on close, and this
    // only tears down the objects afterwards.
    audio_session_.reset();
    session_port_.reset();
    codec_port_.reset();
}

void AppLocalMusic::destroy_page() {
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    list_ = nullptr;
    scroll_slider_ = nullptr;
    status_label_ = nullptr;
    player_title_ = nullptr;
    player_elapsed_ = nullptr;
    player_state_ = nullptr;
}

void AppLocalMusic::build_list_page() {
    destroy_page();
    player_page_ = false;

    root_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(root_, hifi_theme::kScreenWidth, hifi_theme::kScreenHeight);
    lv_obj_align(root_, LV_ALIGN_CENTER, 0, 0);
    style_flat(root_, hifi_theme::bg(), 0);

    // No status bar on this page: upstream dropped it because the list is
    // scrolled constantly and the ~20px matters more than a persistent
    // clock/WiFi readout.
    for (int32_t i = 0; i < hifi_theme::kTabCount; ++i) {
        const bool selected = static_cast<int32_t>(tab_) == i;
        lv_obj_t* tab = lv_button_create(root_);
        lv_obj_set_pos(tab, hifi_theme::kTabX + i * hifi_theme::kTabPitch, hifi_theme::kTabY);
        lv_obj_set_size(tab, hifi_theme::kTabWidth, hifi_theme::kTabHeight);
        style_flat(tab, selected ? hifi_theme::accent_deep() : hifi_theme::panel(),
                   hifi_theme::kTabRadius);
        lv_obj_set_user_data(tab, this);
        lv_obj_add_event_cb(tab, on_tab_clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(i)));
        make_text(tab, kTabLabels[i], selected ? hifi_theme::ink() : hifi_theme::ink_dim(),
                  LV_ALIGN_CENTER, 0, 0);
    }

    char header[32] = {};
    std::snprintf(header, sizeof(header), "共 %u 首", static_cast<unsigned>(tracks_.size()));
    status_label_ = make_text(root_, header, hifi_theme::ink_dim(), LV_ALIGN_TOP_RIGHT, -10, 6);

    list_ = lv_obj_create(root_);
    lv_obj_set_pos(list_, hifi_theme::kListX, hifi_theme::kListY);
    lv_obj_set_size(list_, hifi_theme::kListWidth, hifi_theme::kListHeight);
    lv_obj_set_style_bg_opa(list_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_, 0, 0);
    lv_obj_set_style_pad_all(list_, 0, 0);
    lv_obj_set_style_radius(list_, 0, 0);
    lv_obj_add_flag(list_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list_, LV_DIR_VER);
    // Replaced by the draggable slider below: LVGL's own scrollbar is an
    // indicator only, not something a finger can grab.
    lv_obj_set_scrollbar_mode(list_, LV_SCROLLBAR_MODE_OFF);

    if (tab_ != Tab::Songs) {
        // Artists/Albums/Today/Favourites need ID3 tags, import timestamps and
        // favourites, none of which the scanner collects yet.  Say so instead
        // of showing an empty list that looks like a failure.
        make_text(list_, "该分类需要曲库信息", hifi_theme::ink_dim(), LV_ALIGN_CENTER, 0, 0);
        return;
    }
    if (tracks_.empty()) {
        make_text(list_, "未找到音乐文件", hifi_theme::ink_dim(), LV_ALIGN_CENTER, 0, 0);
        return;
    }

    int32_t row_y = 0;
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        lv_obj_t* row = lv_button_create(list_);
        lv_obj_set_pos(row, 0, row_y);
        lv_obj_set_size(row, hifi_theme::kRowWidth, hifi_theme::kRowHeight);
        style_flat(row, hifi_theme::panel(), hifi_theme::kRowRadius);
        lv_obj_set_user_data(row, this);
        lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(i)));

        // Single line: title on the left, artist on the right and dimmer.  Not
        // joined into "title · artist" -- in CJK that separator smears into the
        // glyphs, and two labels read more clearly for the same glyph count.
        const char* title = tracks_[i].title.empty() ? "未知曲目" : tracks_[i].title.c_str();
        lv_obj_t* title_label = make_text(row, title, hifi_theme::ink(), LV_ALIGN_LEFT_MID, 10, 0);
        lv_label_set_long_mode(title_label, LV_LABEL_LONG_DOT);
        // LV_LABEL_LONG_DOT wraps by width and only ellipsises once the text
        // exceeds the label's HEIGHT, so the height has to be pinned to one
        // line or long titles silently become two cramped rows.
        lv_obj_set_size(title_label, 170, lv_font_get_line_height(body_font()));

        // Artist metadata is not read yet; the slot is kept so the layout does
        // not shift once ID3 parsing lands.
        lv_obj_t* detail =
            make_text(row, "未知艺术家", hifi_theme::ink_faint(), LV_ALIGN_RIGHT_MID, -24, 0);
        lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
        lv_obj_set_size(detail, 76, lv_font_get_line_height(body_font()));
        lv_obj_set_style_text_align(detail, LV_TEXT_ALIGN_RIGHT, 0);

        row_y += hifi_theme::kRowPitch;
    }

    if (row_y > hifi_theme::kListHeight) {
        scroll_slider_ = lv_slider_create(root_);
        lv_obj_set_pos(scroll_slider_, hifi_theme::kScrollSliderX, hifi_theme::kListY);
        lv_obj_set_size(scroll_slider_, hifi_theme::kScrollSliderWidth, hifi_theme::kListHeight);
        lv_slider_set_range(scroll_slider_, 0, row_y - hifi_theme::kListHeight);
        lv_obj_set_style_bg_color(scroll_slider_, hifi_theme::panel_deep(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(scroll_slider_, hifi_theme::accent_deep(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(scroll_slider_, hifi_theme::accent_bright(), LV_PART_KNOB);
        lv_obj_set_user_data(scroll_slider_, this);
        lv_obj_add_event_cb(scroll_slider_, on_scroll_slider, LV_EVENT_VALUE_CHANGED, nullptr);
    }
}

void AppLocalMusic::build_player_page() {
    destroy_page();
    player_page_ = true;

    root_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(root_, hifi_theme::kScreenWidth, hifi_theme::kScreenHeight);
    lv_obj_align(root_, LV_ALIGN_CENTER, 0, 0);
    style_flat(root_, hifi_theme::bg(), 0);

    make_text(root_, "正在播放", hifi_theme::ink_dim(), LV_ALIGN_TOP_MID, 0, 8);

    player_title_ =
        make_text(root_, selected_title_.c_str(), hifi_theme::ink(), LV_ALIGN_TOP_MID, 0, 34);
    lv_label_set_long_mode(player_title_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(player_title_, 286);
    lv_obj_set_style_text_align(player_title_, LV_TEXT_ALIGN_CENTER, 0);

    player_elapsed_ = make_text(root_, "--:--", hifi_theme::ink_dim(), LV_ALIGN_TOP_MID, 0, 70);
    player_state_ = make_text(root_, "", hifi_theme::accent_bright(), LV_ALIGN_TOP_MID, 0, 96);

    lv_obj_t* back = lv_button_create(root_);
    lv_obj_set_size(back, 120, 34);
    lv_obj_align(back, LV_ALIGN_BOTTOM_MID, 0, -10);
    style_flat(back, hifi_theme::panel(), 12);
    lv_obj_set_user_data(back, this);
    lv_obj_add_event_cb(back, on_back_clicked, LV_EVENT_CLICKED, nullptr);
    make_text(back, "返回列表", hifi_theme::ink(), LV_ALIGN_CENTER, 0, 0);
}

void AppLocalMusic::on_tab_clicked(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    const auto index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    self->pending_tab_ = static_cast<Tab>(index);
    self->pending_action_ = PendingAction::SwitchTab;
}

void AppLocalMusic::on_row_clicked(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    self->pending_index_ = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    self->pending_action_ = PendingAction::SelectTrack;
}

void AppLocalMusic::on_back_clicked(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    self->pending_action_ = PendingAction::BackToList;
}

void AppLocalMusic::on_scroll_slider(lv_event_t* event) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(slider));
    if (self == nullptr || self->list_ == nullptr) return;
    // The slider grows upwards from the bottom while scroll_y grows downwards
    // from the top, so the value is inverted rather than mapped one to one.
    const int32_t range = lv_slider_get_max_value(slider);
    const int32_t value = lv_slider_get_value(slider);
    lv_obj_scroll_to_y(self->list_, range - value, LV_ANIM_OFF);
}

void AppLocalMusic::apply_pending_action() {
    const PendingAction action = pending_action_;
    if (action == PendingAction::None) {
        return;
    }
    pending_action_ = PendingAction::None;

    if (action == PendingAction::BackToList) {
        // Stop outside the LVGL lock: the pump task may be waiting for that
        // same lock to borrow the SD bus, and holding it here would deadlock.
        if (playback_) {
            playback_->stop();
        }
        LvglLockGuard lock;
        build_list_page();
        return;
    }
    if (action == PendingAction::SwitchTab) {
        tab_ = pending_tab_;
        LvglLockGuard lock;
        build_list_page();
        return;
    }
    select_track(pending_index_);
}

void AppLocalMusic::select_track(std::size_t index) {
    const auto selected = local_music::select_track(tracks_, index);
    if (!selected.accepted || index >= tracks_.size()) {
        return;
    }

    selected_title_ = tracks_[index].title;
    shown_status_.clear();
    shown_elapsed_.clear();
    {
        LvglLockGuard lock;
        build_player_page();
    }

    // Opening the stream borrows the display bus, so it must not be done while
    // an application-level LVGL lock is held.
    auto stream = sd_card_.open_track(tracks_[index]);
    auto* backend = media::create_hifi_decoder_backend();
    if (!stream || backend == nullptr || !playback_) {
        LvglLockGuard lock;
        if (player_state_ != nullptr) lv_label_set_text(player_state_, "解码器不可用");
        return;
    }

    auto decoder = std::make_unique<media::HifiDecoderAdapter>(*backend);
    playback_->select(selected_title_, std::move(stream), std::move(decoder));
    playback_->start();
}

void AppLocalMusic::refresh_player_page() {
    if (!playback_ || !player_page_ || player_state_ == nullptr) {
        return;
    }

    // tick() is lock-free: taking the controller mutex here would mean waiting
    // for the audio task to finish a whole chunk, so the clock would advance in
    // multi-second jumps instead of smoothly.
    const auto tick = playback_->tick();
    const std::string status = playback_status(tick.state);

    std::string elapsed = "--:--";
    if (tick.sample_rate > 0) {
        const uint32_t seconds = tick.played_frames / tick.sample_rate;
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "%u:%02u  @%uHz", static_cast<unsigned>(seconds / 60),
                      static_cast<unsigned>(seconds % 60), static_cast<unsigned>(tick.sample_rate));
        elapsed = buffer;
    }

    if (status == shown_status_ && elapsed == shown_elapsed_) {
        return;
    }
    shown_status_ = status;
    shown_elapsed_ = elapsed;

    LvglLockGuard lock;
    lv_label_set_text(player_state_, shown_status_.c_str());
    if (player_elapsed_ != nullptr) {
        lv_label_set_text(player_elapsed_, shown_elapsed_.c_str());
    }
}

std::string AppLocalMusic::playback_status() const {
    return playback_ ? playback_status(playback_->tick().state) : "音频不可用";
}

std::string AppLocalMusic::playback_status(media::PlaybackState state) const {
    if (!playback_) {
        return "音频不可用";
    }

    // Only the error path pays for the lock: it is rare and needs the text.
    if (state == media::PlaybackState::Error) {
        const auto snapshot = playback_->snapshot();
        if (!snapshot.error.empty()) {
            return snapshot.error;
        }
    }

    switch (state) {
        case media::PlaybackState::Preparing:
            return "准备中";
        case media::PlaybackState::Buffering:
            return "缓冲中";
        case media::PlaybackState::Playing:
            return "播放中（静音）";
        case media::PlaybackState::Paused:
            return "已暂停";
        case media::PlaybackState::PreparingForAi:
            return "交还语音";
        case media::PlaybackState::Stopping:
            return "停止中";
        case media::PlaybackState::Error:
            return "播放错误";
        case media::PlaybackState::Idle:
        default:
            return "就绪";
    }
}
