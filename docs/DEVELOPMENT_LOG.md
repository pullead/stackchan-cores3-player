# StackChan CoreS3 Player 移植开发日志

## 项目范围

本仓库基于 M5Stack 官方 StackChan 固件，面向 CoreS3 硬件，加入本地媒体浏览基础能力，并为后续电台/播放器模式保留清晰的硬件边界。固件继续保留 StackChan 的 AI 语音、表情、舵机和原有启动器；媒体模式与 AI 模式采用互斥模式切换，避免同时争用显示、音频编解码器和 SD 卡总线。

本项目仅供个人使用。测试期间音频输出策略固定为 0%，不会因为浏览本地文件而自动播放声音，也不会改写或格式化 SD 卡。

## 需求与约束

- 硬件：M5Stack StackChan / CoreS3。
- 基线：`m5stack/StackChan` 官方固件结构；媒体扫描参考 `pullead/esp32-hifi` 的 SD 卡目录布局。
- 必须保留：AI 语音、唤醒词、表情动画、舵机控制。
- 可不保留：OTA（本移植阶段没有把 OTA 作为媒体功能依赖）。
- SD 卡：只读挂载，不格式化、不创建目录、不写入播放列表；同一张卡需要继续供原 ESP32 HiFi 播放器使用。
- 默认音量：0%。所有硬件验证均以静音为前提。
- 目标功能：本地音乐入口、真实长文件名、有限深度目录扫描、列表滚动；实际 MP3 解码播放和电台网络播放仍属于后续阶段。

## 关键问题与定位过程

### 1. SD 卡挂载失败并非文件系统格式问题

CoreS3 的 SD 卡使用 SPI，总线脚位为：

| 信号 | GPIO |
| --- | ---: |
| CS | 4 |
| MOSI | 37 |
| CLK | 36 |
| MISO | 35 |

GPIO35 同时经过显示 D/C 路径。StackChan 启动显示后，如果不释放 SPI3 的矩阵映射，SD 访问会出现超时或原始扇区读取失败。实现了显示与 SD 之间的显式 SPI3 handoff，并在挂载前按 CoreS3 电源路径重新启用 SD 供电：AXP2101 ALDO4 与 AW9523B P0_4。挂载配置保持 `format_if_mount_failed=false`，因此失败时不会格式化卡。

### 2. “没有歌曲”是目录和扩展名不匹配

HiFi 卡的典型布局是 `/audiofiles/` 及其子目录，文件通常是 MP3、AAC、M4A、FLAC、OGG、OPUS 或 WAV；原 StackChan 页面只查看根目录且只接受 WAV，所以即使 SD 已挂载也会显示空列表。

当前扫描器：

- 扫描 SD 根目录和有限深度的子目录（深度上限为 2）；
- 支持 `wav/mp3/aac/m4a/flac/ogg/opus`，大小写不敏感；
- 忽略歌词文件（例如 `.lrc`）；
- 保存相对路径，避免同名文件互相覆盖；
- 仅执行目录读取，未加入任何写入或格式化操作。

### 3. `UTADA、UTADA2...` 是 FAT 8.3 别名

旧配置明确关闭了 FAT 长文件名支持（`CONFIG_FATFS_LFN_NONE`），FatFs 只能返回类似 `UTADA~1.MP3` 的短别名。现已改为堆上分配长文件名缓冲区：

```text
CONFIG_FATFS_LFN_HEAP=y
CONFIG_FATFS_MAX_LFN=255
CONFIG_FATFS_API_ENCODING_UTF_8=y
```

这样目录项可以返回真实文件名；中文、日文等字体是否能完整显示，仍取决于固件内置字体覆盖范围，不影响文件发现和路径读取。

### 4. 曲目列表不能滑动

旧页面把曲目画成固定数量的静态标签，并主动关闭了页面滚动。现在改为独立的 LVGL 垂直滚动容器：列表按实际扫描结果动态创建标签，启用垂直滚动和活动滚动条，页面底部的返回按钮不参与列表滚动。当前列表行主要用于浏览，点击选曲与解码播放链路尚未宣称完成。

## 已实现的源码区域

