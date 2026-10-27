#include "media/library/cover_art.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace media {
namespace {

std::size_t synchsafe32(const std::uint8_t* bytes) {
    return (static_cast<std::size_t>(bytes[0] & 0x7F) << 21) |
           (static_cast<std::size_t>(bytes[1] & 0x7F) << 14) |
           (static_cast<std::size_t>(bytes[2] & 0x7F) << 7) |
           static_cast<std::size_t>(bytes[3] & 0x7F);
}

std::size_t big_endian32(const std::uint8_t* bytes) {
    return (static_cast<std::size_t>(bytes[0]) << 24) | (static_cast<std::size_t>(bytes[1]) << 16) |
           (static_cast<std::size_t>(bytes[2]) << 8) | static_cast<std::size_t>(bytes[3]);
}

std::size_t big_endian24(const std::uint8_t* bytes) {
    return (static_cast<std::size_t>(bytes[0]) << 16) | (static_cast<std::size_t>(bytes[1]) << 8) |
           static_cast<std::size_t>(bytes[2]);
}

// 跳过一段以 NUL 结尾的描述文字，返回其后的位置；找不到结尾返回 end。
// UTF-16（编码 1、2）以对齐的双字节 NUL 结尾，其余编码以单字节 NUL 结尾。
std::size_t skip_terminated(const std::uint8_t* data, std::size_t pos, std::size_t end,
                            std::uint8_t encoding) {
    if (encoding == 1 || encoding == 2) {
        for (std::size_t i = pos; i + 1 < end; i += 2) {
            if (data[i] == 0 && data[i + 1] == 0) return i + 2;
        }
        return end;
    }
    for (std::size_t i = pos; i < end; ++i) {
        if (data[i] == 0) return i + 1;
    }
    return end;
}

bool is_jpeg(const std::uint8_t* data, std::size_t length) {
    return length >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF;
}

int luminance(int r, int g, int b) {
    // Rec.709 近似，整数运算。
    return (r * 54 + g * 183 + b * 19) >> 8;
}

std::uint8_t clamp_byte(int value) {
    return static_cast<std::uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}

}  // namespace

std::size_t id3v2_tag_size(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size < 10) return 0;
    if (data[0] != 'I' || data[1] != 'D' || data[2] != '3') return 0;
    std::size_t total = 10 + synchsafe32(data + 6);
    if (data[3] == 4 && (data[5] & 0x10) != 0) total += 10;  // v2.4 footer
    return total;
}

bool find_id3v2_picture(const std::uint8_t* data, std::size_t size, EmbeddedPicture& out) {
    out = {};
    if (data == nullptr || size < 10) return false;
    if (data[0] != 'I' || data[1] != 'D' || data[2] != '3') return false;

    const std::uint8_t major = data[3];
    if (major < 2 || major > 4) return false;
    const std::uint8_t flags = data[5];
    // 反同步改写了标签内容，还原它是另一个解析器的工作；与 mp3_info 一致地放弃。
    if ((flags & 0x80) != 0) return false;

    const std::size_t end = std::min(size, 10 + synchsafe32(data + 6));
    std::size_t pos = 10;
    if ((flags & 0x40) != 0) {
        // v2.2 的这一位表示压缩，无法读取；v2.3/2.4 表示扩展头，跳过即可。
        if (major == 2 || pos + 4 > end) return false;
        pos += major == 3 ? 4 + big_endian32(data + pos) : synchsafe32(data + pos);
    }

    const std::size_t header_size = major == 2 ? 6 : 10;
    bool have_fallback = false;
    EmbeddedPicture fallback;
    while (pos + header_size <= end) {
        const std::uint8_t* frame = data + pos;
        if (frame[0] == 0) break;  // 之后是填充

        std::size_t frame_size = 0;
        if (major == 2) {
            frame_size = big_endian24(frame + 3);
        } else {
            frame_size = major == 4 ? synchsafe32(frame + 4) : big_endian32(frame + 4);
        }
        if (frame_size == 0) break;
        const std::size_t body = pos + header_size;
        if (body + frame_size > end) break;  // 超出了读到的字节
        const std::size_t body_end = body + frame_size;
        pos = body_end;

        std::size_t cursor = body;
        std::uint8_t picture_type = 0;
        if (major == 2) {
            if (frame[0] != 'P' || frame[1] != 'I' || frame[2] != 'C') continue;
            // 编码(1) 图片格式(3，如 "JPG") 类型(1) 描述 图片数据
            if (frame_size < 6) continue;
            const std::uint8_t encoding = data[cursor];
            cursor += 4;
            picture_type = data[cursor++];
            cursor = skip_terminated(data, cursor, body_end, encoding);
        } else {
            if (frame[0] != 'A' || frame[1] != 'P' || frame[2] != 'I' || frame[3] != 'C') continue;
            // 压缩、加密、分组、帧级反同步都无法直接读取。
            if (frame[9] != 0) continue;
            if (frame_size < 4) continue;
            // 编码(1) MIME(以 NUL 结尾的 Latin-1) 类型(1) 描述 图片数据
            const std::uint8_t encoding = data[cursor++];
            cursor = skip_terminated(data, cursor, body_end, 0);
            if (cursor >= body_end) continue;
            picture_type = data[cursor++];
            cursor = skip_terminated(data, cursor, body_end, encoding);
        }
        if (cursor >= body_end) continue;

        // 以内容判断格式而不是 MIME：不少标签把 MIME 写成 "image/jpg" 或留空。
        const std::size_t length = body_end - cursor;
        if (!is_jpeg(data + cursor, length)) continue;
        if (picture_type == 3) {
            out.offset = cursor;
            out.length = length;
            return true;
        }
        if (!have_fallback) {
            fallback.offset = cursor;
            fallback.length = length;
            have_fallback = true;
        }
    }
    if (have_fallback) out = fallback;
    return have_fallback;
}

