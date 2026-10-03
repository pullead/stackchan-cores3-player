#include "app_local_music.h"

#include "hifi_theme.h"

#include <assets/assets.h>
#include <audio/audio_codec.h>
#include <board.h>
#include <hal/board/cores3_audio_codec.h>
#include <hal/hal.h>
#include <media/audio/volume_policy.h>
#include <media/decoder/hifi_decoder_adapter.h>
#include <media/library/audio_format.h>
#include <media/library/library_store.h>
#include <media/library/mp3_info.h>
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

// How much of a file's head is read for metadata.  ID3v2 tags run from tens of
// bytes to over a hundred kilobytes; this covers the common case, including the
// first MPEG frame, without reading a whole cover art frame off the card.
constexpr std::size_t kMetadataProbeBytes = 16384;

// One file per interval while the list is idle.  A head read borrows the display
// bus for about ten milliseconds, so spreading the library out is what keeps the
// list responsive; the whole card is indexed over a few minutes of browsing, one
// file at a time, and never while a track is playing.
constexpr uint32_t kIndexIntervalMs = 250;

// playable_ states: not looked at yet, decodable, and a container this firmware
// has no demuxer for (Ogg, MP4).
constexpr uint8_t kFormatUnknown = 0;
constexpr uint8_t kFormatPlayable = 1;
constexpr uint8_t kFormatUnsupported = 2;

// Case-insensitive suffix test.  The MPEG reader below only speaks MP3, and WAV
// PCM that happens to contain a 0xFFEx pattern would otherwise report a
// nonsense duration for a file that never had one.
bool ends_with(const std::string& text, const char* suffix) {
    const std::size_t length = std::char_traits<char>::length(suffix);
    if (text.size() < length) return false;
    for (std::size_t i = 0; i < length; ++i) {
        char character = text[text.size() - length + i];
        if (character >= 'A' && character <= 'Z') {
            character = static_cast<char>(character - 'A' + 'a');
        }
        if (character != suffix[i]) return false;
    }
    return true;
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

    // Favourites live in NVS, never on the card: the SD card is shared with
    // another player and stays read-only here.
#ifdef ESP_PLATFORM
    store_ = media::LibraryStore(media::make_nvs_library_store_port());
    store_.load();
#endif

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

    // Nothing is indexed yet: every row falls back to its filename until the
    // indexer has read that file.
    metadata_.assign(tracks_.size(), media::Mp3Tags{});
    indexed_.assign(tracks_.size(), 0);
    playable_.assign(tracks_.size(), 0);
    index_cursor_ = 0;
    last_index_tick_ = lv_tick_get();

    LvglLockGuard lock;
    build_list_page();
}

void AppLocalMusic::onRunning() {
    // Runs outside LVGL event dispatch, so rebuilding the page is safe here.
    apply_pending_action();
    advance_when_finished();
    index_one_track_when_idle();
    refresh_player_page();
}

AppLocalMusic::~AppLocalMusic() {
    // uninstallAllApps() destroys the app without calling onClose(), so this is
    // the only teardown the AI handoff ever gets -- and it is what stops the
    // widget tree from outliving the object and dangling its user_data.
    release_resources();
}

void AppLocalMusic::onClose() {
    mclog::tagInfo(getAppInfo().name, "on close");
    release_resources();
}

