# StackChan Local Music Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deliver a silent CoreS3-safe Local Music Mooncake app that first proves microSD browsing, then plays only compatible WAV PCM through the existing permanently muted audio sink.

**Architecture:** Keep RIFF parsing in a host-testable `WavReader`; make `SdCardPort` the only FATFS/SDSPI boundary; let `LocalPlaybackController` own reader, state machine and `AudioSink`. The Mooncake app renders a copy-only snapshot and never receives a codec/file/socket pointer. The SD browse hardware gate precedes every sink open/write because CoreS3 shares SPI3/GPIO35 between LCD and SD.

**Tech Stack:** ESP-IDF 5.5.5, C++17, FATFS/SDSPI, Mooncake, smooth UI toolkit/LVGL, existing `media::AudioSink` and `MediaStateMachine`.

---

## File structure

- `firmware/main/media/local/wav_reader.{h,cpp}`: safe RIFF/WAVE parser accepting only 24 kHz mono S16LE PCM.
- `firmware/main/media/local/local_playback_controller.{h,cpp}`: sink-owned bounded streaming and silent shutdown.
- `firmware/main/media/storage/sd_card_port.{h,cpp}`: CoreS3 FAT32 mount/list/unmount; never format on failure.
- `firmware/main/apps/app_local_music/app_local_music.{h,cpp}`: Mooncake lifecycle and compact track/status UI.
- `firmware/tests/wav_reader_test.cpp`, `firmware/tests/local_playback_controller_test.cpp`, `firmware/tests/sd_card_port_test.cpp`: host tests.
- `firmware/main/apps/apps.h`, `firmware/main/main.cpp`, and both CMake files: launcher registration and explicit dependencies.

### Task 1: Add a host-testable WAV parser

