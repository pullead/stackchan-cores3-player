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

## 2026-09-14：方案 B 第一阶段实施进度

本轮按最新 `pullead/esp32-hifi` 主分支（`1b9185e`，功能基线为其父提交 `a7f57b4`）开始执行方案 B。核心原则保持不变：StackChan CoreS3 独占 I2S/AW88298，HiFi 项目的文件流、解码器和曲目控制逻辑通过窄接口移植，不直接实例化 Arduino `Audio` 对象，不抢占 AI 音频链路；SD 卡始终只读，默认音量为 0%。

已完成并经过规格/质量审查的代码阶段：

1. 建立 `AudioStream`、`AudioDecoder` 和 `PcmBlock` 契约，明确 EOF、关闭、错误、容量和只读语义。
2. 将 CoreS3 SPI3 显示让渡逻辑接入 `SdAudioStream`：基于真实 `SdTrack.path` 使用 `fopen(..., "rb")`、`fread`、`fseek`、`ftell`、`fclose`；支持 UTF-8 长文件名；打开期间保持 handoff，关闭后才卸载和释放；卸载/读取/关闭错误均向上层传播。
3. 建立 HiFi 解码器适配边界，默认构建不会伪装成 MP3 播放成功；真实 `ESP32-audioI2S` backend 通过条件编译接入，当前尚未 vendored、固定并启用，因此尚未宣称真实 MP3 已可播放。
4. 完成流式 PCM 到 StackChan `AudioSink` 的播放桥：固定大小 PCM block、无需整曲缓存、支持 WAV 兼容路径、双声道转单声道、`EOF + 最后一批 PCM`、停止/错误/`stop_for_ai` 的确定性清理；媒体快照继续保持静音。
5. `LOCAL MUSIC` 曲目行已连接到真实 `SdTrack` 和只读流入口，保留上下滑动、中文/日文 UTF-8 文件名；当解码 backend 未启用时显示明确的 `DECODER UNAVAILABLE` / `MP3 BACKEND NOT ENABLED / MUTED`，不会伪造播放状态。

本轮提交范围：

- `def09b1` 至 `1267a0c`：流与解码器契约及边界测试。
- `dd26805` 至 `9e39fd0`：CoreS3 只读 SD 音频流、错误传播和 handoff 回归测试。
- `4e15056` 至 `4213747`：HiFi 解码器适配边界和条件编译安全检查。
- `388e80f` 至 `a914b87`：PCM 播放桥、停止清理、立体声 downmix 和 EOF 边界。
- `5eb867f`：`LOCAL MUSIC` 选曲入口和 decoder unavailable 状态。

验证限制与下一步：

- 当前工作环境缺少可用的主机 `cmake`/`g++`/`clang++`，新增主机测试已注册但未能在本机执行；已有固件编译链仍需在 ESP-IDF 环境进行完整验证。
- 真实 `ESP32-audioI2S` backend 仍需取得并审计精确依赖版本、接入 ESP-IDF 构建，然后再进行静音刷写和 COM6 设备验证。
- 本轮没有修改用户未提交的 `firmware/dependencies.lock`；SD 卡内容、音乐和歌词文件不会上传。
- 当前阶段不宣称 MP3/AAC/FLAC 已经可以播放；完成真实 backend 后再进行本地音乐播放和频谱模块接入。

## 2026-09-14：媒体所有权与频谱采样边界完成

继续执行 Phase 1 Task 6：

- 新增媒体/AI 音频所有权控制边界。进入媒体模式时保存真实 AI 快照、释放 AI 音频所有权并强制静音；退出媒体模式时恢复快照。重复进入/退出具备幂等语义，失败清理不会遗留媒体所有权。
- 新增原子 SPSC `PcmTap` 环形缓冲区。频谱观察数据满载时只丢弃并累计计数，不阻塞音频 writer；FFT 计算仍留在消费者侧，不进入音频写入任务。
- 播放失败清理后保留 `PlaybackState::Error` 和错误文本；显式 `stop()` 或重新选择曲目后才清除错误并回到干净 `Idle`。
- 修复 sink 打开失败状态、错误清理、ownership 释放和立体声 downmix 相关边界测试。

对应提交：`c96d24b`、`c4efaf0`、`8e1ae02`、`8295872`、`df3e0f7`。

仍未完成：真实 `ESP32-audioI2S` backend 的固定版本接入，以及 Phase 1 完整 ESP-IDF 编译、COM6 静音刷写和设备回归。当前没有执行任何未授权的 SD 写入或音量提升。

