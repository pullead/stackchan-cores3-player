#include "app_local_music.h"

#include <assets/assets.h>
#include <audio/audio_codec.h>
#include <board.h>
#include <hal/board/cores3_audio_codec.h>
#include <hal/hal.h>
#include <media/audio/volume_policy.h>
#include <media/decoder/hifi_decoder_adapter.h>
#include <media/library/audio_format.h>
#include <media/library/cover_art.h>
#include <media/library/library_store.h>
#include <media/library/mp3_info.h>
#include <mooncake_log.h>

#include <algorithm>
#include <cstdio>

// 本文件只放逻辑：曲库、索引、播放控制与动作分发。界面在
// app_local_music_view.cpp。

namespace {

// How much of a file's head is read for metadata.  ID3v2 tags run from tens of
// bytes to over a hundred kilobytes; this covers the common case, including the
// first MPEG frame, without reading a whole cover art frame off the card.
constexpr std::size_t kMetadataProbeBytes = 16384;

// 开播时为了取封面最多读这么多标签。内嵌封面通常几十到几百 KB；更大的
// 不读，免得一首歌开头卡住太久，此时用默认封面。
constexpr std::size_t kMaxCoverTagBytes = 512 * 1024;
// 标签之后再多读一点，把第一个 MPEG 帧和 Xing 头也带进来，用于算时长。
constexpr std::size_t kFrameProbeBytes = 4096;

// One file per interval while nothing plays.  A head read borrows the display
// bus for a while, so spreading the library out is what keeps the list
// responsive; the whole card is indexed over a few minutes of browsing, one file
// at a time, and never while a track is playing.
constexpr uint32_t kIndexIntervalMs = 250;

// playable_ states: not looked at yet, decodable, and a container this firmware
// has no demuxer for (Ogg, MP4).
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

// 一次 read() 不保证填满：循环到读满、到文件尾或出错为止。
std::size_t read_up_to(media::AudioStream& stream, uint8_t* destination, std::size_t capacity) {
    std::size_t total = 0;
    while (total < capacity) {
        std::size_t got = 0;
        const media::AudioStreamStatus status = stream.read(destination + total, capacity - total, got);
        total += got;
        if (status != media::AudioStreamStatus::Ok || got == 0) break;
    }
    return total;
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

    // 封面缓冲区与默认封面的配色先准备好，界面一建起来就是毛玻璃背景。
    if (allocate_cover_buffers()) {
        prepare_default_cover();
        load_cover(nullptr, 0);
    }

    // browse_tracks() owns the display/SD handoff and therefore runs without an
    // application-level LVGL lock.  It also unmounts before returning.
    tracks_ = sd_card_.browse_tracks();
    // 应用打开期间常驻挂载：索引器和播放器打开文件只做引用计数。
    if (!sd_card_.retain_mount()) {
        mclog::tagWarn(getAppInfo().name, "SD card could not stay mounted: {}", sd_card_.last_error());
    }

    // Nothing is indexed yet: every row falls back to its filename until the
    // indexer has read that file.
    metadata_.assign(tracks_.size(), media::Mp3Tags{});
    indexed_.assign(tracks_.size(), 0);
    playable_.assign(tracks_.size(), 0);
    index_cursor_ = 0;
    last_index_tick_ = lv_tick_get();
    filter_visible_tracks();

    LvglLockGuard lock;
    build_ui();
    // 还没有在播的歌，先停在列表上。
    scroll_to_list(false);
}

void AppLocalMusic::onRunning() {
    // Runs outside LVGL event dispatch, so rebuilding widgets is safe here.
    apply_pending_action();
    advance_when_finished();
    index_one_track_when_idle();
    // 状态文字在拿 LVGL 锁之前算好：出错时它要拿播放控制器的互斥锁读错误
    // 文本，而播放任务关闭文件流时是先持有控制器锁、再借总线（要 LVGL 锁）。
    // 两边顺序相反，持着 LVGL 锁去拿控制器锁就可能互相等死。
    const std::string info = status_line();
    LvglLockGuard lock;
    refresh_view(info);
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
    // 文件流都已关闭，交还常驻挂载。借总线要拿 LVGL 锁，所以在锁外进行。
    sd_card_.release_mount();

    {
        LvglLockGuard lock;
        destroy_ui();
    }
    // 图像描述符引用这些缓冲区，界面删掉之后才能释放。
    free_cover_buffers();
    tracks_.clear();
    metadata_.clear();
    indexed_.clear();
    playable_.clear();
    visible_.clear();
    index_cursor_ = 0;
    has_track_ = false;
    pending_action_ = PendingAction::None;
    selected_title_.clear();
    selected_artist_.clear();
    pump_task_.reset();
    playback_.reset();
    speaker_sink_.reset();
    // Release the audio channel last: the sink hands it back on close, and this
    // only tears down the objects afterwards.
    audio_session_.reset();
    session_port_.reset();
    codec_port_.reset();
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
    return "未知艺术家";
}

void AppLocalMusic::index_one_track_when_idle() {
    if (tracks_.empty() || index_cursor_ >= tracks_.size()) return;

    // An idle controller means the pump task is not going to borrow the SD bus
    // under us -- the handoff is global and not reentrant, so overlapping
    // borrowers would break playback.
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

    // Let the visible rows pick the new title up now rather than on the next
    // scroll.  onRunning() is outside LVGL event dispatch, so taking the lock
    // here is safe.
    LvglLockGuard lock;
    rebind_rows(true);
}

void AppLocalMusic::apply_pending_action() {
    const PendingAction action = pending_action_;
    if (action == PendingAction::None) {
        return;
    }
    pending_action_ = PendingAction::None;

    switch (action) {
        case PendingAction::ShowList: {
            LvglLockGuard lock;
            scroll_to_list(true);
            return;
        }
        case PendingAction::ShowPlayer: {
            LvglLockGuard lock;
            scroll_to_player(true);
            return;
        }
        case PendingAction::ToggleView: {
            LvglLockGuard lock;
            if (list_visible()) {
                scroll_to_player(true);
            } else {
                scroll_to_list(true);
            }
            return;
        }
        case PendingAction::SwitchTab: {
            tab_ = pending_tab_;
            filter_visible_tracks();
            LvglLockGuard lock;
            reload_list();
            return;
        }
        case PendingAction::SelectTrack:
            select_track(pending_index_);
            return;
        case PendingAction::PlayPause: {
            if (!playback_) return;
            if (!has_track_) {
                // 还没选过歌：从当前视图的第一首开始，而不是让按钮没反应。
                if (!visible_.empty()) select_track(visible_.front());
                return;
            }
            if (playback_->tick().state == media::PlaybackState::Playing) {
                playback_->pause();
            } else if (!playback_->resume()) {
                // Nothing to resume: the track has ended or failed, so start it
                // over rather than leaving a dead button.
                start_track(current_index_);
                return;
            }
            LvglLockGuard lock;
            update_controls();
            return;
        }
        case PendingAction::Prev:
            if (has_track_) step_track(-1);
            return;
        case PendingAction::Next:
            if (has_track_) step_track(1);
            return;
        case PendingAction::ToggleShuffle: {
            play_mode_ = play_mode_ == PlayMode::Shuffle ? PlayMode::Sequential : PlayMode::Shuffle;
            LvglLockGuard lock;
            update_controls();
            return;
        }
        case PendingAction::CycleRepeat: {
            // 循环键只在「顺序 → 列表循环 → 单曲循环」之间轮换；随机状态下按它
            // 视为改用列表循环。
            switch (play_mode_) {
                case PlayMode::Sequential:
                    play_mode_ = PlayMode::RepeatAll;
                    break;
                case PlayMode::RepeatAll:
                    play_mode_ = PlayMode::RepeatOne;
                    break;
                case PlayMode::RepeatOne:
                    play_mode_ = PlayMode::Sequential;
                    break;
                case PlayMode::Shuffle:
                    play_mode_ = PlayMode::RepeatAll;
                    break;
            }
            LvglLockGuard lock;
            update_controls();
            return;
        }
        case PendingAction::ToggleFavourite: {
            if (!has_track_ || current_index_ >= tracks_.size()) return;
            current_favourite_ =
                store_.toggle_favourite(media::make_track_id(tracks_[current_index_].path));
            if (!store_.last_write_ok()) {
                // The heart deliberately keeps the old state rather than showing
                // something that would be gone after a reboot.
                mclog::tagWarn(getAppInfo().name, "favourite was not persisted");
            }
            LvglLockGuard lock;
            update_controls();
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
            shown_time_.clear();
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
    // already says so, and opening the player for it would only mislead.
    if (index < playable_.size() && playable_[index] == kFormatUnsupported) {
        // 点击必须有可见反馈：此前这里静默返回，看起来像触摸失灵。
        mclog::tagWarn(getAppInfo().name, "refused unsupported format: {}", tracks_[index].path);
        LvglLockGuard lock;
        if (count_label_ != nullptr) lv_label_set_text(count_label_, "该曲格式不支持");
        return;
    }

    {
        LvglLockGuard lock;
        scroll_to_player(true);
    }
    start_track(index);
}

void AppLocalMusic::start_track(std::size_t index) {
    if (index >= tracks_.size() || !playback_) {
        return;
    }
    current_index_ = index;
    has_track_ = true;
    selected_title_ = tracks_[index].title;
    selected_artist_.clear();
    total_seconds_ = 0;
    shown_info_.clear();
    shown_time_.clear();
    analyzer_.reset();
    pcm_tap_.reset();
    spectrum_frame_ = 0;

    {
        LvglLockGuard lock;
        show_track_info();
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
        if (info_label_ != nullptr) lv_label_set_text(info_label_, "解码器不可用");
        return;
    }

    // Read the tag before the decoder takes the stream, then put the read head
    // back: the decoder starts at byte zero and skips the tag itself.  This is
    // one bounded read on the app task, not per row on the scroll path.
    // 带封面的标签往往远超 16 KB，这时把整个标签读进来（有上限），封面和
    // 标签后第一个 MPEG 帧都在里面。
    std::vector<uint8_t> head(kMetadataProbeBytes);
    head.resize(read_up_to(*stream, head.data(), head.size()));
    const std::size_t tag_size = media::id3v2_tag_size(head.data(), head.size());
    if (tag_size > head.size() && tag_size <= kMaxCoverTagBytes) {
        const std::size_t before = head.size();
        head.resize(tag_size + kFrameProbeBytes);
        head.resize(before + read_up_to(*stream, head.data() + before, head.size() - before));
    }
    {
        media::Mp3Tags tags;
        std::size_t tag_bytes = 0;
        if (media::parse_id3v2_tags(head.data(), head.size(), tags, tag_bytes)) {
            // The filename stays the title when the tag has none.
            if (!tags.title.empty()) selected_title_ = tags.title;
            selected_artist_ = tags.artist;
        }
        if (ends_with(tracks_[index].path, ".mp3")) {
            media::Mp3AudioInfo info;
            if (media::parse_mp3_audio_info(head.data(), head.size(), tag_bytes, stream->size(), info)) {
                total_seconds_ = info.duration_seconds;
            }
        }
    }
    media::EmbeddedPicture picture;
    const bool has_picture = media::find_id3v2_picture(head.data(), head.size(), picture);
    stream->seek(0);

    current_favourite_ = store_.is_favourite(media::make_track_id(tracks_[index].path));

    auto decoder = std::make_unique<media::HifiDecoderAdapter>(std::move(backend));
    playback_->select(selected_title_, std::move(stream), std::move(decoder));
    playback_->set_pcm_tap(&pcm_tap_);
    playback_->start();

    // 先出声再解码封面：标签已经在内存里，解码只占 CPU，不会再借 SD 总线。
    if (has_picture) {
        load_cover(head.data() + picture.offset, picture.length);
    } else {
        load_cover(nullptr, 0);
    }

    LvglLockGuard lock;
    show_track_info();
    update_controls();
    rebind_rows(true);
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
            return has_track_ ? "已停止" : "就绪";
    }
}
