# StackChan CoreS3 Local Player Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task.

**Goal:** Add the first real local-player slice to StackChan: stream one MP3 from the read-only CoreS3 SD mount through an adapted decoder/PCM pipeline into the existing StackChan audio sink, while keeping AI mode and the default 0% volume policy intact.

**Architecture:** Keep CoreS3 SPI3 SD handoff and StackChan's I2S/AW88298 ownership. Adapt the latest `pullead/esp32-hifi` decoder/file-flow concepts behind narrow StackChan interfaces; do not instantiate the HiFi Arduino `Audio` object or its I2S writer. `LOCAL MUSIC` will initially select and control one track, with the actual output forced muted.

**Tech Stack:** ESP-IDF 5.5.5, ESP32-S3, C++23, LVGL/smooth-ui-toolkit, FatFs VFS, existing `AudioSink`, host C++ tests, direct COM6 flash verification.

---

### Task 1: Define the decoder and stream boundary

**Files:**
- Create: `firmware/main/media/decoder/audio_stream.h`
- Create: `firmware/main/media/decoder/audio_decoder.h`
- Test: `firmware/tests/audio_stream_contract_test.cpp`
- Modify: `firmware/main/CMakeLists.txt`
- Modify: `firmware/tests/CMakeLists.txt`

- [ ] Write a host test that supplies a fake read-only stream and verifies that reads are bounded, EOF is reported, and no write operation exists in the interface.
- [ ] Run the focused host test and observe the expected compile failure because the new contract is absent.
- [ ] Define `AudioStream` with `read`, `seek`, `tell`, `size`, and `close` operations returning explicit status values; do not expose Arduino `File` or `FS` types.
- [ ] Define `AudioDecoder` with `open(AudioStream&)`, `decode(PcmBlock&)`, `format()`, `metadata()`, `eof()`, and `last_error()`; keep decoded PCM ownership explicit.
- [ ] Register the files in the IDF component and host-test CMake lists.
- [ ] Run the focused host test until it passes and commit `feat: define local decoder stream contracts`.

### Task 2: Adapt read-only SD paths to `AudioStream`

**Files:**
- Modify: `firmware/main/media/storage/sd_card_port.h`
- Modify: `firmware/main/media/storage/sd_card_port.cpp`
- Create: `firmware/main/media/storage/sd_audio_stream.h`
- Create: `firmware/main/media/storage/sd_audio_stream.cpp`
- Test: `firmware/tests/sd_audio_stream_test.cpp`

- [ ] Add a test fake for opening an indexed `SdTrack`, reading a bounded byte range, seeking within the file, and rejecting writes.
- [ ] Run the test before implementation and record the expected failure.
- [ ] Implement an SD-backed stream that opens only after the existing SPI3 display handoff is acquired and closes before the handoff is released.
- [ ] Keep `format_if_mount_failed=false`; do not add `mkdir`, `remove`, `rename`, or write access.
- [ ] Preserve UTF-8 long names and full paths from the existing scanner.
- [ ] Run focused tests and commit `feat: expose read-only SD audio streams`.

### Task 3: Add the first decoder adapter using the HiFi baseline

**Files:**
- Create: `firmware/main/media/decoder/hifi_decoder_adapter.h`
- Create: `firmware/main/media/decoder/hifi_decoder_adapter.cpp`
- Modify: `firmware/main/CMakeLists.txt`
- Modify: `firmware/dependencies.lock` only if the decoder dependency must be pinned; preserve unrelated existing edits
- Test: `firmware/tests/hifi_decoder_adapter_test.cpp`

- [ ] Add a deterministic test fixture for a small supported MP3 frame stream and assert metadata, PCM format, EOF and malformed-input errors.
- [ ] Run the test before implementation and record the expected failure.
- [ ] Pin the exact decoder dependency revision used by the latest HiFi baseline; do not consume an unpinned moving branch.
- [ ] Adapt decoder output to the new `AudioDecoder` interface without initializing I2S or changing volume.
- [ ] Add explicit format conversion/resampling only if the decoder output differs from the CoreS3 sink contract; keep this task limited to one format path.
- [ ] Run focused tests, then commit `feat: adapt hifi decoder to local audio interface`.

