#include "wallpaper_player.h"

#include <cstring>

#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#include <esp_jpeg_dec.h>
#include <esp_log.h>
#include <esp_partition.h>
#endif

namespace view {

#ifdef ESP_PLATFORM
namespace {

constexpr char kTag[] = "Wallpaper";
constexpr uint16_t kWidth = 320;
constexpr uint16_t kHeight = 240;
constexpr std::size_t kFrameBytes = static_cast<std::size_t>(kWidth) * kHeight * 2;

// LMWP 文件头，布局见 tools/wallpaper/make_wallpaper.py。
struct Header {
    char magic[4];
    uint16_t version;
    uint16_t width;
    uint16_t height;
    uint16_t fps_x100;
    uint32_t frame_count;
    uint32_t index_offset;
    uint32_t total_bytes;
    uint32_t reserved[2];
};
static_assert(sizeof(Header) == 32, "LMWP header is 32 bytes");

// 壁纸借用闲置的 ota_1 应用分区。本固件不做双分区 OTA；万一 otadata 指向
// 这里，引导程序校验镜像失败也会回到 ota_0，不会启动到壁纸数据上。
const esp_partition_t* wallpaper_partition() {
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, nullptr);
}

bool read_header(const esp_partition_t* partition, Header& header) {
    if (partition == nullptr) return false;
    if (esp_partition_read(partition, 0, &header, sizeof(header)) != ESP_OK) return false;
    if (std::memcmp(header.magic, "LMWP", 4) != 0 || header.version != 1) return false;
    if (header.width != kWidth || header.height != kHeight) return false;
    if (header.frame_count == 0 || header.fps_x100 == 0 || header.index_offset != sizeof(Header)) {
        return false;
    }
    if (header.total_bytes > partition->size ||
        header.index_offset + header.frame_count * 8ULL > header.total_bytes) {
        return false;
    }
    return true;
}

void* open_decoder() {
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    // LVGL 的 RGB565 是本机小端；显示驱动在送屏时自己处理字节序。
    config.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t decoder = nullptr;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) return nullptr;
    return decoder;
}

}  // namespace

bool WallpaperPlayer::available() {
    Header header{};
    return read_header(wallpaper_partition(), header);
}

