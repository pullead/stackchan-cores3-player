// 主机测试：ID3 内嵌封面定位与封面像素加工。
//
// 标签在测试里现场拼出来，让偏移量与长度在测试中就是解析器应当找到的那个数。

#include "media/library/cover_art.h"

#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace media;

namespace {

void append_size(std::vector<uint8_t>& out, std::size_t value, bool synchsafe) {
    if (synchsafe) {
        out.push_back(static_cast<uint8_t>((value >> 21) & 0x7F));
        out.push_back(static_cast<uint8_t>((value >> 14) & 0x7F));
        out.push_back(static_cast<uint8_t>((value >> 7) & 0x7F));
        out.push_back(static_cast<uint8_t>(value & 0x7F));
    } else {
        out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(value & 0xFF));
    }
}

void append_bytes(std::vector<uint8_t>& out, const char* text, std::size_t length) {
    out.insert(out.end(), text, text + length);
}

// 一个假 JPEG：真实的 SOI 加 APP0 起始字节，后面填充到指定长度。
std::vector<uint8_t> fake_jpeg(std::size_t length) {
    std::vector<uint8_t> jpeg = {0xFF, 0xD8, 0xFF, 0xE0};
    jpeg.resize(length, 0x55);
    return jpeg;
}

// v2.3/v2.4 的帧：4 字节 ID、长度、2 字节标志、内容。
void append_frame(std::vector<uint8_t>& tag, const char* id, const std::vector<uint8_t>& body,
                  bool synchsafe) {
    append_bytes(tag, id, 4);
    append_size(tag, body.size(), synchsafe);
    tag.push_back(0);
    tag.push_back(0);
    tag.insert(tag.end(), body.begin(), body.end());
}

std::vector<uint8_t> apic_body(uint8_t picture_type, const std::vector<uint8_t>& picture,
                               const char* mime = "image/jpeg") {
    std::vector<uint8_t> body;
    body.push_back(0);  // Latin-1
    append_bytes(body, mime, std::strlen(mime) + 1);
    body.push_back(picture_type);
    append_bytes(body, "cover", 6);
    body.insert(body.end(), picture.begin(), picture.end());
    return body;
}

std::vector<uint8_t> wrap_tag(uint8_t major, const std::vector<uint8_t>& frames) {
    std::vector<uint8_t> tag = {'I', 'D', '3', major, 0, 0};
    append_size(tag, frames.size(), true);
    tag.insert(tag.end(), frames.begin(), frames.end());
    return tag;
}

}  // namespace