**Files:**
- Create: `firmware/main/media/local/wav_reader.h`
- Create: `firmware/main/media/local/wav_reader.cpp`
- Create: `firmware/tests/wav_reader_test.cpp`
- Modify: `firmware/tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing parser tests**

Use byte-vector fixtures. Assert `WavReader::open` accepts RIFF/WAVE PCM format 1, one channel, 24000 Hz, 16-bit samples, and finds `data` after an arbitrary `JUNK` chunk. Reject absent signatures, chunk length beyond input, stereo, 44100 Hz, 8-bit samples, and a truncated `fmt ` chunk.

```cpp
media::local::WavReader reader;
CHECK(reader.open(valid_bytes));
CHECK(reader.format() == media::PcmFormat{24000, 1, 16});
CHECK(reader.remaining_frames() == 2);
CHECK(!reader.open(stereo_bytes));
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake -S firmware -B firmware/build-host-local -DBUILD_TESTING=ON; cmake --build firmware/build-host-local --target wav_reader_test; firmware/build-host-local/wav_reader_test.exe`

Expected: missing target/source or failing assertions.

- [ ] **Step 3: Write minimal parser implementation**

Provide `open(std::span<const uint8_t>)`, `format()`, `data_offset()`, `remaining_frames()`, and `read_frames(int16_t*, size_t)`. Decode little-endian fields bytewise. Require PCM format 1, 1 channel, 24000 Hz, 16-bit, block align 2 and data size divisible by two. Skip each chunk only after checked `8 + size + (size & 1)` range arithmetic. Reset state before every open.

- [ ] **Step 4: Run tests**

Run: `cmake --build firmware/build-host-local --target wav_reader_test media_volume_policy_test media_state_machine_test media_speaker_sink_test; ctest --test-dir firmware/build-host-local --output-on-failure`

Expected: all tests pass.

- [ ] **Step 5: Commit**

```powershell
git add firmware/main/media/local/wav_reader.h firmware/main/media/local/wav_reader.cpp firmware/tests/wav_reader_test.cpp firmware/tests/CMakeLists.txt
git commit -m "feat: parse compatible local WAV files"
```

### Task 2: Add silent local playback control

**Files:**
- Create: `firmware/main/media/local/local_playback_controller.h`
- Create: `firmware/main/media/local/local_playback_controller.cpp`
- Create: `firmware/tests/local_playback_controller_test.cpp`
- Modify: `firmware/tests/CMakeLists.txt`

- [ ] **Step 1: Write the failing controller tests**

Create a `FakeSink` recording open/write/pause/flush/close. A compatible fixture must drive `Idle -> Preparing -> Buffering -> Playing`, write no more than `kPlaybackChunkFrames` per `pump()`, and return Idle after EOF. Malformed input never opens the sink; sink failure closes it; `stop_for_ai()` flushes before close.

```cpp
controller.select(valid_wav);
CHECK(controller.start());
controller.pump();
CHECK(fake_sink.open_calls == 1);
CHECK(fake_sink.written_frames > 0);
controller.stop_for_ai();
CHECK(fake_sink.events == std::vector<Event>{Event::Open, Event::Write, Event::Flush, Event::Close});
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build firmware/build-host-local --target local_playback_controller_test; firmware/build-host-local/local_playback_controller_test.exe`

Expected: missing target/source or failing assertions.

- [ ] **Step 3: Write minimal controller implementation**

Define `LocalPlaybackSnapshot { PlaybackState state; std::string title; size_t total_frames; size_t played_frames; std::string error; bool muted = true; }`. Inject `AudioSink&`; parse selected bytes in `start()`; open only `{24000,1,16}`; consume at most `kPlaybackChunkFrames = 1024` in each `pump()`. On EOF, error, stop, or AI handoff, transition through Stopping, flush, close, and return Idle. Do not expose a volume argument.

- [ ] **Step 4: Run tests**

Run: `cmake --build firmware/build-host-local --target local_playback_controller_test; ctest --test-dir firmware/build-host-local --output-on-failure`

Expected: all tests pass, including fake-sink close ordering.

- [ ] **Step 5: Commit**

```powershell
git add firmware/main/media/local/local_playback_controller.h firmware/main/media/local/local_playback_controller.cpp firmware/tests/local_playback_controller_test.cpp firmware/tests/CMakeLists.txt
git commit -m "feat: add muted local playback controller"
```

### Task 3: Implement the microSD browse-only hardware gate

**Files:**
- Create: `firmware/main/media/storage/sd_card_port.h`
- Create: `firmware/main/media/storage/sd_card_port.cpp`
- Create: `firmware/tests/sd_card_port_test.cpp`
- Modify: `firmware/main/hal/board/stackchan.cc`
- Modify: `firmware/main/CMakeLists.txt`
- Modify: `firmware/tests/CMakeLists.txt`

- [ ] **Step 1: Write pure filename-filtering tests**

Test `is_supported_wav_filename(std::string_view)` for case-insensitive `.wav`, no extension, `.wave`, and directory names. Define `SdTrack { std::string path; std::string title; size_t bytes; }` and `SdCardPort::{mount,unmount,is_mounted,list_tracks,last_error}`; its constructor must not initialize SPI.

- [ ] **Step 2: Run test to verify it fails**

Run: `cmake --build firmware/build-host-local --target sd_card_port_test; firmware/build-host-local/sd_card_port_test.exe`

Expected: missing target/source or failing assertions.

- [ ] **Step 3: Write the board-specific mount implementation**

Use `esp_vfs_fat_sdspi_mount("/sdcard", ...)` with `format_if_mount_failed = false`, max files 4, and `esp_vfs_fat_sdcard_unmount`. List root-only regular `.wav` files through `opendir/readdir`, cap at 64, and return error text without `ESP_ERROR_CHECK`. Add `sdmmc` and `esp_driver_sdspi` CMake dependencies. Extend the existing SPI3 bus only to expose MISO GPIO35; do not initialize SPI3 again or alter LCD CS/D-C/clock/panel config. Emit an INFO browse-gate log and do not construct an audio sink.

- [ ] **Step 4: Build and verify silently**

Run: `cmake --build firmware/build-host-local; ctest --test-dir firmware/build-host-local --output-on-failure; idf.py -B build-shortpath build`

Expected: host tests pass and ESP-IDF reports `Project build complete`.

- [ ] **Step 5: Flash the hardware gate**

Run: `idf.py -B build-shortpath -p COM6 flash`

With a FAT32 card inserted, inspect mount/list status and verify LCD/touch stability. Do not select a track or call a sink. Record only visual/log result in `firmware/docs/MEDIA_FOUNDATION_SMOKE_TEST.md`.

- [ ] **Step 6: Commit**

```powershell
git add firmware/main/media/storage firmware/main/hal/board/stackchan.cc firmware/main/CMakeLists.txt firmware/tests
git commit -m "feat: add CoreS3 SD music browse gate"
```

### Task 4: Register the Local Music Mooncake app

**Files:**
- Create: `firmware/main/apps/app_local_music/app_local_music.h`
- Create: `firmware/main/apps/app_local_music/app_local_music.cpp`
- Modify: `firmware/main/apps/apps.h`
- Modify: `firmware/main/main.cpp`
- Modify: `firmware/docs/MEDIA_FOUNDATION_SMOKE_TEST.md`

- [ ] **Step 1: Implement lifecycle-safe UI**

Set app name to `LOCAL MUSIC`. In `onOpen`, under `LvglLockGuard`, create title, status, eight bounded track rows and BACK. Mount/list before rendering. Each row only selects a controller track. `onRunning` reads a copy snapshot; `onClose` stops controller, unmounts card, then destroys LVGL objects. Render explicit no-card/no-WAV/mount-error states.

- [ ] **Step 2: Register and build**

Register `std::make_unique<AppLocalMusic>()` beside the existing apps. Keep the mute call and explicit AI request loop unchanged.

Run: `idf.py -B build-shortpath build; rg -n "AppLocalMusic|LOCAL MUSIC" firmware/main/main.cpp firmware/main/apps`

Expected: project build completes and one registration is found.

- [ ] **Step 3: Flash and run browse checkpoint**

Run: `idf.py -B build-shortpath -p COM6 flash`

Open LOCAL MUSIC with and without a FAT32 card. Verify title/status/back navigation and no audible output. Do not run playback.

- [ ] **Step 4: Commit**

```powershell
git add firmware/main/apps/app_local_music firmware/main/apps/apps.h firmware/main/main.cpp firmware/docs/MEDIA_FOUNDATION_SMOKE_TEST.md
git commit -m "feat: add Local Music launcher app"
```

### Task 5: Connect the silent WAV play action after the card gate passes

**Files:**
- Modify: `firmware/main/media/local/local_playback_controller.{h,cpp}`
- Modify: `firmware/main/media/storage/sd_card_port.{h,cpp}`
- Modify: `firmware/main/apps/app_local_music/app_local_music.cpp`
- Modify: `firmware/docs/MEDIA_FOUNDATION_SMOKE_TEST.md`

- [ ] **Step 1: Add file-backed tests**

Use a fake bounded byte reader and inject an I/O failure after one block. Assert selection enters Playing, frame count advances, I/O failure closes the sink then returns Idle, and no test path requests nonzero volume.

- [ ] **Step 2: Implement bounded file pumping**

Allow `SdCardPort` to open only tracks returned by its current list. Parse prefix, seek only after validation, and pump one bounded block per `onRunning`. A selected row displays `MUTED PLAYING <frames>`; every stop/back/error path stops controller before unmount.

- [ ] **Step 3: Build and verify**

Run: `cmake --build firmware/build-host-local; ctest --test-dir firmware/build-host-local --output-on-failure; idf.py -B build-shortpath build`

Expected: all tests pass and ESP-IDF reports `Project build complete`.

- [ ] **Step 4: Flash the silent playback checkpoint**

Run: `idf.py -B build-shortpath -p COM6 flash`

With a compliant 24 kHz mono 16-bit WAV on FAT32 microSD, select a track and verify muted-playing status/frame growth; return with BACK. Confirm silence. Update smoke-test record. Never change volume.

- [ ] **Step 5: Commit**

```powershell
git add firmware/main/media/local firmware/main/media/storage firmware/main/apps/app_local_music firmware/tests firmware/docs/MEDIA_FOUNDATION_SMOKE_TEST.md
git commit -m "feat: stream local WAV through muted sink"
```

## Plan self-review

- Spec coverage: Tasks 1-5 cover the approved Local Music hardware gate, compatible WAV parsing, muted stateful playback, launcher integration, and silent device checks. Internet Radio is intentionally a separate plan: storage/network/decoder work is independent and should not delay the SD hardware proof.
- Placeholder scan: no placeholder markers are present; every task names files, commands, expected result, and failure behavior.
- Type consistency: `WavReader`, `SdCardPort`, and `LocalPlaybackController` are specified before app use; `PcmFormat` and `AudioSink` reuse existing interfaces.