bool WallpaperPlayer::open() {
    const esp_partition_t* partition = wallpaper_partition();
    Header header{};
    if (!read_header(partition, header)) return false;

    // 只映射实际用到的部分，不占用整个分区的地址空间。
    const void* mapped = nullptr;
    esp_partition_mmap_handle_t handle = 0;
    if (esp_partition_mmap(partition, 0, header.total_bytes, ESP_PARTITION_MMAP_DATA, &mapped, &handle) !=
        ESP_OK) {
        ESP_LOGW(kTag, "cannot map the wallpaper partition");
        return false;
    }
    data_ = static_cast<const uint8_t*>(mapped);
    mmap_handle_ = handle;
    mapped_ = true;
    frame_count_ = header.frame_count;
    total_bytes_ = header.total_bytes;
    frame_interval_ms_ = 100000u / header.fps_x100;
    if (frame_interval_ms_ == 0) frame_interval_ms_ = 1;

    for (int i = 0; i < 2; ++i) {
        // 解码器要求输出缓冲 16 字节对齐。
        buffers_[i] = static_cast<uint16_t*>(
            heap_caps_aligned_alloc(16, kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buffers_[i] == nullptr) {
            ESP_LOGW(kTag, "no memory for wallpaper frames");
            release();
            return false;
        }
        dscs_[i] = {};
        dscs_[i].header.magic = LV_IMAGE_HEADER_MAGIC;
        dscs_[i].header.cf = LV_COLOR_FORMAT_RGB565;
        dscs_[i].header.w = kWidth;
        dscs_[i].header.h = kHeight;
        dscs_[i].header.stride = kWidth * 2;
        dscs_[i].data_size = kFrameBytes;
        dscs_[i].data = reinterpret_cast<const uint8_t*>(buffers_[i]);
    }

    decoder_ = open_decoder();
    if (decoder_ == nullptr || !decode_frame(0, buffers_[0])) {
        ESP_LOGW(kTag, "first wallpaper frame does not decode");
        release();
        return false;
    }
    front_ = 0;
    back_ready_ = false;
    next_frame_ = frame_count_ > 1 ? 1 : 0;
    last_frame_ms_ = 0;
    ESP_LOGI(kTag, "playing %u frames at %u ms per frame (%u bytes)", static_cast<unsigned>(frame_count_),
             static_cast<unsigned>(frame_interval_ms_), static_cast<unsigned>(total_bytes_));
    return true;
}

bool WallpaperPlayer::decode_frame(uint32_t frame, uint16_t* destination) {
    if (frame >= frame_count_ || data_ == nullptr || destination == nullptr) return false;
    uint32_t entry[2];
    std::memcpy(entry, data_ + sizeof(Header) + frame * 8, sizeof(entry));
    const uint32_t offset = entry[0];
    const uint32_t length = entry[1];
    if (length == 0 || offset < sizeof(Header) || offset + static_cast<uint64_t>(length) > total_bytes_) {
        return false;
    }

    for (int attempt = 0; attempt < 2; ++attempt) {
        if (decoder_ == nullptr) decoder_ = open_decoder();
        if (decoder_ == nullptr) return false;
        auto decoder = static_cast<jpeg_dec_handle_t>(decoder_);
        jpeg_dec_io_t io = {};
        // 解码器只读输入；接口签名不带 const。
        io.inbuf = const_cast<uint8_t*>(data_ + offset);
        io.inbuf_len = static_cast<int>(length);
        jpeg_dec_header_info_t info = {};
        if (jpeg_dec_parse_header(decoder, &io, &info) == JPEG_ERR_OK && info.width == kWidth &&
            info.height == kHeight) {
            io.outbuf = reinterpret_cast<uint8_t*>(destination);
            if (jpeg_dec_process(decoder, &io) == JPEG_ERR_OK) return true;
        }
        // 句柄复用失败时重开一次再试，避免一帧坏数据让后面全部失败。
        jpeg_dec_close(decoder);
        decoder_ = nullptr;
    }
    return false;
}

bool WallpaperPlayer::decode_next(uint32_t now_ms) {
    if (decoder_ == nullptr || frame_count_ < 2) return false;
    if (back_ready_) return true;  // 上一帧还没显示
    if (now_ms - last_frame_ms_ < frame_interval_ms_) return false;
    last_frame_ms_ = now_ms;
    const int back = 1 - front_;
    const bool ok = decode_frame(next_frame_, buffers_[back]);
    next_frame_ = (next_frame_ + 1) % frame_count_;
    back_ready_ = ok;
    return ok;
}

void WallpaperPlayer::release() {
    if (decoder_ != nullptr) {
        jpeg_dec_close(static_cast<jpeg_dec_handle_t>(decoder_));
        decoder_ = nullptr;
    }
    for (auto*& buffer : buffers_) {
        if (buffer != nullptr) {
            heap_caps_free(buffer);
            buffer = nullptr;
        }
    }
    if (mapped_) {
        esp_partition_munmap(static_cast<esp_partition_mmap_handle_t>(mmap_handle_));
        mapped_ = false;
    }
    data_ = nullptr;
}

#else  // 主机构建没有分区与解码器。

bool WallpaperPlayer::available() { return false; }
bool WallpaperPlayer::open() { return false; }
bool WallpaperPlayer::decode_frame(uint32_t, uint16_t*) { return false; }
bool WallpaperPlayer::decode_next(uint32_t) { return false; }
void WallpaperPlayer::release() {}

#endif

WallpaperPlayer::~WallpaperPlayer() {
    // 图像控件引用着帧缓冲，必须先撤下屏幕再释放。
    close_screen();
    release();
}

void WallpaperPlayer::build_screen() {
    if (screen_ != nullptr || buffers_[0] == nullptr) return;
    previous_screen_ = lv_screen_active();
    screen_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(screen_, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen_, 0, 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    image_ = lv_image_create(screen_);
    lv_image_set_src(image_, &dscs_[front_]);
    lv_obj_set_pos(image_, 0, 0);
    lv_screen_load(screen_);
}

void WallpaperPlayer::present() {
    if (!back_ready_ || image_ == nullptr) return;
    front_ = 1 - front_;
    // 换成另一块缓冲的描述符：地址不同，LVGL 会重新读图并重绘。
    lv_image_set_src(image_, &dscs_[front_]);
    back_ready_ = false;
}

void WallpaperPlayer::close_screen() {
    if (screen_ == nullptr) return;
    if (previous_screen_ != nullptr) lv_screen_load(previous_screen_);
    lv_obj_delete(screen_);
    screen_ = nullptr;
    image_ = nullptr;
    previous_screen_ = nullptr;
}

}  // namespace view
