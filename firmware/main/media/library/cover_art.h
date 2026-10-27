#pragma once

#include <cstddef>
#include <cstdint>

// 专辑封面：从 ID3v2 标签里找出内嵌图片，并把解码后的像素加工成播放页
// 要用的三样东西——圆形封面、毛玻璃背景和配色。这里全是纯计算，不碰
// 硬件与 LVGL，可以在主机上测试；JPEG 解码在调用方。
namespace media {

// ID3v2 标签的总长度（头 + 内容 + 可选的 v2.4 尾部），不是标签时返回 0。
// 只需要前 10 个字节，用来决定为了封面要从文件开头读多少。
std::size_t id3v2_tag_size(const std::uint8_t* data, std::size_t size);

struct EmbeddedPicture {
    std::size_t offset = 0;  // 图片数据在 data 中的起点
    std::size_t length = 0;
};

// 在 ID3v2.2/2.3/2.4 标签中查找 JPEG 封面（APIC / PIC 帧）。有多张图时优先
// 取封面（picture type 3），否则取第一张 JPEG。PNG 等其他格式不报告：
// 固件里只有 JPEG 解码器。反同步的标签、压缩或加密的帧同样跳过。
bool find_id3v2_picture(const std::uint8_t* data, std::size_t size, EmbeddedPicture& out);

struct Rgb {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
};

// 由封面推出的播放页配色。
struct CoverTheme {
    // 封面整体偏暗：背景混黑、文字用浅色；否则背景混白、文字用深色。
    bool dark = false;
    // 封面里最鲜艳的颜色，用于频谱光条与进度条。
    Rgb accent;
    // 正文、次要文字与图标的颜色。
    Rgb ink;
    Rgb ink_soft;
    Rgb ink_faint;
};

// 把 RGB888 图像居中裁成正方形后，按区域平均缩放到 dst_w x dst_h。
void downsample_rgb888(const std::uint8_t* src, int src_w, int src_h, std::uint8_t* dst, int dst_w,
                       int dst_h);

// 由正方形 RGB888 图像生成 LVGL ARGB8888（内存顺序 B,G,R,A）的圆形封面，
// 圆外完全透明，边缘一像素做抗锯齿。
void make_round_cover(const std::uint8_t* rgb, int size, std::uint8_t* argb_out);

// 由封面的小尺寸网格（RGB888）推出配色。
CoverTheme analyse_cover(const std::uint8_t* grid, int grid_w, int grid_h);

// 毛玻璃背景：把小网格平滑放大到 out_w x out_h，并按主题向白色或黑色混合，
// 输出 LVGL 原生 RGB565。转换时做有序抖动，避免大面积渐变出现色带。
void make_frosted_background(const std::uint8_t* grid, int grid_w, int grid_h,
                             const CoverTheme& theme, std::uint16_t* out, int out_w, int out_h);

}  // namespace media