int main() {
    // 标签总长：头 10 字节 + 内容，v2.4 带尾部时再加 10。
    {
        const std::vector<uint8_t> tag = wrap_tag(3, std::vector<uint8_t>(100, 0));
        assert(id3v2_tag_size(tag.data(), tag.size()) == 110);
        std::vector<uint8_t> footer = wrap_tag(4, std::vector<uint8_t>(100, 0));
        footer[5] = 0x10;
        assert(id3v2_tag_size(footer.data(), footer.size()) == 120);
        const uint8_t frame[] = {0xFF, 0xFB, 0x90, 0x00, 0, 0, 0, 0, 0, 0};
        assert(id3v2_tag_size(frame, sizeof(frame)) == 0);
        assert(id3v2_tag_size(nullptr, 0) == 0);
    }

    // v2.3：文字帧之后的 APIC，偏移量正好落在 JPEG 第一个字节。
    {
        std::vector<uint8_t> frames;
        std::vector<uint8_t> title = {0};
        append_bytes(title, "Song", 4);
        append_frame(frames, "TIT2", title, false);
        const std::vector<uint8_t> jpeg = fake_jpeg(300);
        const std::size_t apic_start = 10 + frames.size();
        append_frame(frames, "APIC", apic_body(3, jpeg), false);
        const std::vector<uint8_t> tag = wrap_tag(3, frames);

        EmbeddedPicture picture;
        assert(find_id3v2_picture(tag.data(), tag.size(), picture));
        // 帧头 10 + 编码 1 + "image/jpeg\0" 11 + 类型 1 + "cover\0" 6
        assert(picture.offset == apic_start + 10 + 1 + 11 + 1 + 6);
        assert(picture.length == jpeg.size());
        assert(tag[picture.offset] == 0xFF && tag[picture.offset + 1] == 0xD8);
    }

    // v2.4：synchsafe 帧长，UTF-16 描述以对齐的双字节 NUL 结尾。
    {
        std::vector<uint8_t> body = {1};  // UTF-16 带 BOM
        append_bytes(body, "image/jpeg", 11);
        body.push_back(3);
        const uint8_t description[] = {0xFF, 0xFE, 'a', 0, 'b', 0, 0, 0};
        body.insert(body.end(), description, description + sizeof(description));
        // 图片长度 200 > 127：synchsafe 与普通大端写法不同，读错就会越界。
        const std::vector<uint8_t> jpeg = fake_jpeg(200);
        body.insert(body.end(), jpeg.begin(), jpeg.end());
        std::vector<uint8_t> frames;
        append_frame(frames, "APIC", body, true);
        const std::vector<uint8_t> tag = wrap_tag(4, frames);

        EmbeddedPicture picture;
        assert(find_id3v2_picture(tag.data(), tag.size(), picture));
        assert(picture.length == jpeg.size());
        assert(tag[picture.offset] == 0xFF && tag[picture.offset + 2] == 0xFF);
    }

    // 有多张图时优先取封面（类型 3），即使它排在后面。
    {
        std::vector<uint8_t> frames;
        append_frame(frames, "APIC", apic_body(0, fake_jpeg(50)), false);
        const std::vector<uint8_t> front = fake_jpeg(80);
        append_frame(frames, "APIC", apic_body(3, front), false);
        const std::vector<uint8_t> tag = wrap_tag(3, frames);
        EmbeddedPicture picture;
        assert(find_id3v2_picture(tag.data(), tag.size(), picture));
        assert(picture.length == front.size());
    }

    // 只有其他类型的图时退而取第一张 JPEG。
    {
        std::vector<uint8_t> frames;
        append_frame(frames, "APIC", apic_body(4, fake_jpeg(60)), false);
        const std::vector<uint8_t> tag = wrap_tag(3, frames);
        EmbeddedPicture picture;
        assert(find_id3v2_picture(tag.data(), tag.size(), picture));
        assert(picture.length == 60);
    }

    // PNG 不报告：固件只有 JPEG 解码器。按内容判断，MIME 写成 jpeg 也一样。
    {
        std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
        png.resize(64, 0);
        std::vector<uint8_t> frames;
        append_frame(frames, "APIC", apic_body(3, png, "image/jpeg"), false);
        const std::vector<uint8_t> tag = wrap_tag(3, frames);
        EmbeddedPicture picture;
        assert(!find_id3v2_picture(tag.data(), tag.size(), picture));
    }

    // v2.2 的 PIC 帧：3 字节 ID 与 3 字节长度，格式字段是 "JPG"。
    {
        std::vector<uint8_t> body = {0};
        append_bytes(body, "JPG", 3);
        body.push_back(3);
        body.push_back(0);  // 空描述
        const std::vector<uint8_t> jpeg = fake_jpeg(40);
        body.insert(body.end(), jpeg.begin(), jpeg.end());
        std::vector<uint8_t> frames;
        append_bytes(frames, "PIC", 3);
        frames.push_back(0);
        frames.push_back(0);
        frames.push_back(static_cast<uint8_t>(body.size()));
        frames.insert(frames.end(), body.begin(), body.end());
        const std::vector<uint8_t> tag = wrap_tag(2, frames);
        EmbeddedPicture picture;
        assert(find_id3v2_picture(tag.data(), tag.size(), picture));
        assert(picture.length == jpeg.size());
    }

    // 截断的缓冲区与反同步标签都放弃，不越界读。
    {
        std::vector<uint8_t> frames;
        append_frame(frames, "APIC", apic_body(3, fake_jpeg(500)), false);
        std::vector<uint8_t> tag = wrap_tag(3, frames);
        EmbeddedPicture picture;
        assert(!find_id3v2_picture(tag.data(), 200, picture));
        tag[5] = 0x80;
        assert(!find_id3v2_picture(tag.data(), tag.size(), picture));
        assert(!find_id3v2_picture(nullptr, 0, picture));
    }

    // 缩放：纯色保持不变；非正方形居中裁切，两侧的颜色不进入结果。
    {
        std::vector<uint8_t> solid(4 * 4 * 3);
        for (std::size_t i = 0; i < solid.size(); i += 3) {
            solid[i] = 10;
            solid[i + 1] = 20;
            solid[i + 2] = 30;
        }
        uint8_t out[2 * 2 * 3] = {};
        downsample_rgb888(solid.data(), 4, 4, out, 2, 2);
        for (int i = 0; i < 4; ++i) {
            assert(out[i * 3] == 10 && out[i * 3 + 1] == 20 && out[i * 3 + 2] == 30);
        }

        // 6x2：左右各一列红色，中间 4 列绿色；裁成 2x2 正方形后只剩绿色。
        std::vector<uint8_t> wide(6 * 2 * 3, 0);
        for (int y = 0; y < 2; ++y) {
            for (int x = 0; x < 6; ++x) {
                uint8_t* p = wide.data() + (y * 6 + x) * 3;
                if (x < 2 || x > 3) {
                    p[0] = 255;
                } else {
                    p[1] = 255;
                }
            }
        }
        uint8_t centre[1 * 1 * 3] = {};
        downsample_rgb888(wide.data(), 6, 2, centre, 1, 1);
        assert(centre[0] == 0 && centre[1] == 255);
    }

    // 圆形封面：中心不透明，角上完全透明，通道顺序为 B,G,R,A。
    {
        const int size = 16;
        std::vector<uint8_t> rgb(size * size * 3);
        for (std::size_t i = 0; i < rgb.size(); i += 3) {
            rgb[i] = 200;
            rgb[i + 1] = 100;
            rgb[i + 2] = 50;
        }
        std::vector<uint8_t> argb(size * size * 4);
        make_round_cover(rgb.data(), size, argb.data());
        const uint8_t* centre = argb.data() + (8 * size + 8) * 4;
        assert(centre[0] == 50 && centre[1] == 100 && centre[2] == 200 && centre[3] == 255);
        assert(argb[3] == 0);                                // 左上角
        assert(argb[((size - 1) * size + size - 1) * 4 + 3] == 0);  // 右下角
        // 边上的中点在圆内但靠近边缘：部分透明或不透明，绝不是全透明。
        assert(argb[(8 * size + 0) * 4 + 3] > 0);
    }

    // 配色：暗封面用浅色字，亮封面用深色字；灰度封面不把灰色当强调色。
    {
        std::vector<uint8_t> dark(4 * 4 * 3, 20);
        const CoverTheme night = analyse_cover(dark.data(), 4, 4);
        assert(night.dark);
        assert(night.ink.r == 0xff);

        std::vector<uint8_t> sky(4 * 4 * 3);
        for (std::size_t i = 0; i < sky.size(); i += 3) {
            sky[i] = 150;
            sky[i + 1] = 200;
            sky[i + 2] = 240;
        }
        const CoverTheme day = analyse_cover(sky.data(), 4, 4);
        assert(!day.dark);
        assert(day.ink.r < 0x80);
        // 强调色偏蓝，并且被压暗到在浅色背景上可读。
        assert(day.accent.b > day.accent.r);

        std::vector<uint8_t> grey(4 * 4 * 3, 180);
        const CoverTheme mono = analyse_cover(grey.data(), 4, 4);
        assert(mono.accent.r != 180 || mono.accent.g != 180);
    }

    // 毛玻璃背景：浅色主题比原色更亮，尺寸正确，四角取自对应网格。
    {
        // 2x2 网格：左上红、右上绿、左下蓝、右下黑。
        const uint8_t grid[2 * 2 * 3] = {200, 0, 0, 0, 200, 0, 0, 0, 200, 0, 0, 0};
        CoverTheme theme;
        theme.dark = false;
        std::vector<uint16_t> out(32 * 24, 0);
        make_frosted_background(grid, 2, 2, theme, out.data(), 32, 24);
        const uint16_t top_left = out[0];
        const int red = (top_left >> 11) & 0x1F;
        const int blue = top_left & 0x1F;
        assert(red > blue);  // 左上偏红
        // 右下角原本是黑色，混白之后不再接近黑色。
        const uint16_t bottom_right = out[out.size() - 1];
        assert(((bottom_right >> 11) & 0x1F) > 8);

        theme.dark = true;
        make_frosted_background(grid, 2, 2, theme, out.data(), 32, 24);
        assert(((out[out.size() - 1] >> 11) & 0x1F) < 8);  // 深色主题混黑，仍然很暗
    }

    return 0;
}