void downsample_rgb888(const std::uint8_t* src, int src_w, int src_h, std::uint8_t* dst, int dst_w,
                       int dst_h) {
    if (src == nullptr || dst == nullptr || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
    }
    // 居中裁成正方形：封面偶尔不是正方形，拉伸会让人物变形。
    const int side = std::min(src_w, src_h);
    const int x0 = (src_w - side) / 2;
    const int y0 = (src_h - side) / 2;
    for (int dy = 0; dy < dst_h; ++dy) {
        int ya = y0 + dy * side / dst_h;
        int yb = y0 + (dy + 1) * side / dst_h;
        if (yb <= ya) yb = ya + 1;
        for (int dx = 0; dx < dst_w; ++dx) {
            int xa = x0 + dx * side / dst_w;
            int xb = x0 + (dx + 1) * side / dst_w;
            if (xb <= xa) xb = xa + 1;
            std::uint32_t sum[3] = {0, 0, 0};
            std::uint32_t count = 0;
            for (int y = ya; y < yb && y < src_h; ++y) {
                const std::uint8_t* row = src + (static_cast<std::size_t>(y) * src_w + xa) * 3;
                for (int x = xa; x < xb && x < src_w; ++x) {
                    sum[0] += row[0];
                    sum[1] += row[1];
                    sum[2] += row[2];
                    row += 3;
                    ++count;
                }
            }
            std::uint8_t* out = dst + (static_cast<std::size_t>(dy) * dst_w + dx) * 3;
            for (int c = 0; c < 3; ++c) {
                out[c] = static_cast<std::uint8_t>(count > 0 ? sum[c] / count : 0);
            }
        }
    }
}

void make_round_cover(const std::uint8_t* rgb, int size, std::uint8_t* argb_out, int ring_width) {
    if (rgb == nullptr || argb_out == nullptr || size <= 0) return;
    const float centre = (static_cast<float>(size) - 1.0f) * 0.5f;
    const float radius = static_cast<float>(size) * 0.5f;
    const float inner_radius = radius - static_cast<float>(std::max(ring_width, 0));
    auto coverage = [](float value) {
        return value <= 0.0f ? 0.0f : (value >= 1.0f ? 1.0f : value);
    };
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            const float dx = static_cast<float>(x) - centre;
            const float dy = static_cast<float>(y) - centre;
            const float distance = std::sqrt(dx * dx + dy * dy);
            const float outer = coverage(radius - distance + 0.5f);
            // 白边占比：内边界以外为 1，内边界以内为 0，跨边界的像素按比例混合。
            const float ring = ring_width > 0 ? coverage(distance - inner_radius + 0.5f) : 0.0f;
            const std::uint8_t* in = rgb + (static_cast<std::size_t>(y) * size + x) * 3;
            std::uint8_t* out = argb_out + (static_cast<std::size_t>(y) * size + x) * 4;
            auto blend = [ring](std::uint8_t channel) {
                return static_cast<std::uint8_t>(channel + (255.0f - channel) * ring + 0.5f);
            };
            // LVGL 的 lv_color32_t 内存顺序是 B, G, R, A。
            out[0] = blend(in[2]);
            out[1] = blend(in[1]);
            out[2] = blend(in[0]);
            out[3] = static_cast<std::uint8_t>(outer * 255.0f);
        }
    }
}