## 2026-09-14：改用 ESP-IDF 固定 MP3 解码组件

对完整 Arduino `ESP32-audioI2S` 进行审计后，确认其 `Audio` 对象会接管 I2S、FS 和音量，不适合直接嵌入 StackChan。随后改用固件已经锁定的 `espressif/esp_audio_codec` 2.4.1，其组件哈希为 `4d5cbe02f59fb45e40112d63317a8ddd00019cd4`，提供 `esp_mp3_dec_open/decode/reset/close`。

新增真实 `EspMp3DecoderBackend`：

- 只从 `AudioStream` 增量读取 MP3 编码数据；
- 通过 `esp_mp3_dec_decode` 输出 PCM，不创建 Arduino `Audio`、I2S 或 `AudioSink`；
- 校验采样率、1/2 声道、16-bit、完整交错帧和输出容量；
- 区分正常 EOF、截断/损坏数据、I/O 错误和 decoder 错误；
- 首帧解码后才知道格式并打开 AudioSink，双声道继续由 StackChan 控制器 downmix 为单声道；
- 保持 WAV 路径原有启动顺序和默认静音策略。

相关提交：`4b82fb8`、`eb00930`、`82233f5`、`6061762`、`ebc603b`。

当前阻塞仅剩环境验证：本机 IDF 构建第一次因 `cmake` 不在 PATH 失败，需要重新加载 ESP-IDF 工具环境后运行完整固件编译；编译通过后再进行 COM6 静音刷写和真实 MP3 设备验证。

## 2026-09-14：真实 MP3 backend 编译验证启动

随后修复并确认了压缩音频启动顺序：MP3 解码器在 `open()` 后格式尚未确定，控制器会先完成首帧解码，再根据实际采样率/声道打开 AudioSink；首帧立体声仍会 downmix 为 CoreS3 sink 所需的单声道。对应测试已更新，确认 MP3 启动阶段不会过早打开 sink，首帧成功后才打开。

本轮使用 ESP-IDF 5.5.5 工具链重新配置了独立构建目录 `firmware/build-real-mp3`。配置阶段成功识别：

- 目标：ESP32-S3；
- `espressif/esp_audio_codec` 2.4.1；
- `main` 组件和新增 `EspMp3DecoderBackend`；
- 生成默认资源和分区表。

全量 Ninja 编译已开始，当前记录进度约为 `115/2507`，尚未出现 MP3 backend 或 C++ 源码错误。由于这是全新构建目录，编译会比增量构建耗时更长；完成后仍需检查最终链接、镜像大小、刷写和 COM6 静音启动日志。

本轮新增/修复提交：`eb00930`、`82233f5`、`6061762`、`ebc603b`、`024e022`。本次日志提交不包含用户未提交的 `firmware/dependencies.lock` 修改。
## 2026-09-14 — Real ESP32-audioI2S backend gate

本阶段完成了真实解码 backend 的依赖审计边界。最新 `esp32-hifi` 主分支为 `1b9185e`，其 PlatformIO 声明使用未固定的 `ESP32-audioI2S` Git URL。该库的常规 `Audio` API 同时接管 Arduino FS 和 I2S 输出，不能直接塞入 CoreS3 的只读 `SdAudioStream`，否则会绕过 GPIO35/SPI3 显示让渡并产生第二个音频所有者。

因此本阶段没有把未审计的 moving branch 或无法验证的源码冒充成真实 MP3 backend。`hifi_decoder_adapter` 继续保持显式 build gate：只有定义 `CONFIG_STACKCHAN_HIFI_AUDIOI2S_BACKEND` 且提供 `STACKCHAN_AUDIOI2S_BACKEND_TARGET` 时才允许接入；默认构建返回空 backend，避免“假播放成功”。新增 `docs/ESP32_AUDIOI2S_BACKEND_PROVENANCE.md`，记录接口约束、许可证/来源和可复现接入条件。

当前结论：真实 MP3 fixture/integration test 仍需在取得并审计不可变 dependency SHA 后进行；本轮不修改 SD 内容、不改变音量，也不修改用户未提交的 `firmware/dependencies.lock`。

### 补充审计：已有固定 Espressif MP3 解码器

检查发现仓库现有 `espressif/esp_audio_codec` 2.4.1 已包含 MP3 decoder，不必把 Arduino `ESP32-audioI2S` 整个运行时移入 StackChan。组件来源在 `firmware/managed_components/espressif__esp_audio_codec/idf_component.yml` 固定为 `4d5cbe02f59fb45e40112d63317a8ddd00019cd4`，且 `firmware/dependencies.lock` 已记录该组件。

