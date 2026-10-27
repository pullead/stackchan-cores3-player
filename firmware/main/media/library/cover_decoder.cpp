#include "media/library/cover_decoder.h"

#include "media/library/cover_art.h"

#include <algorithm>

#ifdef ESP_PLATFORM
#include <esp_jpeg_dec.h>
#include <esp_log.h>
#endif

namespace media {

#ifdef ESP_PLATFORM
namespace {

constexpr char kTag[] = "CoverDecoder";

int round_up_8(int value) { return (value + 7) / 8 * 8; }

// 解析头部拿到原图尺寸；解码器句柄要按缩放参数重新创建，所以单独一步。
bool read_dimensions(const std::uint8_t* data, std::size_t length, int& width, int& height) {
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    jpeg_dec_handle_t decoder = nullptr;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) return false;
    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<std::uint8_t*>(data);
    io.inbuf_len = static_cast<int>(length);
    jpeg_dec_header_info_t info = {};
    const bool ok = jpeg_dec_parse_header(decoder, &io, &info) == JPEG_ERR_OK;
    jpeg_dec_close(decoder);
    width = info.width;
    height = info.height;
    return ok && width > 0 && height > 0;
}

}  // namespace

bool decode_cover_jpeg(const std::uint8_t* data, std::size_t length, int size,
                       std::vector<std::uint8_t>& rgb_out) {
    if (data == nullptr || length == 0 || size <= 0) return false;
    int width = 0;
    int height = 0;
    if (!read_dimensions(data, length, width, height)) {
        ESP_LOGW(kTag, "cover header not decodable");
        return false;
    }

    // 解码器的缩放要求目标宽高是 8 的倍数，且最多缩到 1/8。取「不小于目标、
    // 也不小于 1/8 原图」的最小 8 倍数；比原图还大就不缩放。
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = JPEG_PIXEL_FORMAT_RGB888;
    int out_w = width;
    int out_h = height;
    const int scaled_w = std::max(round_up_8(size), round_up_8((width + 7) / 8));
    if (scaled_w < width) {
        const int scaled_h = round_up_8(std::max(1, height * scaled_w / width));
        if (scaled_h < height) {
            config.scale.width = static_cast<uint16_t>(scaled_w);
            config.scale.height = static_cast<uint16_t>(scaled_h);
            out_w = scaled_w;
            out_h = scaled_h;
        }
    }

    jpeg_dec_handle_t decoder = nullptr;
    if (jpeg_dec_open(&config, &decoder) != JPEG_ERR_OK) return false;
    jpeg_dec_io_t io = {};
    io.inbuf = const_cast<std::uint8_t*>(data);
    io.inbuf_len = static_cast<int>(length);
    jpeg_dec_header_info_t info = {};
    if (jpeg_dec_parse_header(decoder, &io, &info) != JPEG_ERR_OK) {
        jpeg_dec_close(decoder);
        return false;
    }

    const std::size_t out_bytes = static_cast<std::size_t>(out_w) * out_h * 3;
    auto* pixels = static_cast<std::uint8_t*>(jpeg_calloc_align(out_bytes, 16));
    if (pixels == nullptr) {
        jpeg_dec_close(decoder);
        return false;
    }
    io.outbuf = pixels;
    const jpeg_error_t result = jpeg_dec_process(decoder, &io);
    jpeg_dec_close(decoder);
    if (result != JPEG_ERR_OK) {
        ESP_LOGW(kTag, "cover decode failed (%d), %dx%d", static_cast<int>(result), width, height);
        jpeg_free_align(pixels);
        return false;
    }

    rgb_out.resize(static_cast<std::size_t>(size) * size * 3);
    downsample_rgb888(pixels, out_w, out_h, rgb_out.data(), size, size);
    jpeg_free_align(pixels);
    ESP_LOGI(kTag, "cover %dx%d decoded via %dx%d", width, height, out_w, out_h);
    return true;
}
#else
bool decode_cover_jpeg(const std::uint8_t*, std::size_t, int, std::vector<std::uint8_t>&) {
    return false;
}
#endif

}  // namespace media