void AppLocalMusic::release_resources() {
    // Every step is idempotent and null-guarded, so running it twice (onClose
    // followed by the destructor) is harmless.
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
    metadata_.clear();
    indexed_.clear();
    index_cursor_ = 0;
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
    // The pool rows live in the tree that was just deleted: leaving the
    // pointers behind would let the next rebind write through a freed object.
    row_pool_.fill(nullptr);
    row_title_.fill(nullptr);
    row_artist_.fill(nullptr);
    row_position_.fill(0);
    first_row_ = -1;
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
    lv_obj_set_user_data(root_, this);
    // Same edge gesture as the player page, but on the list it leaves the app:
    // there is nothing above the list to go back to.
    lv_obj_add_event_cb(root_, on_press_start, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(root_, on_gesture, LV_EVENT_GESTURE, nullptr);

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

    // Which tracks this page shows: the whole library for Songs, the favourites
    // for ★.  The remaining three tabs need metadata the scanner does not
    // collect yet, and say so below.
    filter_visible_tracks();

    char header[32] = {};
    if (tab_ == Tab::Favourites) {
        std::snprintf(header, sizeof(header), "收藏 %u 首", static_cast<unsigned>(visible_.size()));
    } else {
        std::snprintf(header, sizeof(header), "共 %u 首", static_cast<unsigned>(visible_.size()));
    }
    status_label_ = make_text(root_, header, hifi_theme::ink_dim(), LV_ALIGN_TOP_RIGHT,
                              hifi_theme::kCountRight, 6);

    // The only visible way out of the app: without it the launcher can never be
    // reached again, because nothing else puts this app to sleep.
    lv_obj_t* exit_button = lv_button_create(root_);
    lv_obj_set_size(exit_button, hifi_theme::kExitWidth, hifi_theme::kExitHeight);
    lv_obj_align(exit_button, LV_ALIGN_TOP_RIGHT, hifi_theme::kExitRight, hifi_theme::kExitY);
    style_flat(exit_button, hifi_theme::panel(), hifi_theme::kTabRadius);
    lv_obj_set_user_data(exit_button, this);
    lv_obj_add_event_cb(exit_button, on_exit_clicked, LV_EVENT_CLICKED, nullptr);
    // The CJK body font carries no FontAwesome glyphs, so the icon has to name
    // the symbol font explicitly, the same way the transport icons do.
    lv_obj_t* exit_label =
        make_text(exit_button, LV_SYMBOL_HOME, hifi_theme::ink_dim(), LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_text_font(exit_label, &lv_font_montserrat_16, 0);

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

    if (tab_ == Tab::Artists || tab_ == Tab::Albums || tab_ == Tab::Today) {
        // These need ID3 tags and import timestamps, which the scanner does not
        // collect yet.  Say so instead of showing an empty list that looks like
        // a failure.
        make_text(list_, "该分类需要曲库信息", hifi_theme::ink_dim(), LV_ALIGN_CENTER, 0, 0);
        return;
    }
    if (visible_.empty()) {
        // An empty ★ view is not an error, so it does not share the message a
        // card with no music on it gets.
        make_text(list_, tab_ == Tab::Favourites ? "尚无收藏" : "未找到音乐文件",
                  hifi_theme::ink_dim(), LV_ALIGN_CENTER, 0, 0);
        return;
    }

    // A spacer carries the full content height: the pool rows alone would give
    // the container almost nothing to scroll over.
    const int32_t content_height =
        static_cast<int32_t>(visible_.size()) * hifi_theme::kRowPitch;
    lv_obj_t* spacer = lv_obj_create(list_);
    lv_obj_set_pos(spacer, 0, 0);
    lv_obj_set_size(spacer, 1, content_height);
    lv_obj_set_style_bg_opa(spacer, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(spacer, 0, 0);

    for (int32_t slot = 0; slot < kRowPool; ++slot) {
        lv_obj_t* row = lv_button_create(list_);
        lv_obj_set_size(row, hifi_theme::kRowWidth, hifi_theme::kRowHeight);
        style_flat(row, hifi_theme::panel(), hifi_theme::kRowRadius);
        lv_obj_set_user_data(row, this);
        // The callback carries no index: the row is a pool slot, and what it
        // shows changes as the list scrolls.
        lv_obj_add_event_cb(row, on_row_clicked, LV_EVENT_CLICKED, nullptr);

        // Single line: title on the left, artist on the right and dimmer.  Not
        // joined into "title · artist" -- in CJK that separator smears into the
        // glyphs, and two labels read more clearly for the same glyph count.
        lv_obj_t* title_label = make_text(row, "", hifi_theme::ink(), LV_ALIGN_LEFT_MID, 10, 0);
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

        row_pool_[slot] = row;
        row_title_[slot] = title_label;
        row_artist_[slot] = detail;
    }

    lv_obj_set_user_data(list_, this);
    lv_obj_add_event_cb(list_, on_list_scrolled, LV_EVENT_SCROLL, nullptr);
    first_row_ = -1;
    rebind_rows();

    if (content_height > hifi_theme::kListHeight) {
        scroll_slider_ = lv_slider_create(root_);
        lv_obj_set_pos(scroll_slider_, hifi_theme::kScrollSliderX, hifi_theme::kListY);
        lv_obj_set_size(scroll_slider_, hifi_theme::kScrollSliderWidth, hifi_theme::kListHeight);
        lv_slider_set_range(scroll_slider_, 0, content_height - hifi_theme::kListHeight);
        lv_obj_set_style_bg_color(scroll_slider_, hifi_theme::panel_deep(), LV_PART_MAIN);
        lv_obj_set_style_bg_color(scroll_slider_, hifi_theme::accent_deep(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(scroll_slider_, hifi_theme::accent_bright(), LV_PART_KNOB);
        lv_obj_set_user_data(scroll_slider_, this);
        lv_obj_add_event_cb(scroll_slider_, on_scroll_slider, LV_EVENT_VALUE_CHANGED, nullptr);
        sync_scroll_slider();
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
    lv_obj_set_user_data(progress_, this);
    lv_obj_add_event_cb(progress_, on_progress_event, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(progress_, on_progress_event, LV_EVENT_RELEASED, nullptr);

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
    if (favourite_icon_ != nullptr) {
        // The CJK font has both stars (U+2605 filled, U+2606 hollow), so the
        // state reads without relying on colour alone.
        lv_label_set_text(favourite_icon_, current_favourite_ ? "★" : "☆");
        lv_obj_set_style_text_color(favourite_icon_,
                                    current_favourite_ ? hifi_theme::accent_bright()
                                                       : hifi_theme::ink_dim(),
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
    // The row is a pool slot, so the track it stands for is whatever the last
    // rebind put there: a position in the current view, not a track index.
    for (int32_t slot = 0; slot < kRowPool; ++slot) {
        if (self->row_pool_[slot] == target) {
            const std::size_t position = self->row_position_[slot];
            if (position >= self->visible_.size()) return;
            self->pending_index_ = self->visible_[position];
            self->pending_action_ = PendingAction::SelectTrack;
            return;
        }
    }
}

void AppLocalMusic::on_back_clicked(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    self->pending_action_ = PendingAction::BackToList;
}

void AppLocalMusic::on_exit_clicked(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    // Only record the intent here: close() tears the page down through
    // onClose(), and LVGL must not lose objects while it is dispatching events.
    self->pending_action_ = PendingAction::Exit;
}

void AppLocalMusic::on_scroll_slider(lv_event_t* event) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(slider));
    if (self == nullptr || self->list_ == nullptr) return;
    // sync_scroll_slider() writes the value back from the scroll offset, and
    // that write raises VALUE_CHANGED again, so an echo has to be ignored or
    // the two would keep driving each other.
    if (self->syncing_slider_) return;
    // The slider grows upwards from the bottom while scroll_y grows downwards
    // from the top, so the value is inverted rather than mapped one to one.
    const int32_t range = lv_slider_get_max_value(slider);
    const int32_t value = lv_slider_get_value(slider);
    lv_obj_scroll_to_y(self->list_, range - value, LV_ANIM_OFF);
}

void AppLocalMusic::filter_visible_tracks() {
    visible_.clear();
    if (tab_ == Tab::Favourites) {
        // The filter itself lives in the library module so it can be host
        // tested; this only hands it the paths in library order.
        std::vector<std::string> paths;
        paths.reserve(tracks_.size());
        for (const media::SdTrack& track : tracks_) {
            paths.push_back(track.path);
        }
        visible_ = media::favourite_indices(paths, store_);
        return;
    }
    visible_.reserve(tracks_.size());
    for (std::size_t i = 0; i < tracks_.size(); ++i) {
        visible_.push_back(i);
    }
}

const char* AppLocalMusic::row_title_for(std::size_t index) const {
    if (index < metadata_.size() && indexed_[index] != 0 && !metadata_[index].title.empty()) {
        return metadata_[index].title.c_str();
    }
    if (index >= tracks_.size()) return "未知曲目";
    return tracks_[index].title.empty() ? "未知曲目" : tracks_[index].title.c_str();
}

const char* AppLocalMusic::row_artist_for(std::size_t index) const {
    if (index < playable_.size() && playable_[index] == kFormatUnsupported) {
        // Said here rather than after a tap: the file is on the card, and the
        // list is where the user finds out it cannot be played.
        return "格式不支持";
    }
    if (index < metadata_.size() && indexed_[index] != 0 && !metadata_[index].artist.empty()) {
        return metadata_[index].artist.c_str();
    }
    // ID3 metadata is not read for this track yet; the slot is kept so the row
    // layout does not shift once it is.
    return "未知艺术家";
}

void AppLocalMusic::index_one_track_when_idle() {
    if (tracks_.empty() || index_cursor_ >= tracks_.size()) return;

    // Two conditions, both load-bearing.  The list page means the user is
    // browsing rather than listening, and an idle controller means the pump task
    // is not going to borrow the SD bus under us -- the handoff is global and not
    // reentrant, so overlapping borrowers would break playback.
    if (player_page_) return;
    if (playback_ && playback_->tick().state != media::PlaybackState::Idle) return;

    const uint32_t now = lv_tick_get();
    if (now - last_index_tick_ < kIndexIntervalMs) return;
    last_index_tick_ = now;

    std::size_t index = index_cursor_;
    while (index < tracks_.size() && indexed_[index] != 0) ++index;
    if (index >= tracks_.size()) {
        index_cursor_ = index;
        return;
    }

    std::vector<uint8_t> head;
    std::size_t tag_bytes = 0;
    if (sd_card_.read_head(tracks_[index], head, kMetadataProbeBytes)) {
        // Same bytes decide both what the row is called and whether it can play,
        // so the list never offers a track this firmware cannot decode.
        std::size_t data_offset = 0;
        playable_[index] = media::sniff_audio_format(head.data(), head.size(), data_offset) ==
                                   media::AudioFormat::Unknown
                               ? kFormatUnsupported
                               : kFormatPlayable;
        media::Mp3Tags tags;
        if (media::parse_id3v2_tags(head.data(), head.size(), tags, tag_bytes)) {
            metadata_[index] = std::move(tags);
        }
    }
    // Marked either way: a file with no readable tag keeps its filename, and
    // retrying it every tick would never end.
    indexed_[index] = 1;
    index_cursor_ = index + 1;

    {
        // Let the visible rows pick the new title up now rather than on the next
        // scroll.  onRunning() is outside LVGL event dispatch, so taking the lock
        // here is safe.
        LvglLockGuard lock;
        first_row_ = -1;
        rebind_rows();
    }
}

void AppLocalMusic::rebind_rows() {
    if (list_ == nullptr || row_pool_[0] == nullptr || visible_.empty()) {
        return;
    }
    const int32_t scroll_y = lv_obj_get_scroll_y(list_);
    int32_t first = scroll_y / hifi_theme::kRowPitch;
    const int32_t last_first = static_cast<int32_t>(visible_.size()) - kRowPool;
    if (first > last_first) first = last_first;
    if (first < 0) first = 0;
    // Most scroll events do not move a whole row, and rebinding is the only
    // work this handler does.
    if (first == first_row_) return;
    first_row_ = first;

    for (int32_t slot = 0; slot < kRowPool; ++slot) {
        lv_obj_t* row = row_pool_[slot];
        const std::size_t position = static_cast<std::size_t>(first + slot);
        if (position >= visible_.size()) {
            // Fewer rows than pool slots: keep the surplus rows out of the way.
            row_position_[slot] = 0;
            lv_obj_add_flag(row, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        // Rows are laid out by their position in the view, not by track index,
        // so a filtered view has no gaps where the hidden tracks would be.
        const std::size_t index = visible_[position];
        row_position_[slot] = position;
        lv_obj_remove_flag(row, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(row, 0, static_cast<int32_t>(position) * hifi_theme::kRowPitch);
        lv_label_set_text(row_title_[slot], row_title_for(index));
        lv_label_set_text(row_artist_[slot], row_artist_for(index));
    }
}

void AppLocalMusic::sync_scroll_slider() {
    if (scroll_slider_ == nullptr || list_ == nullptr) return;
    syncing_slider_ = true;
    const int32_t range = lv_slider_get_max_value(scroll_slider_);
    const int32_t scroll_y = lv_obj_get_scroll_y(list_);
    const int32_t value = range - scroll_y;
    lv_slider_set_value(scroll_slider_, value < 0 ? 0 : value, LV_ANIM_OFF);
    syncing_slider_ = false;
}

void AppLocalMusic::on_list_scrolled(lv_event_t* event) {
    auto* target = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(target));
    if (self == nullptr) return;
    self->rebind_rows();
    self->sync_scroll_slider();
}

void AppLocalMusic::on_progress_event(lv_event_t* event) {
    auto* slider = static_cast<lv_obj_t*>(lv_event_get_current_target(event));
    auto* self = static_cast<AppLocalMusic*>(lv_obj_get_user_data(slider));
    if (self == nullptr) return;

    if (lv_event_get_code(event) == LV_EVENT_PRESSED) {
        self->seeking_ = true;
        return;
    }

    // Only on release: seeking on every value change would issue dozens of seeks
    // and decoder re-opens during one drag.
    self->seeking_ = false;
    const int32_t range = lv_slider_get_max_value(slider);
    if (range <= 0) return;
    self->pending_seek_fraction_ =
        static_cast<float>(lv_slider_get_value(slider)) / static_cast<float>(range);
    self->pending_action_ = PendingAction::Seek;
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
    // Back from the player page; out of the app from the list.
    self->pending_action_ =
        self->player_page_ ? PendingAction::BackToList : PendingAction::Exit;
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
            if (tracks_.empty() || current_index_ >= tracks_.size()) return;
            current_favourite_ =
                store_.toggle_favourite(media::make_track_id(tracks_[current_index_].path));
            if (!store_.last_write_ok()) {
                // The star deliberately keeps the old state rather than showing
                // something that would be gone after a reboot.
                mclog::tagWarn(getAppInfo().name, "favourite was not persisted");
            }
            LvglLockGuard lock;
            update_transport_icons();
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
        case PendingAction::Seek: {
            if (!playback_ || total_seconds_ == 0) return;
            const auto tick = playback_->tick();
            if (tick.sample_rate == 0) return;
            // The controller moves by byte fraction; the frame estimate only
            // keeps the elapsed clock consistent with the new position.
            playback_->seek_fraction(
                pending_seek_fraction_,
                static_cast<std::size_t>(total_seconds_) * static_cast<std::size_t>(tick.sample_rate));
            // Force the clock and the bar to be redrawn even if the rounded
            // second happens to be the same.
            shown_elapsed_.clear();
            shown_total_.clear();
            return;
        }
        case PendingAction::Exit:
            // Let Mooncake run onClose() on its next update, then hand the
            // launcher back.  Closing the audio path first keeps the teardown
            // that the destructor may also run from blocking on a live pump.
            if (playback_) {
                playback_->stop();
            }
            close();
            return;
        case PendingAction::None:
            return;
    }
}

void AppLocalMusic::advance_when_finished() {
    if (!playback_ || !playback_->finished()) {
        return;
    }

    // Only the controller can tell "the track ran out" from "the user stopped
    // it"; the mode then decides what that means, and sequential has nothing
    // left to do once the view ends.
    const media::FinishAction action =
        media::action_after_finish(play_mode_, media::is_last_in_view(visible_, current_index_));
    switch (action) {
        case media::FinishAction::RestartTrack:
            start_track(current_index_);
            return;
        case media::FinishAction::AdvanceTrack:
            if (!step_track(1)) {
                // Nothing in the view to move to: stop so the flag is cleared and
                // the next tick does not keep retrying.
                playback_->stop();
            }
            return;
        case media::FinishAction::Stop:
            // Stop for real: that clears the finished flag so the next tick does
            // not keep trying to advance off the end of the list.
            playback_->stop();
            return;
    }
}

bool AppLocalMusic::step_track(int direction) {
    if (tracks_.empty() || visible_.empty()) return false;

    // Navigation follows the view the list was showing, so next/prev stay inside
    // the ★ view when that is what the user was browsing.
    const std::vector<std::size_t>& order = visible_;
    std::size_t position = 0;
    for (std::size_t i = 0; i < order.size(); ++i) {
        if (order[i] == current_index_) {
            position = i;
            break;
        }
    }

    switch (play_mode_) {
        case PlayMode::RepeatOne:
            break;  // Manual prev/next still moves; only auto-advance repeats.
        case PlayMode::Shuffle: {
            // A genuine random pick, as upstream, not a shuffled traversal.
            const std::size_t pick =
                static_cast<std::size_t>(lv_rand(0, static_cast<uint32_t>(order.size() - 1)));
            start_track(order[pick]);
            return true;
        }
        default:
            break;
    }

    if (direction > 0) {
        if (position + 1 >= order.size()) {
            // Sequential stops at the end of the list; repeat-all wraps.
            if (play_mode_ == PlayMode::Sequential) return false;
            position = 0;
        } else {
            position += 1;
        }
    } else {
        if (position == 0) {
            if (play_mode_ == PlayMode::Sequential) return false;
            position = order.size() - 1;
        } else {
            position -= 1;
        }
    }
    start_track(order[position]);
    return true;
}

void AppLocalMusic::select_track(std::size_t index) {
    const auto selected = local_music::select_track(tracks_, index);
    if (!selected.accepted || index >= tracks_.size()) {
        return;
    }
    // A track whose format is known unsupported stays on the list: the row
    // already says so, and opening a player page for it would only mislead.
    if (index < playable_.size() && playable_[index] == kFormatUnsupported) {
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
    selected_artist_.clear();
    total_seconds_ = 0;
    shown_status_.clear();
    shown_elapsed_.clear();
    shown_total_.clear();
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

    // Read the tag before the decoder takes the stream, then put the read head
    // back: the decoder starts at byte zero and skips the tag itself.  This is
    // one bounded read on the app task, not per row on the scroll path.
    {
        std::vector<uint8_t> head(kMetadataProbeBytes);
        std::size_t got = 0;
        const auto read_status = stream->read(head.data(), head.size(), got);
        if (read_status != media::AudioStreamStatus::Closed &&
            read_status != media::AudioStreamStatus::InvalidArgument) {
            media::Mp3Tags tags;
            std::size_t tag_bytes = 0;
            if (media::parse_id3v2_tags(head.data(), got, tags, tag_bytes)) {
                // The filename stays the title when the tag has none.
                if (!tags.title.empty()) selected_title_ = tags.title;
                selected_artist_ = tags.artist;
            }
            if (ends_with(tracks_[index].path, ".mp3")) {
                media::Mp3AudioInfo info;
                if (media::parse_mp3_audio_info(head.data(), got, tag_bytes, stream->size(), info)) {
                    total_seconds_ = info.duration_seconds;
                }
            }
        }
        stream->seek(0);
    }

    if (selected_title_ != tracks_[index].title) {
        LvglLockGuard lock;
        if (player_title_ != nullptr) {
            lv_label_set_text(player_title_, selected_title_.c_str());
        }
    }

    current_favourite_ = store_.is_favourite(media::make_track_id(tracks_[index].path));

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

    // The line under the title carries the artist while a track is playing, and
    // the transport state otherwise (buffering, paused, errors).
    const std::string line =
        tick.state == media::PlaybackState::Playing && !selected_artist_.empty() ? selected_artist_
                                                                                : status;
    std::string total = "--:--";
    if (total_seconds_ > 0) {
        char buffer[32] = {};
        std::snprintf(buffer, sizeof(buffer), "%02u:%02u",
                      static_cast<unsigned>(total_seconds_ / 60),
                      static_cast<unsigned>(total_seconds_ % 60));
        total = buffer;
    }

    LvglLockGuard lock;
    refresh_spectrum();

    if (line == shown_status_ && elapsed == shown_elapsed_ && total == shown_total_) {
        return;
    }
    shown_status_ = line;
    shown_elapsed_ = elapsed;
    shown_total_ = total;

    if (progress_ != nullptr && total_seconds_ > 0 && !seeking_) {
        // Driven by the same tick as the elapsed label, so the bar and the clock
        // cannot disagree; left alone while a finger is on it.
        const int32_t range = lv_slider_get_max_value(progress_);
        const uint32_t seconds =
            tick.sample_rate > 0 ? static_cast<uint32_t>(tick.played_frames / tick.sample_rate) : 0;
        const uint32_t clamped = seconds > total_seconds_ ? total_seconds_ : seconds;
        lv_slider_set_value(
            progress_,
            static_cast<int32_t>(static_cast<int64_t>(clamped) * range /
                                 static_cast<int64_t>(total_seconds_)),
            LV_ANIM_OFF);
    }
    lv_label_set_text(player_state_, shown_status_.c_str());
    if (player_elapsed_ != nullptr) {
        lv_label_set_text(player_elapsed_, shown_elapsed_.c_str());
    }
    if (player_total_ != nullptr) {
        lv_label_set_text(player_total_, shown_total_.c_str());
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