证据：`include/decoder/impl/esp_mp3_dec.h` 提供 `esp_mp3_dec_open/decode/reset/close`，`esp_audio_dec.h` 的 common API 使用调用者提供的编码输入缓冲和 PCM 输出缓冲；ESP32-S3 预编译库已经存在。该 API 是 frame-oriented，需要新增薄适配层把 `AudioStream` 的增量读取、输入缓冲、metadata 和错误状态接到 `AudioDecoder`，但不会创建 I2S、AudioSink 或改变音量。

该组件头文件含 Espressif Modified MIT/MIT 许可证说明，目标硬件是 Espressif CoreS3；实现时优先链接已管理组件，不复制第三方实现源码。真实 MP3 fixture 测试应在适配层完成后加入。

## 2026-09-16：P0 阶段——真实播放链路的四个结构性修复

本轮由 Claude Code 接手。目标是让一首真实 MP3 能在 StackChan 上不断音地播放，同时播放期间屏幕仍可刷新。用户确认的三个前提：媒体模式下重建 codec 为 44.1 kHz 立体声、音量继续锁定 0%（架构先行，出声测试另行授权）、退出 `LOCAL MUSIC` 即停止播放。

### 先修基线：主机测试从未真正执行过

`codex` 阶段登记的主机测试在本机首次运行后暴露 5 个问题，全部已修复：

1. `local_playback_controller_test` 与 `hifi_decoder_adapter_test` 无法链接。CMake 漏了 `pcm_tap.cpp`、`media_mode_controller.cpp`；真实 MP3 后端依赖 ESP 组件头，主机无法编译，改为 `tests/host_stubs/esp_mp3_decoder_backend_host_stub.cpp`，并在测试中注明该断言只代表主机语义。
2. WAV 路径的分块契约被破坏。`read_frames(frames.data(), frames.size())` 把数组元素数当作帧数上限，一次读 2048 帧而非约定的 1024（`6061762` 扩大缓冲时漏改）。
3. 多个测试在控制器销毁 `FakeStream` 之后读取 `stream->closed`，属于 use-after-free，结果不确定。改为通过生命周期更长的标志观察关闭。
4. 三处测试期望与 `df3e0f7` 有意引入的「失败后保留 `PlaybackState::Error`」行为冲突，已对齐到实现。
5. `sd_audio_stream_test` 让多个流同时存活，而 `Spi3DisplayHandoff` 全局唯一且不可重入，第二个流实际从未打开成功。改为逐用例作用域隔离。

另外新增 `tests/host_stubs/no_abort_dialog.cpp`：MSVC 调试运行时在 `abort()` 时弹出模态对话框并等待人工点击，任何红测试都会挂起整个 ctest（实测 11 秒对 1.2 秒）。现在断言失败直接输出到 stderr 并以非零码退出。

### 采样率死结：媒体模式重建 I2S 输出

CoreS3 的 `AUDIO_INPUT_SAMPLE_RATE` 与 `AUDIO_OUTPUT_SAMPLE_RATE` 都固定为 24000，`CreateDuplexChannels` 断言两者相等，而 `CoreS3SpeakerSink::open` 只接受 24000/单声道。任何 44.1 kHz 的 MP3 都必然在打开 sink 时失败，与解码后端是否可用无关。

新增 `media/audio/media_audio_session.*`：进入媒体模式时先释放麦克风、再重配输出时钟，退出时反向恢复；失败一律回滚，回滚本身失败则标记 `degraded()` 并给出错误文本，不静默吞掉。10 个主机测试覆盖顺序、非法格式、回滚、幂等，以及「切换媒体格式时不得把媒体格式误记为 AI 原始格式」。

硬件侧给 `CoreS3AudioCodec` 增加 `ReconfigureOutput(sample_rate, channels)`：关闭输出 → 停 TX 通道 → `i2s_channel_reconfig_std_clock` → 重新启用 → 按需恢复输出。ESP32-S3 的 I2S 没有 APLL，44.1 kHz 由默认 PLL 的分数分频得到。同时修正了一处隐患：`EnableInput` 原本用 `output_sample_rate_` 配置麦克风，媒体模式改动输出后会带错采样率，已改为 `input_sample_rate_`。

`CoreS3SpeakerSink` 随之放宽格式并绑定会话，`AudioCodecPort::write_mono(frames)` 改为 `write_samples(交错样本)`，`LocalPlaybackController` 去掉降混，立体声原样送达。

### 解码搬出 UI 循环

