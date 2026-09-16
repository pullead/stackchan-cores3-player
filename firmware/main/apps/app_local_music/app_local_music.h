#pragma once

#include "local_music_presenter.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <media/audio/board_audio_codec_port.h>
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

private:
    // Browse dimensions, mirroring the upstream tab strip.  Songs is backed by
    // the SD scan today; the others need library metadata that is not collected
    // yet, and say so rather than showing a silently empty list.
    enum class Tab : uint8_t { Songs, Artists, Albums, Today, Favourites };

    // A click handler must not destroy the widget it was invoked from: the
    // closure dies with the button and the rest of the handler would run on
    // freed memory.  Clicks therefore only record an intent, which onRunning()
    // carries out once LVGL has finished dispatching the event.
    enum class PendingAction : uint8_t { None, SelectTrack, BackToList, SwitchTab };

    void build_list_page();
    void build_player_page();
    void destroy_page();
    void apply_pending_action();
    void select_track(std::size_t index);
    void refresh_player_page();
    std::string playback_status() const;
    std::string playback_status(media::PlaybackState state) const;

    static void on_tab_clicked(lv_event_t* event);
    static void on_row_clicked(lv_event_t* event);
    static void on_back_clicked(lv_event_t* event);
    static void on_scroll_slider(lv_event_t* event);

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

    // One root per page; deleting it takes every child with it.
    lv_obj_t* root_ = nullptr;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* scroll_slider_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* player_title_ = nullptr;
    lv_obj_t* player_elapsed_ = nullptr;
    lv_obj_t* player_state_ = nullptr;

    Tab tab_ = Tab::Songs;
    bool player_page_ = false;
    PendingAction pending_action_ = PendingAction::None;
    std::size_t pending_index_ = 0;
    Tab pending_tab_ = Tab::Songs;
    std::string selected_title_;
    std::string shown_status_;
    std::string shown_elapsed_;
};
