# StackChan CoreS3 本地播放器完整移植方案

## 目标

在 M5Stack 官方 StackChan 固件的 `LOCAL MUSIC` 中，完整移植 `pullead/esp32-hifi` 的本地播放器能力，同时保留 StackChan 的 AI 语音、表情、舵机和 CoreS3 SD 访问修复。

本移植采用方案 B：移植 HiFi 项目的解码、文件流、音乐库和播放队列逻辑，但继续使用 StackChan 的音频硬件管理和 `AudioSink` 输出。ESP32 HiFi 开发板不参与最终运行，仅用于对照测试。

## 上游基线

远程 `main` 当前最新提交为：

```text
1b9185ef09b0660e4f47a3ab96730699568cc746
```

该提交主要增加开发会话日志；播放器功能代码位于其父提交：

```text
a7f57b4 ESP32 HiFiPlayer: MiniWebRadio native-UI port to Waveshare S3-Touch-LCD-1.9 (320x170) + Pico Audio Pack
```

参考仓库：<https://github.com/pullead/esp32-hifi>

## 为什么选择方案 B

### 方案 A：直接嵌入 ESP32-audioI2S

优点是移植速度快，格式支持完整；缺点是它会自行管理 Arduino 文件系统、I2S、音频任务和音量，容易与 StackChan 的 AI 麦克风、I2S0、AW88298 和音量策略冲突。因此只适合短期验证，不作为最终架构。

### 方案 B：移植解码和播放队列

保留 HiFi 的多格式解码、文件流、M3U、元数据和播放队列，把 PCM 交给 StackChan 的 `AudioSink`。这样 I2S 和 AW88298 只有一个所有者，媒体/AI 模式也能显式切换。开发量较大，但硬件边界最清晰。

### 方案 C：重新采用 ESP-ADF/ESP-IDF 播放链路

长期扩展性好，但会同时改变解码器、任务、缓冲区和 I2S 架构，风险和依赖量最高，不适合当前已经稳定的 SD/AI 基础。

## 目标数据流

```text
CoreS3 SD（只读）
        ↓
文件流与音乐库
        ↓
HiFi 解码器适配层
        ↓
PCM 队列
   ┌────┴────┐
   ↓         ↓
AudioSink   FFT/频谱分析
   ↓         ↓
AW88298    LVGL 频谱界面
```

## 模块边界

### `media/storage`

- 复用已经验证的 CoreS3 SPI3 display/SD handoff；
- 只读挂载 SD；
- 扫描根目录、`/audiofiles` 和有限深度子目录；
- 支持 MP3、AAC、M4A、WAV、FLAC、OGG、OPUS；
- 禁止格式化、创建、删除和重命名 SD 文件。

### `media/library`

- 文件索引和路径；
- ID3 标题、艺术家、专辑；
- M3U 播放列表；
- 播放队列、上一首、下一首、随机和循环；
- 收藏及最近播放保存到设备 Flash/NVS，不写回共享 SD 卡。

### `media/decoder`

- 以 HiFi 最新 `main` 的解码支持为基线；
- 将 Arduino `File`/`FS` 接口适配到 StackChan SD 只读文件流；
- 解码器只产生 PCM，不直接初始化 CoreS3 I2S；
- 对不支持或损坏文件返回可显示的错误状态。

### `media/audio`

- PCM 队列送入 StackChan 现有 `AudioSink`；
- 由 CoreS3 AW88298 负责最终输出；
- 进入媒体模式时强制 0% 音量；
- 初期测试不播放声音，后续人工确认后才开放音量测试。

### `media/spectrum`

- 从 PCM 队列旁路一份数据给 FFT；
- 显示频谱柱、VU 电平和峰值保持；
- 控制刷新率，避免抢占 LVGL 和 AI 任务；
- 频谱基于解码 PCM，即使音量为 0% 也可以验证画面。

### `app_local_music`

- 按 StackChan 320×240 触摸屏重新布局；
- 文件夹、曲目列表、播放状态和控制按钮；
- 进度条、当前曲目、播放模式和频谱；
- 使用 CJK 字体显示中文/日文文件名；
- 保留上下滑动和返回功能。

## AI/媒体模式切换

进入媒体模式时保存 AI 状态，停止或暂停 AI 音频链路，取得音频硬件控制权；退出时停止播放器、释放缓冲和音频资源，再恢复 AI 音频、表情和舵机。两个模式不同时占用 I2S 和扬声器。

## 阶段计划

### Phase 1：真实播放基础

- SD 流式文件读取；
- MP3 解码；
- PCM 队列和 `AudioSink` 适配；
- 播放、停止、结束事件；
- 维持静音默认值。

### Phase 2：完整音乐库

- AAC/M4A/FLAC/OGG/OPUS/WAV；
- 子目录浏览；
- M3U；
- 播放队列；
- 上一首/下一首；
- ID3；
- 收藏和最近播放的 NVS 状态。

### Phase 3：完整 UI 与频谱

- 320×240 UI 重构；
- 播放进度与状态；
- 触摸控制；
- FFT/VU 频谱；
- 错误提示；
- CJK 长文件名；
- UI 视觉优化。

### Phase 4：稳定性验证

- AI/媒体模式恢复；
- 表情、舵机和 AI 语音回归；
- SD 与音频并发；
- 长时间播放；
- 断卡、损坏文件和无效 M3U；
- 保持音量 0% 的静音验证。

## 当前进度（2026-09-14）

已完成：

- CoreS3 SD SPI3 handoff 和只读挂载；
- FAT 长文件名配置；
- 多扩展名和子目录扫描基础；
- `LOCAL MUSIC` 滚动列表；
- CJK 文件名字体修正；
- COM6 固件烧录和 bootloader/应用/分区表/资源分区校验。

当前状态：

- 设备可以进入 `LOCAL MUSIC` 并滚动浏览；
- 默认音量为 0%；
- 尚未把 HiFi 解码器真正接入 StackChan 播放链路；
- 尚未开始 Phase 1 代码实施；
- 电台、频谱和完整播放器 UI 属于后续阶段。

## 验收标准

- 不插 ESP32 HiFi 开发板时，StackChan 可以独立完成本地播放；
- 不格式化、不写入共享 SD 卡；
- MP3 至少可完成流式解码、暂停、继续和结束；
- 真实中文、日文和英文文件名可见；
- 音乐库、M3U 和播放队列可操作；
- 频谱画面与 PCM 数据同步；
- 退出媒体模式后 AI、表情和舵机恢复；
- 全部硬件验证默认保持 0% 音量。