原先 `AppLocalMusic::onRunning()` 直接调用 `playback_->pump()`，LVGL 帧率直接决定音频吞吐，必然断音。新增 `media/local/playback_pump_task.*`：core 1、优先级 4（高于表情/舵机任务的 3）、12 KB 栈的独立 FreeRTOS 任务；写 sink 时阻塞在 I2S DMA 上形成自然节流，空闲时休眠让出 CPU。调度规则抽为 `playback_pump_policy.h` 的纯函数以便主机测试。`LocalPlaybackController` 的公有方法改由一把递归互斥保护，UI 线程只读快照。

### SPI 总线：从整曲占用改为分块借用

`SdAudioStream` 原本在构造时取得 `Spi3DisplayHandoffGuard` 并持有到关闭，也就是整首歌期间都持有。而 handoff 会持有 LVGL 锁并把 GPIO35 从显示输出改路由为 SPI3 MISO——播放期间屏幕既没有锁也没有引脚，进度、频谱、触摸反馈都不可能实现。

改为分块借用：SD 卡保持挂载，每次预读 64 KB 时短暂借用总线再立刻归还，解码器随后从 RAM 取数据。按 128 kbps 估算约每 4 秒借用一次，屏幕在间隙中可以正常刷新。预读缓冲超过 512 字节，会由 `CONFIG_SPIRAM_USE_MALLOC` 自动落到 PSRAM。`seek` 会丢弃属于旧位置的缓冲数据，`tell()` 返回已交付字节数而非预读位置，并新增 `borrow_count()` 便于诊断。

### 模式切换会跳过 onClose，导致 AI 语音以错误采样率启动

`main.cpp` 的控制流是：Mooncake 主循环检测到 AI 启动请求后退出 → `uninstallAllApps()` → `DestroyMooncake()` → `startXiaozhi()`（永不返回）。因此 `AppLocalMusic` 与 AI 语音物理上不可能同时运行，原计划中 `MediaModeController` 的 AI 状态快照在当前架构下并非必需。

但 `Mooncake::uninstallAllApps()` 直接 `_app_ability_manager.reset()`，不经过 Ability 状态机，**不会调用 `onClose()`**。而 `CoreS3SpeakerSink` 与 `MediaAudioSession` 原本都没有析构函数，于是「在 `LOCAL MUSIC` 打开状态下切换到 AI 模式」会让 codec 停留在 44.1 kHz 立体声、麦克风关闭的状态，紧接着启动的 AI 语音就会用错采样率，麦克风也不工作。

修复方式是把归还动作下沉到析构：`MediaAudioSession` 析构时 `release()`，`CoreS3SpeakerSink` 析构时 `close()`。两者都补了主机测试，直接断言「仅靠析构也必须恢复 24 kHz 双工并交还麦克风」。同时 `PlaybackPumpTask::stop()` 增加了等待超时的周期性告警，避免泵卡在 SD 借用或 I2S 写入时无声无息。

### 编译过程中发现的真实缺陷

- `lv_font_montserrat_12` 并未编入本固件的字体集。这证实 `codex` 的最后一个提交 `62b63b7` 从未编译过。已改用固件内已有的 `font_puhui_14_1`。
- `main/CMakeLists.txt` 的 `file(GLOB_RECURSE)` 缺少 `CONFIGURE_DEPENDS`，新增源文件会静默不参与链接（`playback_pump_task.cpp` 即因此链接失败）。已补上。

### 当前状态

- 主机测试 19/19 通过。
- 固件镜像编译通过（`firmware/build-real-mp3`）。
- 音量仍固定 0%，SD 卡仍为只读挂载，未执行任何刷写。
- 尚未在设备上验证：44.1 kHz 重配是否真正生效、播放期间屏幕是否确实可刷新、退出后 AI 链路是否完全恢复。这些需要静音刷写后按客观指标核对（I2S 实际配置日志、写入帧数与墙钟时间之比收敛到 44100、环形缓冲 underrun 与 `PcmTap` drop 计数为零）。

## 2026-09-16：设备验证——本地 MP3 播放链路打通

静音前提下完成了设备回归。主机测试与固件编译全绿的情况下，真机仍暴露出 9 个缺陷，其中 7 个是此前从未在设备上执行过的代码所致，2 个由本轮改造引入。最终播放速率稳定在 44,080–44,104 帧/秒，与 44,100 Hz 的目标误差约 0.05%。

### 真机暴露的缺陷

