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
#include <media/library/cover_art.h>
#include <media/library/library_store.h>
#include <media/library/mp3_info.h>
#include <media/library/play_mode.h>
#include <media/local/local_playback_controller.h>
#include <media/local/playback_pump_task.h>
#include <mooncake.h>
#include <smooth_lvgl.hpp>

// 本地音乐播放器。
//
// 界面改写自 LVGL 官方音乐播放器 demo（MIT），见 app_local_music_view.cpp：
// 白色播放页叠在深色曲目列表之上，上滑播放页露出列表。逻辑（曲库、索引、
// 播放控制、自动切歌）在 app_local_music.cpp，界面构建与刷新在 view 文件里。
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
    // 列表的两个视图：全部曲目与收藏。原先的歌手/专辑/今日三个分类一直只是
    // 占位提示，换界面时一并去掉。
    enum class Tab : uint8_t { Songs, Favourites };

    // A click handler must not destroy the widget it was invoked from: the
    // closure dies with the button and the rest of the handler would run on
    // freed memory.  Clicks therefore only record an intent, which onRunning()
    // carries out once LVGL has finished dispatching the event.
    enum class PendingAction : uint8_t {
        None,
        SelectTrack,
        ShowList,
        ShowPlayer,
        ToggleView,
        SwitchTab,
        PlayPause,
        Prev,
        Next,
        ToggleShuffle,
        CycleRepeat,
        ToggleFavourite,
        Exit,
        Seek,
    };

    // The values and their semantics live in the library module so the finish
    // policy can be host tested.
    using PlayMode = media::PlayMode;

    // —— 逻辑（app_local_music.cpp）——
    void release_resources();
    void filter_visible_tracks();
    // Reads one track's tag per call, and only while nothing is playing: the
    // pump task and this would otherwise borrow the same SD/display bus, which
    // is not reentrant.
    void index_one_track_when_idle();
    // The cached title/artist when the track has been indexed, the filename and
    // a placeholder otherwise.
    const char* row_title_for(std::size_t index) const;
    const char* row_artist_for(std::size_t index) const;
    void apply_pending_action();
    // What happens when the current track runs out: advance, repeat or stop,
    // according to the play mode and where the track sits in the current view.
    void advance_when_finished();
    void select_track(std::size_t index);
    void start_track(std::size_t index);
    // Returns false when there is nothing in the view to move to.
    bool step_track(int direction);
    std::string playback_status(media::PlaybackState state) const;

    // —— 界面（app_local_music_view.cpp）——
    // 以下函数都要求调用方已持有 LVGL 锁。
    void build_ui();
    void build_list_panel();
    void build_player_panel();
    void destroy_ui();
    // 视图切换后重排列表：标签样式、计数、内容高度、回到顶部。
    void reload_list();
    void rebind_rows(bool force);
    void show_track_info();
    void update_controls();
    void refresh_view();
    void refresh_spectrum();
    void scroll_to_list(bool animate);
    void scroll_to_player(bool animate);
    bool list_visible() const;
    // 按 theme_ 给文字、图标、进度条与频谱上色（要求持有 LVGL 锁）。
    void apply_theme();

    // —— 封面与毛玻璃背景（app_local_music_view.cpp）——
    bool allocate_cover_buffers();
    void free_cover_buffers();
    // 默认封面（无内嵌图时使用）的 RGB888，onOpen 时准备一次。
    void prepare_default_cover();
    // 由封面 JPEG（nullptr 表示用默认封面）生成圆形封面、背景与配色，再在
    // LVGL 锁内换上。计算在调用线程进行，不持有 LVGL 锁。
    void load_cover(const uint8_t* jpeg, std::size_t length);

    static void on_row_clicked(lv_event_t* event);
    static void on_list_scrolled(lv_event_t* event);
    // 按钮的 user_data 是要执行的 PendingAction。
    static void on_action_clicked(lv_event_t* event);
    static void on_tab_clicked(lv_event_t* event);
    // The progress bar.  A drag is only turned into a seek when the finger
    // lifts, so the value being dragged is never overwritten by the clock.
    static void on_progress_event(lv_event_t* event);
    static void on_spectrum_draw(lv_event_t* event);
    // 封面左右滑动切歌，沿用 demo 的交互。
    static void on_cover_gesture(lv_event_t* event);
    static void on_main_scroll_end(lv_event_t* event);
    // 挂在输入设备上而不是某个控件上：按下事件不会从子控件冒泡，左边缘
    // 手势需要知道手指落在哪里。
    static void on_indev_event(lv_event_t* event);

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
    // Metadata read for the list, filled in progressively by the indexer.  The
    // vectors are parallel to tracks_ on purpose: the list needs a cheap lookup
    // by index, not by path.
    std::vector<media::Mp3Tags> metadata_;
    std::vector<uint8_t> indexed_;
    // 0 unknown, 1 decodable, 2 a container this firmware cannot demux.  Filled
    // by the indexer from the same head bytes it already reads for metadata.
    std::vector<uint8_t> playable_;
    std::size_t index_cursor_ = 0;
    uint32_t last_index_tick_ = 0;

    // The list keeps a fixed pool of row widgets and rebinds them to whatever
    // is on screen; one LVGL object per track does not scale to a real card.
    static constexpr int32_t kRowPool = 7;
    std::array<lv_obj_t*, kRowPool> row_pool_{};
    std::array<lv_obj_t*, kRowPool> row_icon_{};
    std::array<lv_obj_t*, kRowPool> row_title_{};
    std::array<lv_obj_t*, kRowPool> row_artist_{};
    // What each slot shows: a position in visible_, not a track index, because
    // the view can be a subset of the library.
    std::array<std::size_t, kRowPool> row_position_{};
    // The track indices this list is showing: everything for Songs, the
    // favourites for ★.  Next/prev follow the view that was in use.
    std::vector<std::size_t> visible_;
    int32_t first_row_ = -1;

    // 控件。根对象删除时整棵树随之释放，其余指针只是引用。
    lv_obj_t* root_ = nullptr;
    lv_obj_t* list_panel_ = nullptr;
    lv_obj_t* list_ = nullptr;
    lv_obj_t* list_spacer_ = nullptr;
    lv_obj_t* list_empty_ = nullptr;
    lv_obj_t* count_label_ = nullptr;
    std::array<lv_obj_t*, 2> tab_labels_{};
    lv_obj_t* main_cont_ = nullptr;
    lv_obj_t* player_ = nullptr;
    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* artist_label_ = nullptr;
    lv_obj_t* info_label_ = nullptr;
    lv_obj_t* favourite_icon_ = nullptr;
    lv_obj_t* format_label_ = nullptr;
    lv_obj_t* background_ = nullptr;
    lv_obj_t* shuffle_icon_ = nullptr;
    lv_obj_t* prev_icon_ = nullptr;
    lv_obj_t* next_icon_ = nullptr;
    lv_obj_t* play_icon_ = nullptr;
    lv_obj_t* loop_icon_ = nullptr;
    lv_obj_t* loop_badge_ = nullptr;
    lv_obj_t* progress_ = nullptr;
    // 进度条两端：已播放与总时长。
    lv_obj_t* time_label_ = nullptr;
    lv_obj_t* total_label_ = nullptr;
    lv_obj_t* spectrum_obj_ = nullptr;
    lv_obj_t* cover_ = nullptr;
    lv_obj_t* handle_label_ = nullptr;
    lv_indev_t* gesture_indev_ = nullptr;

    // Spectrum data path: the audio task fills the tap, this page drains it.
    media::PcmTap pcm_tap_;
    media::SpectrumAnalyzer analyzer_;
    // demo 的环形频谱只用 4 个频段（低、中低、中、中高），量程约 0..80。
    std::array<int32_t, 4> bands_{};
    uint32_t spectrum_frame_ = 0;
    uint32_t bar_rot_ = 0;
    uint32_t bar_ofs_ = 0;
    uint32_t bass_count_ = 0;
    uint32_t last_bass_frame_ = 0;
    uint32_t last_spectrum_tick_ = 0;
    bool spectrum_resting_ = true;

    // 封面与背景。显示用的缓冲区由 LVGL 图像描述符引用；新封面先算进 scratch，
    // 再在锁内拷贝过去，避免 LVGL 渲染到一半的数据。
    static constexpr int kCoverPixels = 108;
    static constexpr int kBackgroundWidth = 320;
    static constexpr int kBackgroundHeight = 240;
    static constexpr int kThemeGrid = 6;
    uint8_t* cover_pixels_ = nullptr;
    uint8_t* cover_scratch_ = nullptr;
    uint16_t* background_pixels_ = nullptr;
    uint16_t* background_scratch_ = nullptr;
    lv_image_dsc_t cover_dsc_{};
    lv_image_dsc_t background_dsc_{};
    std::vector<uint8_t> default_cover_rgb_;
    media::CoverTheme theme_;
    std::array<lv_color_t, 3> bar_colours_{};

    std::size_t current_index_ = 0;
    bool has_track_ = false;
    PlayMode play_mode_ = PlayMode::Sequential;
    int32_t press_start_x_ = 0;
    float pending_seek_fraction_ = 0.0f;
    // True while a finger is on the progress bar.
    bool seeking_ = false;

    Tab tab_ = Tab::Songs;
    PendingAction pending_action_ = PendingAction::None;
    std::size_t pending_index_ = 0;
    Tab pending_tab_ = Tab::Songs;
    std::string selected_title_;
    // Favourite state of the selected track, kept so the heart can be redrawn
    // without a lookup, and the NVS-backed store it comes from.
    media::LibraryStore store_;
    bool current_favourite_ = false;
    // Filled from the track's own tag when it is opened.
    std::string selected_artist_;
    uint32_t total_seconds_ = 0;
    // 已绘制的状态，未变化时跳过 LVGL 调用。
    std::string shown_info_;
    std::string shown_time_;
    std::string shown_format_;
    media::PlaybackState shown_state_ = media::PlaybackState::Idle;
    bool shown_list_visible_ = true;
    std::size_t shown_highlight_ = SIZE_MAX;
    bool shown_highlight_playing_ = false;
};
