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

#include <algorithm>
#include <cstdio>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

using namespace smooth_ui_toolkit::lvgl_cpp;

// The Puhui font ships with this firmware and covers the UTF-8 filenames FatFs
// returns.  Upstream uses a 13px Noto Sans SC subset, which is an LVGL 8 font
// binary and cannot be loaded here; 14px is the closest available match.
LV_FONT_DECLARE(font_puhui_14_1);

namespace {

constexpr const char* kTabLabels[hifi_theme::kTabCount] = {"歌曲", "歌手", "专辑", "今日", "★"};

// A back swipe has to start within this many pixels of the left edge; further
// in, the gesture belongs to whatever is under the finger.
constexpr int32_t kEdgeGestureWidth = 40;

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
    // The canvas buffer is not owned by the widget tree; deleting the page
    // would leave the canvas pointing at freed memory otherwise.
    if (spectrum_buffer_ != nullptr) {
#ifdef ESP_PLATFORM
        heap_caps_free(spectrum_buffer_);
#else
        lv_free(spectrum_buffer_);
#endif
        spectrum_buffer_ = nullptr;
    }
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    list_ = nullptr;
    scroll_slider_ = nullptr;
    status_label_ = nullptr;
    player_title_ = nullptr;
    player_lyric_ = nullptr;
    player_elapsed_ = nullptr;
    player_total_ = nullptr;
    player_state_ = nullptr;
    progress_ = nullptr;
    spectrum_ = nullptr;
    play_icon_ = nullptr;
    mode_icon_ = nullptr;
    favourite_icon_ = nullptr;
    status_time_ = nullptr;
    status_wifi_ = nullptr;
    status_rate_ = nullptr;
    status_dac_box_ = nullptr;
    status_dac_ = nullptr;
    status_codec_ = nullptr;
    status_volume_ = nullptr;
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

void AppLocalMusic::build_status_bar() {
    lv_obj_t* bar = lv_obj_create(root_);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_size(bar, hifi_theme::kScreenWidth, hifi_theme::kStatusBarHeight);
    lv_obj_set_style_bg_color(bar, hifi_theme::status_bar(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_color(bar, hifi_theme::status_border(), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    // Back chevron in the corner, a small Fitts-friendly target.
    lv_obj_t* back = lv_button_create(root_);
    lv_obj_set_pos(back, 0, 0);
    lv_obj_set_size(back, hifi_theme::kStatusBackWidth, hifi_theme::kStatusBarHeight);
    lv_obj_set_style_radius(back, 6, 0);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_pad_all(back, 0, 0);
    lv_obj_remove_flag(back, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(back, this);
    lv_obj_add_event_cb(back, on_transport, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<uintptr_t>(PendingAction::BackToList)));
    lv_obj_t* chevron = lv_label_create(back);
    lv_label_set_text(chevron, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(chevron, &lv_font_montserrat_12, 0);
    lv_obj_set_style_text_color(chevron, hifi_theme::accent(), 0);
    lv_obj_center(chevron);
    lv_obj_remove_flag(chevron, LV_OBJ_FLAG_CLICKABLE);

    status_time_ = lv_label_create(bar);
    lv_label_set_text(status_time_, "--:--");
    lv_obj_set_style_text_font(status_time_, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(status_time_, hifi_theme::ink(), 0);
    lv_obj_align(status_time_, LV_ALIGN_LEFT_MID, hifi_theme::kStatusTimeX, 0);

    // The WiFi icon carries its state in its own colour, as upstream.
    status_wifi_ = lv_label_create(bar);
    lv_label_set_text(status_wifi_, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_font(status_wifi_, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(status_wifi_, hifi_theme::ink_faint(), 0);
    lv_obj_align(status_wifi_, LV_ALIGN_LEFT_MID, hifi_theme::kStatusWifiX, 0);

    // Sample rate and bit depth of the open output.
    status_rate_ = lv_label_create(bar);
    lv_label_set_text(status_rate_, "");
    lv_obj_set_style_text_font(status_rate_, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(status_rate_, hifi_theme::ink_dim(), 0);
    lv_obj_align(status_rate_, LV_ALIGN_LEFT_MID, hifi_theme::kStatusRateX, 0);

    // Bordered DAC tag: lights up when the amp is actually producing audio.
    status_dac_box_ = lv_obj_create(bar);
    lv_obj_set_size(status_dac_box_, hifi_theme::kStatusDacWidth, hifi_theme::kStatusDacHeight);
    lv_obj_align(status_dac_box_, LV_ALIGN_LEFT_MID, hifi_theme::kStatusDacX, 0);
    lv_obj_set_style_radius(status_dac_box_, 3, 0);
    lv_obj_set_style_bg_opa(status_dac_box_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(status_dac_box_, hifi_theme::ink_faint(), 0);
    lv_obj_set_style_border_width(status_dac_box_, 1, 0);
    lv_obj_set_style_pad_all(status_dac_box_, 0, 0);
    lv_obj_remove_flag(status_dac_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(status_dac_box_, LV_OBJ_FLAG_CLICKABLE);
    status_dac_ = lv_label_create(status_dac_box_);
    lv_label_set_text(status_dac_, "DAC");
    lv_obj_set_style_text_font(status_dac_, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(status_dac_, hifi_theme::ink_faint(), 0);
    lv_obj_center(status_dac_);

    status_codec_ = lv_label_create(bar);
    lv_label_set_text(status_codec_, "");
    lv_obj_set_style_text_font(status_codec_, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(status_codec_, hifi_theme::ink_dim(), 0);
    lv_obj_align(status_codec_, LV_ALIGN_LEFT_MID, hifi_theme::kStatusCodecX, 0);

    // Volume packed against the right edge.
    status_volume_ = lv_label_create(bar);
    lv_label_set_text(status_volume_, "0%");
    lv_obj_set_style_text_font(status_volume_, &lv_font_montserrat_10, 0);
    lv_obj_set_style_text_color(status_volume_, hifi_theme::ink_dim(), 0);
    lv_obj_align(status_volume_, LV_ALIGN_RIGHT_MID, -6, 0);
}

void AppLocalMusic::build_player_page() {
    destroy_page();
    player_page_ = true;
    drawn_rows_.fill(-1);

    root_ = lv_obj_create(lv_screen_active());
    lv_obj_set_size(root_, hifi_theme::kScreenWidth, hifi_theme::kScreenHeight);
    lv_obj_align(root_, LV_ALIGN_CENTER, 0, 0);
    style_flat(root_, hifi_theme::bg(), 0);
    // Left-edge swipe right is the way back, as upstream.
    lv_obj_set_user_data(root_, this);
    lv_obj_add_event_cb(root_, on_press_start, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(root_, on_gesture, LV_EVENT_GESTURE, nullptr);

    build_status_bar();

    // Square cover, no vinyl framing: the turntable look was reverted upstream.
    // Without artwork it is a vertical gradient carrying a music glyph.
    lv_obj_t* cover = lv_obj_create(root_);
    lv_obj_set_pos(cover, hifi_theme::kCoverX, hifi_theme::kCoverY);
    lv_obj_set_size(cover, hifi_theme::kCoverSize, hifi_theme::kCoverSize);
    lv_obj_set_style_radius(cover, 0, 0);
    lv_obj_set_style_bg_color(cover, hifi_theme::panel_deep(), 0);
    lv_obj_set_style_bg_grad_color(cover, hifi_theme::accent_deep(), 0);
    lv_obj_set_style_bg_grad_dir(cover, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_pad_all(cover, 0, 0);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* note = lv_label_create(cover);
    lv_label_set_text(note, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(note, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(note, hifi_theme::ink(), 0);
    lv_obj_center(note);

    // Title and artist share one looping marquee line, which frees the row
    // below it for the lyric.
    player_title_ = make_text(root_, selected_title_.c_str(), hifi_theme::ink(), LV_ALIGN_TOP_LEFT,
                              hifi_theme::kTextColumnX, hifi_theme::kTitleY);
    lv_label_set_long_mode(player_title_, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(player_title_, hifi_theme::kTextColumnWidth);

    // Lyric line: replaced wholesale on each change rather than scrolled.  LRC
    // parsing is not wired yet, so for now it carries the playback state.
    player_lyric_ = make_text(root_, "", hifi_theme::accent_bright(), LV_ALIGN_TOP_LEFT,
                              hifi_theme::kTextColumnX, hifi_theme::kLyricY);
    lv_label_set_long_mode(player_lyric_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(player_lyric_, hifi_theme::kTextColumnWidth);
    player_state_ = player_lyric_;

    // Elapsed and total sit at either end of the text column.
    player_elapsed_ = make_text(root_, "00:00", hifi_theme::ink_dim(), LV_ALIGN_TOP_LEFT,
                                hifi_theme::kTextColumnX, hifi_theme::kTimeRowY);
    player_total_ = make_text(root_, "--:--", hifi_theme::ink_faint(), LV_ALIGN_TOP_RIGHT, -8,
                              hifi_theme::kTimeRowY);

    build_spectrum(root_);

    // Thick draggable seek slider, spanning the full content width.
    progress_ = lv_slider_create(root_);
    lv_obj_set_pos(progress_, hifi_theme::kProgressX, hifi_theme::kProgressY);
    lv_obj_set_size(progress_, hifi_theme::kProgressWidth, hifi_theme::kProgressHeight);
    lv_slider_set_range(progress_, 0, 1000);
    lv_obj_set_style_radius(progress_, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(progress_, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_, hifi_theme::ink_faint(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(progress_, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(progress_, hifi_theme::magenta(), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_color(progress_, hifi_theme::magenta(), LV_PART_INDICATOR);
    lv_obj_set_style_shadow_width(progress_, 4, LV_PART_INDICATOR);
    lv_obj_set_style_shadow_opa(progress_, LV_OPA_50, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_, hifi_theme::ink(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(progress_, 6, LV_PART_KNOB);

    build_control_bar();
    update_transport_icons();
}

void AppLocalMusic::build_spectrum(lv_obj_t* card) {
    // One canvas, not one object per cell: upstream measured that 308 separate
    // cell objects meant 308 invalidate/redraw pipelines, and that -- not the
    // number of cells -- was what made the display stutter.
    const std::size_t pixels =
        static_cast<std::size_t>(hifi_theme::kSpectrumWidth) * hifi_theme::kSpectrumHeight;
    if (spectrum_buffer_ == nullptr) {
#ifdef ESP_PLATFORM
        spectrum_buffer_ =
            static_cast<lv_color_t*>(heap_caps_malloc(pixels * sizeof(lv_color_t), MALLOC_CAP_SPIRAM));
#endif
        if (spectrum_buffer_ == nullptr) {
            spectrum_buffer_ = static_cast<lv_color_t*>(lv_malloc(pixels * sizeof(lv_color_t)));
        }
    }
    if (spectrum_buffer_ == nullptr) {
        return;
    }

    spectrum_ = lv_canvas_create(card);
    lv_obj_set_pos(spectrum_, hifi_theme::kSpectrumX, hifi_theme::kSpectrumY);
    lv_obj_remove_flag(spectrum_, LV_OBJ_FLAG_CLICKABLE);
    lv_canvas_set_buffer(spectrum_, spectrum_buffer_, hifi_theme::kSpectrumWidth,
                         hifi_theme::kSpectrumHeight, LV_COLOR_FORMAT_RGB565);
    // The card is transparent, so the canvas matches the screen behind it.
    lv_canvas_fill_bg(spectrum_, hifi_theme::bg(), LV_OPA_COVER);
    for (int32_t column = 0; column < hifi_theme::kSpectrumCols; ++column) {
        for (int32_t row = 0; row < hifi_theme::kSpectrumRows; ++row) {
            draw_spectrum_cell(column, row, false);
        }
    }
}

void AppLocalMusic::draw_spectrum_cell(int32_t column, int32_t row, bool lit) {
    if (spectrum_ == nullptr) return;
    const int32_t x0 = column * hifi_theme::kSpectrumColPitch;
    // Row 0 is the bottom of the meter.
    const int32_t y0 = hifi_theme::kSpectrumHeight - (row + 1) * hifi_theme::kSpectrumRowPitch + 1;

    lv_color_t colour = hifi_theme::ink_faint();
    if (lit) {
        // The classic LED VU ramp, in this palette: green through the body,
        // purple higher up, magenta at the very top.
        const int32_t high = hifi_theme::kSpectrumRows * 3 / 4;
        const int32_t mid = hifi_theme::kSpectrumRows / 2;
        colour = row >= high ? hifi_theme::magenta()
                             : (row >= mid ? hifi_theme::accent() : hifi_theme::ok());
    }
    for (int32_t dy = 0; dy < hifi_theme::kSpectrumCellHeight; ++dy) {
        for (int32_t dx = 0; dx < hifi_theme::kSpectrumCellWidth; ++dx) {
            const int32_t x = x0 + dx;
            const int32_t y = y0 + dy;
            if (x < 0 || y < 0 || x >= hifi_theme::kSpectrumWidth ||
                y >= hifi_theme::kSpectrumHeight) {
                continue;
            }
            lv_canvas_set_px(spectrum_, x, y, colour, LV_OPA_COVER);
        }
    }
}

void AppLocalMusic::build_control_bar() {
    lv_obj_t* bar = lv_obj_create(root_);
    lv_obj_set_pos(bar, 0, hifi_theme::kControlBarY);
    lv_obj_set_size(bar, hifi_theme::kScreenWidth, hifi_theme::kControlBarHeight);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(bar, hifi_theme::ink_faint(), 0);
    lv_obj_set_style_border_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    // Slot order is fixed: upstream tried centring the play button and that
    // change was explicitly rejected.  Play stays in slot 2.
    static const char* kSymbols[hifi_theme::kControlSlots] = {
        LV_SYMBOL_SHUFFLE, LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT,
        "★",               LV_SYMBOL_LIST, LV_SYMBOL_LOOP};
    static const PendingAction kActions[hifi_theme::kControlSlots] = {
        PendingAction::CyclePlayMode,   PendingAction::Prev,
        PendingAction::PlayPause,       PendingAction::Next,
        PendingAction::ToggleFavourite, PendingAction::BackToList,
        PendingAction::ToggleCassette};

    for (int32_t i = 0; i < hifi_theme::kControlSlots; ++i) {
        lv_obj_t* slot = lv_button_create(bar);
        lv_obj_set_pos(slot, i * hifi_theme::kControlSlotWidth, 0);
        lv_obj_set_size(slot, hifi_theme::kControlSlotWidth, hifi_theme::kControlBarHeight);
        lv_obj_set_style_radius(slot, 10, 0);
        lv_obj_set_style_bg_opa(slot, LV_OPA_TRANSP, 0);
        lv_obj_set_style_shadow_width(slot, 0, 0);
        lv_obj_set_style_border_width(slot, 0, 0);
        lv_obj_set_style_pad_all(slot, 0, 0);
        lv_obj_remove_flag(slot, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_user_data(slot, this);
        lv_obj_add_event_cb(slot, on_transport, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<uintptr_t>(kActions[i])));

        if (i == 2) {
            // The ring is what marks the play button as the primary control.
            lv_obj_t* ring = lv_obj_create(slot);
            lv_obj_set_size(ring, hifi_theme::kPlayRingSize, hifi_theme::kPlayRingSize);
            lv_obj_align(ring, LV_ALIGN_CENTER, 0, 0);
            lv_obj_set_style_radius(ring, 20, 0);
            lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(ring, hifi_theme::magenta(), 0);
            lv_obj_set_style_border_width(ring, 2, 0);
            lv_obj_set_style_shadow_width(ring, 0, 0);
            lv_obj_remove_flag(ring, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(ring, LV_OBJ_FLAG_CLICKABLE);
            play_icon_ = lv_label_create(ring);
            lv_label_set_text(play_icon_, kSymbols[i]);
            lv_obj_set_style_text_font(play_icon_, &lv_font_montserrat_16, 0);
            lv_obj_set_style_text_color(play_icon_, hifi_theme::accent_bright(), 0);
            lv_obj_center(play_icon_);
            lv_obj_remove_flag(play_icon_, LV_OBJ_FLAG_CLICKABLE);
            continue;
        }

        lv_obj_t* icon = lv_label_create(slot);
        lv_label_set_text(icon, kSymbols[i]);
        // LVGL has no star glyph, so that one has to come from the CJK font.
        lv_obj_set_style_text_font(icon, i == 4 ? body_font() : &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(icon, hifi_theme::ink_dim(), 0);
        lv_obj_center(icon);
        lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);
        if (i == 0) mode_icon_ = icon;
        if (i == 4) favourite_icon_ = icon;
    }
}

void AppLocalMusic::update_transport_icons() {
    if (play_icon_ != nullptr && playback_) {
        const bool playing = playback_->tick().state == media::PlaybackState::Playing;
        lv_label_set_text(play_icon_, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    if (mode_icon_ != nullptr) {
        // Sequential is the resting mode and stays dim; the others light up so
        // it is obvious that something other than plain playback is in force.
        const char* symbol = LV_SYMBOL_SHUFFLE;
        switch (play_mode_) {
            case PlayMode::Sequential:
                symbol = LV_SYMBOL_RIGHT;
                break;
            case PlayMode::RepeatAll:
                symbol = LV_SYMBOL_LOOP;
                break;
            case PlayMode::RepeatOne:
                symbol = LV_SYMBOL_REFRESH;
                break;
            case PlayMode::Shuffle:
                symbol = LV_SYMBOL_SHUFFLE;
                break;
        }
        lv_label_set_text(mode_icon_, symbol);
        lv_obj_set_style_text_color(
            mode_icon_,
            play_mode_ == PlayMode::Sequential ? hifi_theme::ink_dim() : hifi_theme::accent_bright(),
            0);
    }
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

void AppLocalMusic::on_transport(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    self->pending_action_ =
        static_cast<PendingAction>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(event)));
}

void AppLocalMusic::on_press_start(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t point{};
    lv_indev_get_point(indev, &point);
    self->press_start_x_ = point.x;
}

void AppLocalMusic::on_gesture(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    if (lv_indev_get_gesture_dir(indev) != LV_DIR_RIGHT) return;
    // Edge gesture only: a swipe that starts mid-screen belongs to whatever is
    // under the finger, not to navigation.
    if (self->press_start_x_ > kEdgeGestureWidth) return;
    self->pending_action_ = PendingAction::BackToList;
}

void AppLocalMusic::apply_pending_action() {
    const PendingAction action = pending_action_;
    if (action == PendingAction::None) {
        return;
    }
    pending_action_ = PendingAction::None;

    switch (action) {
        case PendingAction::BackToList: {
            // Stop outside the LVGL lock: the pump task may be waiting for that
            // same lock to borrow the SD bus, and holding it here would
            // deadlock.
            if (playback_) {
                playback_->stop();
            }
            LvglLockGuard lock;
            build_list_page();
            return;
        }
        case PendingAction::SwitchTab: {
            tab_ = pending_tab_;
            LvglLockGuard lock;
            build_list_page();
            return;
        }
        case PendingAction::SelectTrack:
            select_track(pending_index_);
            return;
        case PendingAction::PlayPause: {
            if (!playback_) return;
            if (playback_->tick().state == media::PlaybackState::Playing) {
                playback_->pause();
            } else if (!playback_->resume()) {
                // Nothing to resume: the track has ended or failed, so start it
                // over rather than leaving a dead button.
                start_track(current_index_);
                return;
            }
            LvglLockGuard lock;
            update_transport_icons();
            return;
        }
        case PendingAction::Prev:
            step_track(-1);
            return;
        case PendingAction::Next:
            step_track(1);
            return;
        case PendingAction::CyclePlayMode: {
            play_mode_ = static_cast<PlayMode>((static_cast<uint8_t>(play_mode_) + 1) % 4);
            LvglLockGuard lock;
            update_transport_icons();
            return;
        }
        case PendingAction::ToggleFavourite: {
            // Favourites need library storage, which does not exist yet.  Say
            // so on the lyric line rather than silently doing nothing.
            LvglLockGuard lock;
            if (player_lyric_ != nullptr) lv_label_set_text(player_lyric_, "收藏需要曲库支持");
            shown_status_.clear();
            return;
        }
        case PendingAction::ToggleCassette: {
            cassette_view_ = !cassette_view_;
            LvglLockGuard lock;
            if (player_lyric_ != nullptr) lv_label_set_text(player_lyric_, "磁带视图尚未移植");
            shown_status_.clear();
            cassette_view_ = false;
            return;
        }
        case PendingAction::None:
            return;
    }
}

void AppLocalMusic::step_track(int direction) {
    if (tracks_.empty()) return;

    std::size_t next = current_index_;
    switch (play_mode_) {
        case PlayMode::RepeatOne:
            break;  // Manual prev/next still moves; only auto-advance repeats.
        case PlayMode::Shuffle:
            // A genuine random pick, as upstream, not a shuffled traversal.
            next = static_cast<std::size_t>(lv_rand(0, static_cast<uint32_t>(tracks_.size() - 1)));
            start_track(next);
            return;
        default:
            break;
    }

    if (direction > 0) {
        if (current_index_ + 1 >= tracks_.size()) {
            // Sequential stops at the end of the list; repeat-all wraps.
            if (play_mode_ == PlayMode::Sequential) return;
            next = 0;
        } else {
            next = current_index_ + 1;
        }
    } else {
        if (current_index_ == 0) {
            if (play_mode_ == PlayMode::Sequential) return;
            next = tracks_.size() - 1;
        } else {
            next = current_index_ - 1;
        }
    }
    start_track(next);
}

void AppLocalMusic::select_track(std::size_t index) {
    const auto selected = local_music::select_track(tracks_, index);
    if (!selected.accepted || index >= tracks_.size()) {
        return;
    }

    {
        LvglLockGuard lock;
        selected_title_ = tracks_[index].title;
        build_player_page();
    }
    start_track(index);
}

void AppLocalMusic::start_track(std::size_t index) {
    if (index >= tracks_.size() || !playback_) {
        return;
    }
    current_index_ = index;
    selected_title_ = tracks_[index].title;
    shown_status_.clear();
    shown_elapsed_.clear();
    analyzer_.reset();
    pcm_tap_.reset();

    {
        LvglLockGuard lock;
        if (player_title_ != nullptr) lv_label_set_text(player_title_, selected_title_.c_str());
    }

    // Stopping and opening the stream both borrow the display bus, so neither
    // may run while an application-level LVGL lock is held.
    playback_->stop();
    auto stream = sd_card_.open_track(tracks_[index]);
    // The factory returns an owned pointer: the decoder adapter keeps it alive
    // for the track and frees it (with its codec handle) when the track ends
    // or the next one is selected.
    auto backend = media::create_hifi_decoder_backend();
    if (!stream || !backend) {
        LvglLockGuard lock;
        if (player_lyric_ != nullptr) lv_label_set_text(player_lyric_, "解码器不可用");
        return;
    }

    auto decoder = std::make_unique<media::HifiDecoderAdapter>(std::move(backend));
    playback_->select(selected_title_, std::move(stream), std::move(decoder));
    playback_->set_pcm_tap(&pcm_tap_);
    playback_->start();

    LvglLockGuard lock;
    update_transport_icons();
}

void AppLocalMusic::refresh_spectrum() {
    if (spectrum_ == nullptr) return;

    // Drain whatever the audio task published since the last frame.  The FFT
    // runs here, on the UI side, never in the audio writer.
    //
    // The tap carries interleaved samples, so the folding has to use the real
    // channel count of the stream: hard-coding stereo shifted the frequency
    // axis of every mono track.
    const uint8_t channels = playback_ != nullptr && playback_->tick().channels > 0
                                 ? playback_->tick().channels
                                 : 1;
    int16_t samples[256];
    bool fed = false;
    while (true) {
        const std::size_t count = pcm_tap_.pop(samples, sizeof(samples) / sizeof(samples[0]));
        if (count == 0) break;
        analyzer_.push(samples, count, channels);
        fed = true;
    }
    if (!fed) {
        analyzer_.decay();
    }

    // Only columns whose height actually changed are redrawn.
    const auto& columns = analyzer_.columns();
    for (int32_t c = 0; c < hifi_theme::kSpectrumCols; ++c) {
        const int32_t lit =
            static_cast<int32_t>(columns[c]) * hifi_theme::kSpectrumRows / 256;
        const int32_t previous = drawn_rows_[c];
        if (lit == previous) continue;
        // Repaint only the cells that actually changed state.  Redrawing the
        // whole column every frame is what made the meter look sluggish.
        const int32_t from = previous < 0 ? 0 : std::min(previous, lit);
        const int32_t to = std::max(previous, lit);
        for (int32_t row = from; row < to; ++row) {
            draw_spectrum_cell(c, row, row < lit);
        }
        drawn_rows_[c] = lit;
    }
    lv_obj_invalidate(spectrum_);
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

    std::string elapsed = "00:00";
    if (tick.sample_rate > 0) {
        const uint32_t seconds = tick.played_frames / tick.sample_rate;
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "%02u:%02u", static_cast<unsigned>(seconds / 60),
                      static_cast<unsigned>(seconds % 60));
        elapsed = buffer;
    }

    LvglLockGuard lock;
    refresh_spectrum();

    if (status == shown_status_ && elapsed == shown_elapsed_) {
        return;
    }
    shown_status_ = status;
    shown_elapsed_ = elapsed;

    lv_label_set_text(player_state_, shown_status_.c_str());
    if (player_elapsed_ != nullptr) {
        lv_label_set_text(player_elapsed_, shown_elapsed_.c_str());
    }
    update_transport_icons();
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
