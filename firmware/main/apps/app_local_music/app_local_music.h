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

class AppLocalMusic : public mooncake::AppAbility {
public:
    AppLocalMusic();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    // A click handler must not destroy the widget it was invoked from: the
    // closure dies with the button and the rest of the handler would run on
    // freed memory.  Clicks therefore only record an intent, which onRunning()
    // carries out once LVGL has finished dispatching the event.
    enum class PendingAction : uint8_t { None, SelectTrack, BackToList };

    void create_view();
    void apply_pending_action();
    void select_track(std::size_t index);
    void render(const local_music::BrowseView& view);
    void render_playback(const std::string& title, const std::string& status);
    void show_list();
    std::string playback_status() const;

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

    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> panel_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> title_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> heading_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> detail_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Container> track_list_;
    std::vector<std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button>> track_rows_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> playback_name_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> playback_progress_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Label> playback_note_;
    std::vector<media::SdTrack> tracks_;
    std::unique_ptr<smooth_ui_toolkit::lvgl_cpp::Button> back_;
    bool playback_view_ = false;
    std::string selected_title_;
    std::string shown_status_;
    PendingAction pending_action_ = PendingAction::None;
    std::size_t pending_index_ = 0;
};
