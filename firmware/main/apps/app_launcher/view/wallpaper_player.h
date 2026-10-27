#pragma once

#include <lvgl.h>

#include <cstddef>
#include <cstdint>

namespace view {

// 启动器屏保的动态壁纸：从闲置的 ota_1 分区播放 LMWP 格式的 JPEG 帧序列
// （由 firmware/tools/wallpaper/make_wallpaper.py 生成）。
//
// 解码与显示分开：decode_next() 不碰 LVGL，在 LVGL 锁外调用；present() 与
// build_screen() 只做很快的 LVGL 操作，要求持有 LVGL 锁。两块帧缓冲交替，
// LVGL 正在显示的那块永远不会被解码覆盖。
class WallpaperPlayer {
public:
    WallpaperPlayer() = default;
    ~WallpaperPlayer();

    WallpaperPlayer(const WallpaperPlayer&) = delete;
    WallpaperPlayer& operator=(const WallpaperPlayer&) = delete;

    // 只读分区头判断有没有可播放的壁纸，很便宜，可以每次进屏保前调用。
    static bool available();

    // 映射分区、分配帧缓冲、解出第一帧。失败返回 false，调用方改用 DVD 屏保。
    bool open();
    // 建屏保屏幕并显示第一帧（持有 LVGL 锁）。
    void build_screen();
    // 到了该换帧的时间就把下一帧解码进后台缓冲（不持有 LVGL 锁）。
    // 返回 true 表示有新帧待显示。
    bool decode_next(uint32_t now_ms);
    // 把刚解好的帧换上屏幕（持有 LVGL 锁）。
    void present();
    // 恢复进屏保前的屏幕并删掉自己的屏幕（持有 LVGL 锁）。析构前调用。
    void close_screen();

private:
    bool decode_frame(uint32_t frame, uint16_t* destination);
    void release();

    const uint8_t* data_ = nullptr;   // 映射后的分区内容
    uint32_t mmap_handle_ = 0;
    bool mapped_ = false;
    uint32_t frame_count_ = 0;
    uint32_t frame_interval_ms_ = 66;
    uint32_t total_bytes_ = 0;

    uint16_t* buffers_[2] = {nullptr, nullptr};
    lv_image_dsc_t dscs_[2]{};
    int front_ = 0;         // 屏幕上正在显示的那块
    bool back_ready_ = false;
    uint32_t next_frame_ = 0;
    uint32_t last_frame_ms_ = 0;
    void* decoder_ = nullptr;

    lv_obj_t* screen_ = nullptr;
    lv_obj_t* image_ = nullptr;
    lv_obj_t* previous_screen_ = nullptr;
};

}  // namespace view