1. **点击回调销毁自身按钮**（use-after-free → panic 重启）。`render_playback()` 的 `track_rows_.clear()` 会销毁正在派发事件的那个按钮，闭包随之释放，后续代码读到野指针，`std::mutex::lock` 解引用地址 2 崩溃。`BACK TO LIST` 同理。改为点击只记录 `PendingAction`，由 `onRunning()` 在事件派发之外执行；LVGL 9 本身也不允许在事件回调中删除对象。
2. **PCM 缓冲小于一个 MP3 帧**。MP3 每帧 1152 采样/声道，立体声需 4608 字节，而 `kPlaybackChunkFrames = 1024` 只提供 4096 字节，解码器每次都返回 `BUFF_NOT_ENOUGH` 且不消费任何数据。该常量原本按 24 kHz 单声道 WAV 选定，从未对照 MP3 帧结构核算；主机测试的假解码器每次只产出 2 帧，因此碰不到这个边界。
3. **ID3v2 标签被当作文件损坏**。解码器消费完整个输入缓冲却产不出帧，原实现在 4 次尝试后判定 `Malformed`。现在在 `open()` 阶段解析标签长度并 seek 跳过（synchsafe 编码），同时允许解码器在“已消费但未产出”时继续搜索帧边界，上限 512 KB。实测标签大小从 55 字节到 108 KB 不等。
4. **SD 卡运行在握手频率**。挂载使用 `SDMMC_FREQ_PROBING`（400 kHz），吞吐上限约 50 KB/s，而 320 kbps 的 MP3 需要约 40 KB/s，导致播放只能跑到 53%。改为 `SDMMC_FREQ_DEFAULT`（20 MHz）并保留失败回退，因为 GPIO35 与 LCD D/C 共享。
5. **进度读取与音频任务争锁**。`onRunning()` 通过 `snapshot()` 读进度需要控制器互斥锁，而音频任务在整个 `pump()` 期间持有该锁（含 SD 借用与阻塞写 I2S），界面只能在块间隙更新，秒数一次跳 5 秒。改为通过原子变量无锁发布状态、已播放帧数和采样率。
6. **音频热路径上的堆分配**。`snapshot()` 按值返回两个 `std::string`，在每块一次的循环里反复分配。改为无分配的 `state()` / `played_frames()`。
7. **`%llu` / `%lld` 打印异常**。ESP-IDF 的精简 newlib 不支持，输出为 `lu` / `ld`，一度使速率日志不可读。

### 本轮改造引入并修复的缺陷

8. **`seek()` 后未重启预读任务**。为避免旧位置数据混入，`seek()` 会停止填充任务，却没有重新启动，环形缓冲再不被填充，解码器永久等待，界面停在 `BUFFERING`。跳过 ID3 标签必然触发 seek，因此这是必经路径。
9. **无上限的等待拖死界面**。承上：`pump()` 持有控制器锁期间无限等待数据，而 `BACK TO LIST` 需要同一把锁，导致整页无响应。读取等待现在有上限，超时报告 stall 并退出。

### 速率问题的定位过程

三次假设被数据否定，记录下来以免重复：

- **PSRAM 带宽**：把解码缓冲移入内部 RAM 后速率纹丝不动（内部堆减少 16.4 KB、PSRAM 释放 12.8 KB，证明改动生效）。假设不成立，但该改动本身正确，予以保留。
- **I2S 分频退化为整数**：读取 `I2S_TX_CLKM_CONF_REG`（N=14）与 `I2S_TX_CLKM_DIV_CONF_REG`（x=4, y=62, z=76）算得 MCLK = 160 MHz ÷ (14 + 76/442) = 11.2899 MHz，采样率 44,101 Hz，误差 0.003%。时钟完全正确。
- **单次 SD 停顿超过 DMA 深度**：预读块从 64 KB 降到 16 KB 后仅提升 0.5%。

真正的原因是架构性的：解码与 SD 读取处于同一任务、串行执行，借用总线的时间完全不产出 PCM，而该时间占比恰好等于欠速比例（约 1.6%），调整块大小无法改变这个比值。解决方案是独立的预读任务加 256 KB 环形缓冲（PSRAM），音频任务只从内存取数据。新增的 `ByteRing` 与 `PrefetchingStream` 均可在主机测试中独立驱动，填充逻辑不写成任务私有代码即为此目的。

### 当前状态

- 主机测试 21/21。
- 播放速率 44,080–44,104 帧/秒；栈余量约 10 KB，内部堆约 107 KB，PSRAM 约 7.59 MB，长时间播放无波动。
- 音量仍固定 0%，SD 卡保持只读挂载。
- 未解决：曾出现一次“播放中自行重启”，此后未能复现，无 backtrace；内存余量数据不支持内存耗尽的解释。
