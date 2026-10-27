"""重新生成 LOCAL MUSIC 播放器界面的图片资源。

原图来自 LVGL 官方音乐播放器 demo（MIT 许可，Copyright (c) LVGL Kft），位于
managed_components/lvgl__lvgl/demos/music/assets/png/272_png。demo 的尺寸是为
480x272 横屏设计的；CoreS3 为 320x240，这里按各元素在新布局里的实际可用空间
缩小后，再用 LVGL 自带的 LVGLImage.py 转成 C 数组。

运行（需要 Pillow 与 pypng；不要装进 IDF 的 Python 环境）：
    python generate.py

输出的 lm_*.c 会被 main/CMakeLists.txt 的 apps/*.c 规则自动收进固件。
"""

import os
import subprocess
import sys
import tempfile

from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
FIRMWARE = os.path.normpath(os.path.join(HERE, "..", "..", "..", ".."))
LVGL = os.path.join(FIRMWARE, "managed_components", "lvgl__lvgl")
SOURCE = os.path.join(LVGL, "demos", "music", "assets", "png", "272_png")
CONVERTER = os.path.join(LVGL, "scripts", "LVGLImage.py")

# (源文件, 输出名, 目标宽, 目标高, 颜色格式)
# 不带透明通道的图（封面、波浪）用 RGB565：体积减半，绘制时也不必做 alpha 混合。
ASSETS = [
    ("btn_play.png", "lm_btn_play", 44, 43, "ARGB8888"),
    ("btn_pause.png", "lm_btn_pause", 44, 43, "ARGB8888"),
    ("btn_prev.png", "lm_btn_prev", 34, 34, "ARGB8888"),
    ("btn_next.png", "lm_btn_next", 34, 34, "ARGB8888"),
    ("btn_rnd.png", "lm_btn_rnd", 18, 18, "ARGB8888"),
    ("btn_loop.png", "lm_btn_loop", 18, 18, "ARGB8888"),
    ("btn_list_play.png", "lm_btn_list_play", 32, 33, "ARGB8888"),
    ("btn_list_pause.png", "lm_btn_list_pause", 32, 33, "ARGB8888"),
    ("icn_heart.png", "lm_icn_heart", 20, 20, "ARGB8888"),
    ("list_border.png", "lm_list_border", 272, 4, "ARGB8888"),
    ("cover_1.png", "lm_cover_default", 108, 107, "RGB565"),
]


def main() -> int:
    with tempfile.TemporaryDirectory() as work:
        for source, name, width, height, colour_format in ASSETS:
            image = Image.open(os.path.join(SOURCE, source))
            image = image.convert("RGBA" if colour_format == "ARGB8888" else "RGB")
            if image.size != (width, height):
                image = image.resize((width, height), Image.LANCZOS)
            png_path = os.path.join(work, name + ".png")
            image.save(png_path)
            subprocess.run(
                [sys.executable, CONVERTER, "--ofmt", "C", "--cf", colour_format,
                 "-o", HERE, png_path],
                check=True,
            )
            # 转换器的回退路径是 "lvgl/lvgl.h"，而 IDF 组件目录名为 lvgl__lvgl，
            # 那条路径找不到；与固件里其他图片资源一样，先声明用 "lvgl.h"。
            c_path = os.path.join(HERE, name + ".c")
            with open(c_path, "r", encoding="utf-8") as handle:
                body = handle.read()
            with open(c_path, "w", encoding="utf-8", newline="\n") as handle:
                handle.write("#ifndef LV_LVGL_H_INCLUDE_SIMPLE\n"
                             "#define LV_LVGL_H_INCLUDE_SIMPLE\n#endif\n" + body)
            print(f"{name}: {width}x{height} {colour_format}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
