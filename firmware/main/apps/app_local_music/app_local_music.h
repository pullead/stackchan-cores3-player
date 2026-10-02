#pragma once

#include "local_music_presenter.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <media/audio/board_audio_codec_port.h>
#include <media/audio/pcm_tap.h>
#include <media/audio/spectrum_analyzer.h>
#include <media/audio/board_audio_session_port.h>
#include <media/audio/core_s3_speaker_sink.h>
#include <media/local/local_playback_controller.h>
#include <media/local/playback_pump_task.h>
#include <mooncake.h>
#include <smooth_lvgl.hpp>

// Local music player, reproducing the esp32-hifi player's look and behaviour.
//
// The widgets are built with the plain LVGL 9 API rather than the C++ wrapper
// used elsewhere in this firmware: the upstream layout is specified down to
// individual pixel offsets, and matching it is easier against the same API
// shape it was written for.
class AppLocalMusic : public mooncake::AppAbility {
public:
    AppLocalMusic();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;
    // Mooncake's uninstallAllApps() resets the ability manager, which destroys
    // the app without ever calling onClose(); the AI handoff takes that path.
    // The destructor therefore repeats the same teardown.
    ~AppLocalMusic() override;

private:
    // Browse dimensions, mirroring the upstream tab strip.  Songs is backed by
    // the SD scan today; the others need library metadata that is not collected
    // yet, and say so rather than showing a silently empty list.
    enum class Tab : uint8_t { Songs, Artists, Albums, Today, Favourites };

    // A click handler must not destroy the widget it was invoked from: the
    // closure dies with the button and the rest of the handler would run on
    // freed memory.  Clicks therefore only record an intent, which onRunning()
    // carries out once LVGL has finished dispatching the event.
    enum class PendingAction : uint8_t {
        None,
        SelectTrack,
        BackToList,
        SwitchTab,
        PlayPause,
        Prev,
        Next,
        CyclePlayMode,
        ToggleFavourite,
        ToggleCassette,
        Exit,
    };

    // Cycled by the play-mode button, in the upstream order: sequential stops
    // at the end of the list, repeat-all wraps, repeat-one replays the same
    // track, shuffle picks at random.
    enum class PlayMode : uint8_t { Sequential, RepeatAll, RepeatOne, Shuffle };

    void build_list_page();
    void build_player_page();
    void build_status_bar();
    void build_control_bar();
    void build_spectrum(lv_obj_t* card);
    void destroy_page();
    void release_resources();
    void rebind_rows();
    void sync_scroll_slider();
    void apply_pending_action();
    void select_track(std::size_t index);
    void start_track(std::size_t index);
    void step_track(int direction);
    void refresh_player_page();
    void refresh_spectrum();
    void draw_spectrum_cell(int32_t column, int32_t row, bool lit);
    void update_transport_icons();
    std::string playback_status() const;
    std::string playback_status(media::PlaybackState state) const;

    static void on_tab_clicked(lv_event_t* event);
    static void on_row_clicked(lv_event_t* event);
    static void on_back_clicked(lv_event_t* event);
    static void on_exit_clicked(lv_event_t* event);
    static void on_scroll_slider(lv_event_t* event);
    static void on_transport(lv_event_t* event);
    // Left-edge swipe right goes back, the same gesture upstream uses as its
    // universal back action.
    static void on_gesture(lv_event_t* event);
    static void on_press_start(lv_event_t* event);
    static void on_list_scrolled(lv_event_t* event);

    media::SdCardPort sd_card_;
    std::unique_ptr<media::BoardAudioCodecPort> codec_port_;
    // Takes the shared I2S channel from the AI voice path while music plays.
    std::unique_ptr<media::BoardAudioSessionPort> session_port_;
    std::unique_ptr<media::MediaAudioSession> audio_session_;
    std::unique_ptr<media::CoreS3SpeakerSink> speaker_sink_;
    std::unique_ptr<media::LocalPlaybackController> playback_;
    // Decoding runs here, not in onRunning(): the LVGL frame rate must not
    // throttle the decoder.
    std::unique_ptr<media::PlaybackPumpTask> pump_task_;

    std::vector<media::SdTrack> tracks_;

    // The list keeps a fixed pool of row widgets and rebinds them to whatever
    // is on screen.  One LVGL object per track only worked while the scanner
    // capped the library at 64; a real card has hundreds.
    static constexpr int32_t kRowPool = 8;
    std::array<lv_obj_t*, kRowPool> row_pool_{};
    std::array<lv_obj_t*, kRowPool> row_title_{};
    std::array<std::size_t, kRowPool> row_track_{};
    int32_t first_row_ = -1;
    // Guards the slider/scroll feedback loop while one is synced from the other.
    bool syncing_slider_ = false;

    // One root per page; deleting it takes every child with it.
    lv_obj_t* root_ = nullptr;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* scroll_slider_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* player_title_ = nullptr;
    lv_obj_t* player_lyric_ = nullptr;
    lv_obj_t* player_elapsed_ = nullptr;
    lv_obj_t* player_total_ = nullptr;
    lv_obj_t* player_state_ = nullptr;
    lv_obj_t* progress_ = nullptr;
    lv_obj_t* spectrum_ = nullptr;
    lv_color_t* spectrum_buffer_ = nullptr;
    lv_obj_t* play_icon_ = nullptr;
    lv_obj_t* mode_icon_ = nullptr;
    lv_obj_t* favourite_icon_ = nullptr;
    lv_obj_t* status_time_ = nullptr;
    lv_obj_t* status_wifi_ = nullptr;
    lv_obj_t* status_rate_ = nullptr;
    lv_obj_t* status_dac_box_ = nullptr;
    lv_obj_t* status_dac_ = nullptr;
    lv_obj_t* status_codec_ = nullptr;
    lv_obj_t* status_volume_ = nullptr;

    // Spectrum data path: the audio task fills the tap, this page drains it.
    media::PcmTap pcm_tap_;
    media::SpectrumAnalyzer analyzer_;
    std::array<int32_t, media::SpectrumAnalyzer::kColumns> drawn_rows_{};

    std::size_t current_index_ = 0;
    PlayMode play_mode_ = PlayMode::Sequential;
    bool cassette_view_ = false;
    int32_t press_start_x_ = 0;

    Tab tab_ = Tab::Songs;
    bool player_page_ = false;
    PendingAction pending_action_ = PendingAction::None;
    std::size_t pending_index_ = 0;
    Tab pending_tab_ = Tab::Songs;
    std::string selected_title_;
    std::string shown_status_;
    std::string shown_elapsed_;
};
