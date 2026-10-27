#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace media {

// 把内嵌封面 JPEG 解码成 size x size 的 RGB888（居中裁成正方形）。解码时先用
// 解码器自带的缩放缩到接近目标尺寸，再做区域平均，避免把整张大图解出来。
// 渐进式 JPEG 等解码器不支持的图返回 false，调用方改用默认封面。
bool decode_cover_jpeg(const std::uint8_t* data, std::size_t length, int size,
                       std::vector<std::uint8_t>& rgb_out);

}  // namespace media