- `firmware/main/hal/board/spi3_display_handoff.cpp`：显示/SD 的 SPI3 总线切换。
- `firmware/main/hal/board/stackchan.cc`：CoreS3 SD 供电、挂载与显示初始化协调。
- `firmware/main/hal/board/cores3_audio_codec.cc`：音频编解码器初始化时维持 0% 默认音量。
- `firmware/main/media/storage/sd_card_port.*`：只读挂载、扇区诊断、有限深度目录扫描和音频扩展名识别。
- `firmware/main/apps/app_local_music/*`：本地音乐页面、真实文件名标题和 LVGL 滚动列表。
- `firmware/main/media/audio/volume_policy.h`：媒体模式的静音策略。
- `firmware/tests/*`：扫描器、LFN 配置、SPI handoff、音量策略和页面数据的回归测试入口。

## 验证记录

### 已完成

- CoreS3 SD 物理访问已观察到正确容量与 MBR `55AA` 签名。
- 通过 SPI3 handoff 后，日志可见 SD mount ready。
- 本地音乐页面已经能够发现 HiFi 卡 `/audiofiles` 下的 MP3 曲目。
- 真实长文件名配置已进入生成的 `sdkconfig.h`：`CONFIG_FATFS_LFN_HEAP=1`、`CONFIG_FATFS_API_ENCODING_UTF_8=1`。
- 主固件镜像已成功完成应用链接、资源生成和镜像尺寸检查，产物为 `firmware/build-diagnostic-idf/stack-chan.bin`。
- 2026-09-14：根据设备回归反馈，将本地音乐列表、曲目数量和状态提示从 Montserrat 切换为固件已有的 `font_puhui_14_1`，修复中文/日文 glyph 缺失导致只看到英文片段（例如类似 `UTADA123`）的问题。
- 2026-09-14：CJK 字体版本已刷写 COM6；bootloader、主固件、分区表和资源分区均通过 `esptool verify_flash`，启动日志再次确认音量为 0% 且 SD browse-only mount ready。

### 暂未完成

- 本日志对应的镜像尚未在本轮刷写到 COM6（按用户要求先提交仓库）。
- 尚未进行最终设备上的真实滑动回归和长文件名视觉确认。
- 当前播放器控制器仍以兼容 WAV 的 PCM 通路为基础；MP3/AAC/FLAC 等格式的解码播放、曲目点击回调和电台网络播放需要后续迭代。
- SD 卡中的音乐和歌词文件不属于本仓库，也不会上传。

## 后续计划

1. 在用户确认设备空闲后刷写已生成镜像，保持音量 0%。
2. 验证 `SETUP -> LOCAL MUSIC` 的真实文件名、目录显示和手指垂直滑动。
3. 为列表行增加播放状态与点击回调，先完成兼容 WAV 的只读播放链路。
4. 在不改变 SD 卡格式的前提下接入 MP3 解码器和 LRC 元数据读取。
5. 实现媒体/AI 模式保存状态与切换，确保退出媒体模式后 AI、表情和舵机恢复。
6. 在上述基础稳定后再移植网络电台模块，并为网络失败、无 SD 卡、解码失败提供可恢复 UI。

## 2026-09-14：完整本地播放器移植方案确定

用户确认采用方案 B。方案 B 不需要插入 ESP32 HiFi 开发板，最终由 StackChan CoreS3 独立完成 SD 读取、解码、PCM 输出、音乐库和频谱显示。HiFi 开发板只作为对照测试设备。

详细方案见 [`docs/LOCAL_PLAYER_MIGRATION_PLAN.md`](LOCAL_PLAYER_MIGRATION_PLAN.md)。本阶段尚未开始播放器解码代码修改，先完成架构记录，避免直接把 HiFi 项目的 Arduino I2S 输出层与 StackChan 的 AI 音频链路叠加。

## 参考来源

- M5Stack StackChan：<https://github.com/m5stack/StackChan>
- M5Stack StackChan BSP：<https://github.com/m5stack/StackChan-BSP>
- ESP32 HiFi 参考项目：<https://github.com/pullead/esp32-hifi>
- 本仓库基于官方开源代码和个人移植修改，未上传设备专属密钥、Wi-Fi 凭据、构建缓存或 SD 卡内容。
