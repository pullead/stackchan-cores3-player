"""把一段视频转成启动器屏保用的动态壁纸（LMWP 格式），烧进闲置的 ota_1 分区。

用法（需要 Pillow 和 imageio-ffmpeg，不要装进 IDF 的 Python 环境）：
    python make_wallpaper.py 输入.mp4 输出.bin [--fps 15] [--anchor 0.5] [--start 0] [--duration 0]

  --fps       输出帧率。屏幕整帧刷新约 30 ms，15–20 比较现实。
  --anchor    从 16:9 裁成 4:3 时裁切框的水平位置：0 贴左、0.5 居中、1 贴右。
              主体不在画面正中时用它把主体留在画面里。
  --start/--duration  截取片段（秒），duration 为 0 表示到结尾。

烧录（ota_1 分区在 0x510000，大小 0x4f0000，见 partitions.csv）：
    python -m esptool --chip esp32s3 -p COM6 write_flash 0x510000 输出.bin

注意：壁纸素材往往有版权，转换结果只烧进自己的设备，不要提交进仓库。

LMWP 格式（小端）：
    0   char[4]  "LMWP"
    4   u16      版本 = 1
    6   u16      宽
    8   u16      高
    10  u16      帧率 x100
    12  u32      帧数
    16  u32      索引表偏移（= 32）
    20  u32      文件总长
    24  u32[2]   保留
    32  帧数 x { u32 偏移, u32 长度 }   每帧是一张 baseline JPEG
    ... JPEG 数据，每帧起点按 16 字节对齐
"""

import argparse
import glob
import os
import struct
import subprocess
import sys
import tempfile

import imageio_ffmpeg
from PIL import Image

WIDTH = 320
HEIGHT = 240
PARTITION_BYTES = 0x4F0000
HEADER_BYTES = 32
ALIGN = 16


def extract_frames(source, work, fps, anchor, start, duration, quality):
    """用 ffmpeg 裁成 4:3、缩到 320x240、按帧率抽帧，输出 JPEG。"""
    for old in glob.glob(os.path.join(work, "f_*.jpg")):
        os.remove(old)
    # 先按高度裁出 4:3 的框，水平位置由 anchor 决定，再缩放。
    crop = f"crop=ih*4/3:ih:(iw-ih*4/3)*{anchor}:0"
    filters = f"fps={fps},{crop},scale={WIDTH}:{HEIGHT}:flags=lanczos,format=yuvj420p"
    command = [imageio_ffmpeg.get_ffmpeg_exe(), "-hide_banner", "-loglevel", "error", "-y"]
    if start > 0:
        command += ["-ss", str(start)]
    command += ["-i", source]
    if duration > 0:
        command += ["-t", str(duration)]
    # mjpeg 编码器输出 baseline JPEG（4:2:0），正是 esp_new_jpeg 支持的格式。
    command += ["-vf", filters, "-q:v", str(quality), os.path.join(work, "f_%05d.jpg")]
    subprocess.run(command, check=True)
    return sorted(glob.glob(os.path.join(work, "f_*.jpg")))


def pack(frames, fps):
    """把 JPEG 帧打包成 LMWP。"""
    count = len(frames)
    data_start = HEADER_BYTES + count * 8
    data_start = (data_start + ALIGN - 1) // ALIGN * ALIGN
    index = []
    blobs = []
    offset = data_start
    for path in frames:
        with open(path, "rb") as handle:
            blob = handle.read()
        index.append((offset, len(blob)))
        padded = (len(blob) + ALIGN - 1) // ALIGN * ALIGN
        blobs.append(blob + b"\0" * (padded - len(blob)))
        offset += padded
    total = offset
    header = struct.pack("<4sHHHHIII8x", b"LMWP", 1, WIDTH, HEIGHT, int(round(fps * 100)),
                         count, HEADER_BYTES, total)
    table = b"".join(struct.pack("<II", o, n) for o, n in index)
    body = header + table
    body += b"\0" * (data_start - len(body))
    return body + b"".join(blobs)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("source")
    parser.add_argument("output")
    parser.add_argument("--fps", type=float, default=15)
    parser.add_argument("--anchor", type=float, default=0.5)
    parser.add_argument("--start", type=float, default=0)
    parser.add_argument("--duration", type=float, default=0)
    args = parser.parse_args()
    anchor = min(max(args.anchor, 0.0), 1.0)

    with tempfile.TemporaryDirectory() as work:
        # 从较好的画质开始；装不进分区就逐步降低（ffmpeg 的 q 越大越差）。
        for quality in range(3, 16):
            frames = extract_frames(args.source, work, args.fps, anchor, args.start, args.duration,
                                    quality)
            if not frames:
                print("没有抽出任何帧", file=sys.stderr)
                return 1
            blob = pack(frames, args.fps)
            print(f"q={quality}: {len(frames)} 帧, {len(blob)} 字节"
                  f"（分区 {PARTITION_BYTES} 字节，平均每帧 {len(blob) // len(frames)} 字节）")
            if len(blob) <= PARTITION_BYTES:
                break
        else:
            print("降到最低画质也装不进分区：请缩短时长或降低帧率", file=sys.stderr)
            return 1

        with open(args.output, "wb") as handle:
            handle.write(blob)
        # 顺手留一张第一帧的预览，方便在电脑上确认裁切位置。
        preview = os.path.splitext(args.output)[0] + "_preview.png"
        Image.open(frames[0]).save(preview)
        print(f"已写出 {args.output}，预览 {preview}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