### Task 4: Bridge decoded PCM to the StackChan `AudioSink`

**Files:**
- Modify: `firmware/main/media/local/local_playback_controller.h`
- Modify: `firmware/main/media/local/local_playback_controller.cpp`
- Modify: `firmware/main/media/audio/audio_sink.h`
- Test: `firmware/tests/local_playback_controller_test.cpp`

- [ ] Extend the controller test with select/start/pump/EOF/error cases using a fake decoder, fake stream and fake sink; assert the sink receives PCM but the reported snapshot remains muted.
- [ ] Run the focused test before implementation and record the expected failure.
- [ ] Replace the current in-memory WAV-only selection path with a stream/decoder selection path while retaining the existing WAV tests as a compatibility path.
- [ ] Feed bounded PCM blocks to `AudioSink::write`; never buffer an entire song in RAM.
- [ ] Ensure `stop`, decoder error, EOF and `stop_for_ai` close the stream and sink deterministically.
- [ ] Run focused tests and commit `feat: stream decoded PCM through StackChan audio sink`.

### Task 5: Connect one-track controls to `LOCAL MUSIC`

**Files:**
- Modify: `firmware/main/apps/app_local_music/app_local_music.h`
- Modify: `firmware/main/apps/app_local_music/app_local_music.cpp`
- Modify: `firmware/main/apps/app_local_music/local_music_presenter.cpp`
- Test: `firmware/tests/local_music_presenter_test.cpp`

- [ ] Add a presenter test for a selected row showing title, path/status and muted playback state.
- [ ] Run the presenter test before implementation and record the expected failure.
- [ ] Change rows from passive labels to touchable controls backed by the selected `SdTrack` path.
- [ ] Add play/stop state and a compact progress/status area without changing the existing scroll behavior.
- [ ] Keep headings and rows on the CJK-capable font; keep all media controls muted.
- [ ] Run host tests and commit `feat: connect local music row controls`.

### Task 6: Add media/AI mode ownership and spectrum tap boundary

**Files:**
- Modify: `firmware/main/media/media_state_machine.*`
- Create: `firmware/main/media/audio/pcm_tap.h`
- Create: `firmware/main/media/audio/pcm_tap.cpp`
- Modify: `firmware/main/hal/board/cores3_audio_codec.cc`
- Test: `firmware/tests/media_mode_transition_test.cpp`
- Test: `firmware/tests/pcm_tap_test.cpp`

- [ ] Test that entering media mode saves AI state, stops AI audio ownership, and leaves the output muted.
- [ ] Run the tests before implementation and record the expected failure.
- [ ] Implement the smallest ownership transition needed for one-track playback; do not refactor unrelated AI behavior.
- [ ] Add a bounded PCM tap interface for a future FFT consumer, with drop/overrun counters instead of blocking the decoder.
- [ ] Keep spectrum computation out of the audio writer task.
- [ ] Run focused tests and commit `feat: reserve media mode and PCM spectrum tap`.

### Checkpoint: Phase 1 firmware validation

- [ ] Run all available host tests for media, SD, handoff, presenter and decoder contracts.
- [ ] Build the IDF image with the Python environment on PATH and capture the exit code.
- [ ] Flash only the application partitions to COM6 using the generated flash arguments; do not touch the SD card.
- [ ] Verify bootloader, application, partition table and assets with `esptool verify_flash`.
- [ ] Read serial logs and confirm `AudioCodec: Set output volume to 0` and `TF browse-only mount ready inside display handoff`.
- [ ] Manually enter `SETUP -> LOCAL MUSIC`, select one row, and verify no sound is emitted during the muted test.

## Explicit non-goals for Phase 1

- No radio/network playback.
- No SD-card writes, playlist persistence, formatting, rename or delete operations.
- No OTA changes.
- No final visual redesign of every StackChan page.
- No claim of audible playback until the user explicitly authorizes an unmuted test.