CoverTheme analyse_cover(const std::uint8_t* grid, int grid_w, int grid_h) {
    CoverTheme theme;
    const int cells = grid_w * grid_h;
    if (grid == nullptr || cells <= 0) return theme;

    int luma_sum = 0;
    int best_score = -1;
    Rgb best{};
    for (int i = 0; i < cells; ++i) {
        const int r = grid[i * 3];
        const int g = grid[i * 3 + 1];
        const int b = grid[i * 3 + 2];
        luma_sum += luminance(r, g, b);
        const int high = std::max(r, std::max(g, b));
        const int low = std::min(r, std::min(g, b));
        // 鲜艳程度：饱和度为主，过暗的颜色降权（近黑色的「高饱和」没有意义）。
        const int score = (high - low) * (64 + high);
        if (score > best_score) {
            best_score = score;
            best = {static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(b)};
        }
    }
    const int average_luma = luma_sum / cells;
    theme.dark = average_luma < 105;

    const int high = std::max(best.r, std::max(best.g, best.b));
    const int low = std::min(best.r, std::min(best.g, best.b));
    if (high - low < 40) {
        // 几乎是灰度封面：用中性的蓝灰色，避免把灰色当成强调色。
        best = theme.dark ? Rgb{0x9a, 0xa8, 0xd8} : Rgb{0x5a, 0x6b, 0x8c};
    }
    // 强调色要在背景上看得清：浅色主题里压暗，深色主题里提亮。
    int r = best.r;
    int g = best.g;
    int b = best.b;
    const int luma = luminance(r, g, b);
    if (!theme.dark && luma > 140) {
        r = r * 140 / luma;
        g = g * 140 / luma;
        b = b * 140 / luma;
    } else if (theme.dark && luma < 150) {
        const int lift = 150 - luma;
        r += lift;
        g += lift;
        b += lift;
    }
    theme.accent = {clamp_byte(r), clamp_byte(g), clamp_byte(b)};

    if (theme.dark) {
        theme.ink = {0xff, 0xff, 0xff};
        theme.ink_soft = {0xdc, 0xdc, 0xe4};
        theme.ink_faint = {0xa6, 0xa6, 0xb2};
    } else {
        theme.ink = {0x26, 0x26, 0x2e};
        theme.ink_soft = {0x4a, 0x4a, 0x55};
        theme.ink_faint = {0x6e, 0x6e, 0x7a};
    }
    return theme;
}

void make_frosted_background(const std::uint8_t* grid, int grid_w, int grid_h,
                             const CoverTheme& theme, std::uint16_t* out, int out_w, int out_h) {
    if (grid == nullptr || out == nullptr || grid_w <= 0 || grid_h <= 0 || out_w <= 0 || out_h <= 0) {
        return;
    }
    // 4x4 Bayer 矩阵：RGB565 只有 32/64 级，大面积柔和渐变不抖动就会出现色带。
    static constexpr int kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

    // 混合比例（/256）：浅色主题混白，深色主题混黑，像透过磨砂玻璃看封面。
    const int keep = theme.dark ? 150 : 140;
    const int towards = theme.dark ? 0 : 255;

    auto axis = [](int position, int length, int cells, int& index, int& weight) {
        // 网格单元中心对齐到输出像素；smoothstep 让相邻色块之间过渡更柔和。
        const float f = (static_cast<float>(position) + 0.5f) * cells / length - 0.5f;
        const float clamped = std::max(0.0f, std::min(f, static_cast<float>(cells - 1)));
        index = static_cast<int>(clamped);
        if (index >= cells - 1) {
            index = cells > 1 ? cells - 2 : 0;
        }
        float t = cells > 1 ? clamped - static_cast<float>(index) : 0.0f;
        t = t * t * (3.0f - 2.0f * t);
        weight = static_cast<int>(t * 256.0f);
    };

    // 横向的网格下标与权重每列都一样，先算好。
    std::vector<int> column_index(static_cast<std::size_t>(out_w));
    std::vector<int> column_weight(static_cast<std::size_t>(out_w));
    for (int x = 0; x < out_w; ++x) {
        axis(x, out_w, grid_w, column_index[x], column_weight[x]);
    }

    const int next_x = grid_w > 1 ? 1 : 0;
    const int next_y = grid_h > 1 ? 1 : 0;
    for (int y = 0; y < out_h; ++y) {
        int gy = 0;
        int wy = 0;
        axis(y, out_h, grid_h, gy, wy);
        for (int x = 0; x < out_w; ++x) {
            const int gx = column_index[x];
            const int wx = column_weight[x];
            const std::uint8_t* c00 = grid + (static_cast<std::size_t>(gy) * grid_w + gx) * 3;
            const std::uint8_t* c10 = c00 + next_x * 3;
            const std::uint8_t* c01 = c00 + static_cast<std::size_t>(next_y) * grid_w * 3;
            const std::uint8_t* c11 = c01 + next_x * 3;
            const int bayer = kBayer[y & 3][x & 3];
            int channel[3];
            for (int c = 0; c < 3; ++c) {
                const int top = c00[c] * (256 - wx) + c10[c] * wx;
                const int bottom = c01[c] * (256 - wx) + c11[c] * wx;
                const int value = (top * (256 - wy) + bottom * wy) >> 16;
                channel[c] = (value * keep + towards * (256 - keep)) >> 8;
            }
            // 抖动幅度取一个量化台阶：红蓝 5 位（步长 8），绿 6 位（步长 4）。
            const int r = clamp_byte(channel[0] + (bayer * 8) / 16 - 4);
            const int g = clamp_byte(channel[1] + (bayer * 4) / 16 - 2);
            const int b = clamp_byte(channel[2] + (bayer * 8) / 16 - 4);
            out[static_cast<std::size_t>(y) * out_w + x] =
                static_cast<std::uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        }
    }
}

}  // namespace media
