# StackChan Media Foundation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Establish a host-tested, always-muted media foundation that can later accept local-file and radio decoders without competing with the official StackChan AI audio path.

**Architecture:** Add a target-neutral media core under `firmware/main/media`, inject the board codec through a narrow port, and make the speaker sink enforce mute-before-enable. This first plan deliberately stops before SD, decoding, radio, and UI so the highest-risk audio ownership and safety rules are proven first.

**Tech Stack:** ESP-IDF 5.5.4, C++17, existing host-side CMake/CTest harness, Mooncake firmware, CoreS3 `AudioCodec`/AW88298 path.

---

## Program Sequence

The approved design is split into independently executable plans because storage, local decoding, radio, UI, AI handoff, and USB are separate failure domains.

1. This plan: media types, state machine, safe volume, sink boundary, host tests, and firmware build.
2. CoreS3 SPI microSD and WAV local-playback vertical slice.
3. MP3/AAC/FLAC, library index, metadata, favorites, queue, and resume.
4. HTTP/HTTPS radio, playlists, ICY metadata, buffering, and reconnect.
5. LVGL 9 `AppMedia` screens and launcher integration.
6. AI handoff persistence and warm-reboot restore card.
7. Long-run/fault validation and performance tuning.
8. Optional Waveshare UAC1 profile and `UsbHiFiSink`.

Later plans are written only after the preceding hardware checkpoint reports its real memory, timing, and ownership results. OTA is not removed in this plan because leaving unused OTA code in place is lower risk than changing partitions before flash pressure is measured.

## Planned File Structure

```text
firmware/main/media/
  audio/
    audio_codec_port.h          board-independent codec contract
    audio_sink.h                PCM sink contract
    board_audio_codec_port.h    CoreS3 AudioCodec adapter declaration
    board_audio_codec_port.cpp  CoreS3 AudioCodec adapter implementation
    core_s3_speaker_sink.h      safe internal-speaker sink declaration
    core_s3_speaker_sink.cpp    mute-before-enable sink implementation
    volume_policy.h             startup/test/user-volume policy
  media_state_machine.h         legal media state transitions
  media_state_machine.cpp
  media_types.h                 shared PCM/source/state types

firmware/tests/
  media_volume_policy_test.cpp
  media_state_machine_test.cpp
  media_speaker_sink_test.cpp
```

## Task 1: Add the hard-mute volume policy

**Files:**
- Create: `firmware/main/media/audio/volume_policy.h`
- Create: `firmware/tests/media_volume_policy_test.cpp`
- Modify: `firmware/tests/CMakeLists.txt`

- [ ] **Step 1: Add the failing host test**

Create `firmware/tests/media_volume_policy_test.cpp`:

```cpp
#include <cassert>
#include <cstdint>
#include "media/audio/volume_policy.h"

int main()
{
    static_assert(media::kMutedVolumePercent == 0);
    assert(media::startup_volume(0) == 0);
    assert(media::startup_volume(70) == 0);
    assert(media::test_volume(100) == 0);
    assert(media::clamp_user_volume(-1) == 0);
    assert(media::clamp_user_volume(42) == 42);
    assert(media::clamp_user_volume(101) == 100);
    return 0;
}
```

Append this target to `firmware/tests/CMakeLists.txt`:

```cmake
add_executable(media_volume_policy_test
    media_volume_policy_test.cpp
)

target_include_directories(media_volume_policy_test PRIVATE
    ../main
)

target_compile_features(media_volume_policy_test PRIVATE cxx_std_17)
add_test(NAME media_volume_policy_test COMMAND $<TARGET_FILE:media_volume_policy_test>)
```

- [ ] **Step 2: Run the test to verify it fails**

Run from repository root:

```powershell
cmake -S firmware/tests -B firmware/build-host-tests
cmake --build firmware/build-host-tests --target media_volume_policy_test
```

Expected: compilation fails because `media/audio/volume_policy.h` does not exist.

- [ ] **Step 3: Implement the minimal policy**

Create `firmware/main/media/audio/volume_policy.h`:

```cpp
#pragma once
#include <cstdint>

namespace media {

inline constexpr uint8_t kMutedVolumePercent = 0;

constexpr uint8_t startup_volume(int) noexcept
{
    return kMutedVolumePercent;
}

constexpr uint8_t test_volume(int) noexcept
{
    return kMutedVolumePercent;
}

constexpr uint8_t clamp_user_volume(int value) noexcept
{
    return value < 0 ? 0 : value > 100 ? 100 : static_cast<uint8_t>(value);
}

}  // namespace media
```

- [ ] **Step 4: Run the focused and complete host suites**

```powershell
cmake --build firmware/build-host-tests --target media_volume_policy_test
ctest --test-dir firmware/build-host-tests -R media_volume_policy_test --output-on-failure
ctest --test-dir firmware/build-host-tests --output-on-failure
```

Expected: `media_volume_policy_test` passes and the existing motion test still passes.

- [ ] **Step 5: Commit the policy and test**

```powershell
git add firmware/main/media/audio/volume_policy.h firmware/tests/media_volume_policy_test.cpp firmware/tests/CMakeLists.txt
git commit -m "test: define muted media volume policy"
```

## Task 2: Define media types and legal state transitions

**Files:**
- Create: `firmware/main/media/media_types.h`
- Create: `firmware/main/media/media_state_machine.h`
- Create: `firmware/main/media/media_state_machine.cpp`
- Create: `firmware/tests/media_state_machine_test.cpp`
- Modify: `firmware/tests/CMakeLists.txt`

- [ ] **Step 1: Add a failing transition test**

Create `firmware/tests/media_state_machine_test.cpp`:

```cpp
#include <cassert>
#include "media/media_state_machine.h"

int main()
{
    media::MediaStateMachine machine;
    assert(machine.state() == media::PlaybackState::Idle);
    assert(machine.transition(media::PlaybackState::Preparing));
    assert(machine.transition(media::PlaybackState::Buffering));
    assert(machine.transition(media::PlaybackState::Playing));
    assert(machine.transition(media::PlaybackState::Paused));
    assert(machine.transition(media::PlaybackState::Playing));
    assert(machine.transition(media::PlaybackState::PreparingForAi));
    assert(machine.transition(media::PlaybackState::Stopping));
    assert(machine.transition(media::PlaybackState::Idle));

    media::MediaStateMachine invalid;
    assert(!invalid.transition(media::PlaybackState::Playing));
    assert(invalid.state() == media::PlaybackState::Idle);

    media::MediaStateMachine failed;
    assert(failed.transition(media::PlaybackState::Preparing));
    assert(failed.transition(media::PlaybackState::Error));
    assert(failed.transition(media::PlaybackState::Stopping));
    assert(failed.transition(media::PlaybackState::Idle));
    return 0;
}
```

Add to `firmware/tests/CMakeLists.txt`:

```cmake
add_executable(media_state_machine_test
    media_state_machine_test.cpp
    ../main/media/media_state_machine.cpp
)

target_include_directories(media_state_machine_test PRIVATE ../main)
target_compile_features(media_state_machine_test PRIVATE cxx_std_17)
add_test(NAME media_state_machine_test COMMAND $<TARGET_FILE:media_state_machine_test>)
```

- [ ] **Step 2: Verify the test fails before implementation**

```powershell
cmake -S firmware/tests -B firmware/build-host-tests
cmake --build firmware/build-host-tests --target media_state_machine_test
```

Expected: compilation fails because the media types and state machine do not exist.

- [ ] **Step 3: Add the shared types**

Create `firmware/main/media/media_types.h`:

```cpp
#pragma once
#include <cstdint>

namespace media {

enum class MediaSource : uint8_t { None, LocalFile, Radio };

enum class PlaybackState : uint8_t {
    Idle,
    Preparing,
    Buffering,
    Playing,
    Paused,
    Error,
    Stopping,
    PreparingForAi,
};

struct PcmFormat {
    uint32_t sample_rate = 0;
    uint8_t channels = 0;
    uint8_t bits_per_sample = 0;

    constexpr bool valid() const noexcept
    {
        return sample_rate > 0 && (channels == 1 || channels == 2) && bits_per_sample == 16;
    }
};

}  // namespace media
```

- [ ] **Step 4: Implement the state machine**

Create `firmware/main/media/media_state_machine.h`:

```cpp
#pragma once
#include "media_types.h"

namespace media {

class MediaStateMachine {
public:
    PlaybackState state() const noexcept { return state_; }
    bool transition(PlaybackState next) noexcept;

private:
    PlaybackState state_ = PlaybackState::Idle;
};

}  // namespace media
```

Create `firmware/main/media/media_state_machine.cpp`:

```cpp
#include "media_state_machine.h"

namespace media {

bool MediaStateMachine::transition(PlaybackState next) noexcept
{
    const auto current = state_;
    const bool allowed =
        (current == PlaybackState::Idle && next == PlaybackState::Preparing) ||
        (current == PlaybackState::Preparing && (next == PlaybackState::Buffering || next == PlaybackState::Error || next == PlaybackState::Stopping)) ||
        (current == PlaybackState::Buffering && (next == PlaybackState::Playing || next == PlaybackState::Error || next == PlaybackState::Stopping)) ||
        (current == PlaybackState::Playing && (next == PlaybackState::Paused || next == PlaybackState::Buffering || next == PlaybackState::Error || next == PlaybackState::Stopping || next == PlaybackState::PreparingForAi)) ||
        (current == PlaybackState::Paused && (next == PlaybackState::Playing || next == PlaybackState::Stopping || next == PlaybackState::PreparingForAi)) ||
        (current == PlaybackState::Error && next == PlaybackState::Stopping) ||
        (current == PlaybackState::PreparingForAi && next == PlaybackState::Stopping) ||
        (current == PlaybackState::Stopping && next == PlaybackState::Idle);

    if (allowed) {
        state_ = next;
    }
    return allowed;
}

}  // namespace media
```

- [ ] **Step 5: Run tests and commit**

```powershell
cmake --build firmware/build-host-tests --target media_state_machine_test
ctest --test-dir firmware/build-host-tests --output-on-failure
git add firmware/main/media/media_types.h firmware/main/media/media_state_machine.h firmware/main/media/media_state_machine.cpp firmware/tests/media_state_machine_test.cpp firmware/tests/CMakeLists.txt
git commit -m "feat: add media playback state machine"
```

Expected: all host tests pass before the commit.

## Task 3: Define and test a mute-before-enable speaker sink

**Files:**
- Create: `firmware/main/media/audio/audio_codec_port.h`
- Create: `firmware/main/media/audio/audio_sink.h`
- Create: `firmware/main/media/audio/core_s3_speaker_sink.h`
- Create: `firmware/main/media/audio/core_s3_speaker_sink.cpp`
- Create: `firmware/tests/media_speaker_sink_test.cpp`
- Modify: `firmware/tests/CMakeLists.txt`

- [ ] **Step 1: Add the failing sink safety test**

Create `firmware/tests/media_speaker_sink_test.cpp`:

```cpp
#include <cassert>
#include <cstdint>
#include <vector>
#include "media/audio/core_s3_speaker_sink.h"

class FakeCodecPort final : public media::AudioCodecPort {
public:
    void set_volume(uint8_t volume) override
    {
        calls.push_back(volume == 0 ? 1 : 99);
        last_volume = volume;
    }

    bool enable_output(bool enabled) override
    {
        calls.push_back(enabled ? 2 : 4);
        output_enabled = enabled;
        return true;
    }

    size_t write_mono(const int16_t*, size_t frames) override
    {
        calls.push_back(3);
        return frames;
    }

    std::vector<int> calls;
    uint8_t last_volume = 100;
    bool output_enabled = false;
};

int main()
{
    FakeCodecPort codec;
    media::CoreS3SpeakerSink sink(codec);
    assert(sink.open({24000, 1, 16}));
    assert((codec.calls == std::vector<int>{1, 2}));
    assert(codec.last_volume == 0);

    const int16_t silence[4]{};
    assert(sink.write(silence, 4) == 4);
    assert(codec.calls.back() == 3);

    sink.close();
    assert(codec.calls.back() == 4);
    assert(codec.last_volume == 0);

    FakeCodecPort wrong_format;
    media::CoreS3SpeakerSink rejected(wrong_format);
    assert(!rejected.open({48000, 2, 16}));
    assert(wrong_format.calls.empty());
    return 0;
}
```

Add to `firmware/tests/CMakeLists.txt`:

```cmake
add_executable(media_speaker_sink_test
    media_speaker_sink_test.cpp
    ../main/media/audio/core_s3_speaker_sink.cpp
)

target_include_directories(media_speaker_sink_test PRIVATE ../main)
target_compile_features(media_speaker_sink_test PRIVATE cxx_std_17)
add_test(NAME media_speaker_sink_test COMMAND $<TARGET_FILE:media_speaker_sink_test>)
```

- [ ] **Step 2: Verify the test fails**

```powershell
cmake -S firmware/tests -B firmware/build-host-tests
cmake --build firmware/build-host-tests --target media_speaker_sink_test
```

Expected: compilation fails because the sink interfaces do not exist.

- [ ] **Step 3: Add codec and sink interfaces**

Create `firmware/main/media/audio/audio_codec_port.h`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>

namespace media {

class AudioCodecPort {
public:
    virtual void set_volume(uint8_t volume) = 0;
    virtual bool enable_output(bool enabled) = 0;
    virtual size_t write_mono(const int16_t* samples, size_t frames) = 0;
    virtual ~AudioCodecPort() = default;
};

}  // namespace media
```

Create `firmware/main/media/audio/audio_sink.h`:

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include "media/media_types.h"

namespace media {

class AudioSink {
public:
    virtual bool open(const PcmFormat& format) = 0;
    virtual size_t write(const int16_t* pcm, size_t frames) = 0;
    virtual void pause() = 0;
    virtual void flush() = 0;
    virtual void close() = 0;
    virtual ~AudioSink() = default;
};

}  // namespace media
```

- [ ] **Step 4: Implement the CoreS3 speaker sink**

Create `firmware/main/media/audio/core_s3_speaker_sink.h`:

```cpp
#pragma once
#include "audio_codec_port.h"
#include "audio_sink.h"

namespace media {

class CoreS3SpeakerSink final : public AudioSink {
public:
    explicit CoreS3SpeakerSink(AudioCodecPort& codec) : codec_(codec) {}
    bool open(const PcmFormat& format) override;
    size_t write(const int16_t* pcm, size_t frames) override;
    void pause() override;
    void flush() override;
    void close() override;

private:
    AudioCodecPort& codec_;
    bool open_ = false;
};

}  // namespace media
```

Create `firmware/main/media/audio/core_s3_speaker_sink.cpp`:

```cpp
#include "core_s3_speaker_sink.h"
#include "volume_policy.h"

namespace media {

bool CoreS3SpeakerSink::open(const PcmFormat& format)
{
    if (!format.valid() || format.sample_rate != 24000 || format.channels != 1) {
        return false;
    }
    codec_.set_volume(kMutedVolumePercent);
    open_ = codec_.enable_output(true);
    return open_;
}

size_t CoreS3SpeakerSink::write(const int16_t* pcm, size_t frames)
{
    return open_ && pcm != nullptr ? codec_.write_mono(pcm, frames) : 0;
}

void CoreS3SpeakerSink::pause()
{
    codec_.set_volume(kMutedVolumePercent);
}

void CoreS3SpeakerSink::flush()
{
}

void CoreS3SpeakerSink::close()
{
    codec_.set_volume(kMutedVolumePercent);
    if (open_) {
        codec_.enable_output(false);
        open_ = false;
    }
}

}  // namespace media
```

- [ ] **Step 5: Run tests and commit**

```powershell
cmake --build firmware/build-host-tests --target media_speaker_sink_test
ctest --test-dir firmware/build-host-tests --output-on-failure
git add firmware/main/media/audio firmware/tests/media_speaker_sink_test.cpp firmware/tests/CMakeLists.txt
git commit -m "feat: add muted CoreS3 speaker sink boundary"
```

Expected: every host test passes; the fake codec call sequence proves volume 0 is applied before output enable.

## Task 4: Connect the sink boundary to the official board codec

**Files:**
- Create: `firmware/main/media/audio/board_audio_codec_port.h`
- Create: `firmware/main/media/audio/board_audio_codec_port.cpp`
- Modify: `firmware/main/CMakeLists.txt`

- [ ] **Step 1: Create the board adapter declaration**

Create `firmware/main/media/audio/board_audio_codec_port.h`:

```cpp
#pragma once
#include "audio_codec_port.h"

class AudioCodec;

namespace media {

class BoardAudioCodecPort final : public AudioCodecPort {
public:
    explicit BoardAudioCodecPort(AudioCodec& codec) : codec_(codec) {}
    void set_volume(uint8_t volume) override;
    bool enable_output(bool enabled) override;
    size_t write_mono(const int16_t* samples, size_t frames) override;

private:
    AudioCodec& codec_;
};

}  // namespace media
```

- [ ] **Step 2: Implement the adapter without creating I2S resources**

Create `firmware/main/media/audio/board_audio_codec_port.cpp`:

```cpp
#include "board_audio_codec_port.h"
#include <audio/audio_codec.h>
#include <vector>

namespace media {

void BoardAudioCodecPort::set_volume(uint8_t volume)
{
    codec_.SetOutputVolume(volume);
}

bool BoardAudioCodecPort::enable_output(bool enabled)
{
    codec_.EnableOutput(enabled);
    return codec_.output_enabled() == enabled;
}

size_t BoardAudioCodecPort::write_mono(const int16_t* samples, size_t frames)
{
    std::vector<int16_t> chunk(samples, samples + frames);
    codec_.OutputData(chunk);
    return frames;
}

}  // namespace media
```

This adapter calls only the existing `AudioCodec` API. It must not include `driver/i2s_std.h`, call `i2s_new_channel`, or change the configured 24 kHz format.

- [ ] **Step 3: Add media sources to the firmware component**

In `firmware/main/CMakeLists.txt`, add these patterns to `STACK_CHAN_SOURCES` immediately after the existing `hal/*.cpp` pattern:

```cmake
    "media/*.c"
    "media/*.cc"
    "media/*.cpp"
```

The existing recursive glob then includes nested `media/audio` files.

- [ ] **Step 4: Build host tests and firmware**

```powershell
ctest --test-dir firmware/build-host-tests --output-on-failure
idf.py -C firmware build
```

Expected: all host tests pass and ESP-IDF prints `Project build complete`.

- [ ] **Step 5: Commit the board adapter**

```powershell
git add firmware/main/media/audio/board_audio_codec_port.h firmware/main/media/audio/board_audio_codec_port.cpp firmware/main/CMakeLists.txt
git commit -m "feat: adapt media sink to StackChan audio codec"
```

## Task 5: Enforce muted boot and muted built-in microphone tests

**Files:**
- Modify: `firmware/main/main.cpp`
- Modify: `firmware/main/hal/board/stackchan.cc`
- Modify: `firmware/main/hal/audio.cpp`
- Modify: `firmware/main/apps/app_setup/workers/audio.cpp`
- Modify: `firmware/main/apps/app_setup/workers/workers.h`

- [ ] **Step 1: Apply mute immediately after HAL initialization**

Add this include to `firmware/main/main.cpp`:

```cpp
#include <media/audio/volume_policy.h>
```

Immediately after `GetHAL().init();`, add:

```cpp
GetHAL().setSpeakerVolume(media::kMutedVolumePercent, false);
```

This executes before the Mooncake/Xiaozhi branch, so both cold-start paths begin muted.

- [ ] **Step 2: Permit persisted/default volume to be exactly zero**

Replace `hal_bridge::board_get_speaker_volume()` in `firmware/main/hal/board/stackchan.cc` with:

```cpp
uint8_t hal_bridge::board_get_speaker_volume()
{
    Settings settings("audio", false);
    const int volume = settings.GetInt("output_volume", 0);
    return static_cast<uint8_t>(std::clamp(volume, 0, 100));
}
```

Ensure `<algorithm>` is included in that translation unit. This removes the current behavior that turns a stored zero into 10%.

- [ ] **Step 3: Make the setup microphone test silent**

In `firmware/main/apps/app_setup/workers/audio.cpp`, include the policy and replace the constructor/destructor volume behavior:

```cpp
#include <media/audio/volume_policy.h>

MicTestWorker::MicTestWorker()
{
    GetHAL().setSpeakerVolume(media::kMutedVolumePercent, false);
    _waveform_frame.resize(_waveform_point_count, 0);
    // Existing UI construction remains unchanged.
}

MicTestWorker::~MicTestWorker()
{
    GetHAL().clearupMicTest();
    GetHAL().setSpeakerVolume(media::kMutedVolumePercent, false);
}
```

Keep the existing UI construction between those shown constructor statements. Remove `_original_volume` from `MicTestWorker` in `firmware/main/apps/app_setup/workers/workers.h`.

- [ ] **Step 4: Mute directly around codec playback**

In `Hal::startMicTest()` immediately before `audio_codec->EnableOutput(true)`, add:

```cpp
setSpeakerVolume(media::kMutedVolumePercent, false);
```

Add `#include <media/audio/volume_policy.h>` to `firmware/main/hal/audio.cpp`. At the end of `Hal::clearupMicTest()`, add the same `setSpeakerVolume` call. The microphone waveform remains testable, but playback is electrically enabled only at volume 0.

- [ ] **Step 5: Prove there is no test-time unmute path**

Run:

```powershell
rg -n "setSpeakerVolume\(100|SetOutputVolume\(100|_original_volume" firmware/main/apps/app_setup firmware/main/hal/audio.cpp
```

Expected: no matches.

Run:

```powershell
ctest --test-dir firmware/build-host-tests --output-on-failure
idf.py -C firmware build
```

Expected: all host tests pass and the firmware build completes.

- [ ] **Step 6: Commit the safety integration**

```powershell
git add firmware/main/main.cpp firmware/main/hal/board/stackchan.cc firmware/main/hal/audio.cpp firmware/main/apps/app_setup/workers/audio.cpp firmware/main/apps/app_setup/workers/workers.h
git commit -m "fix: keep boot and audio diagnostics muted"
```

## Task 6: Perform a muted hardware smoke test

**Files:**
- Create: `firmware/docs/MEDIA_FOUNDATION_SMOKE_TEST.md`

- [ ] **Step 1: Record the test contract before flashing**

Create `firmware/docs/MEDIA_FOUNDATION_SMOKE_TEST.md` with this checklist:

```markdown
# Media Foundation Smoke Test

- Firmware commit:
- Date and device identifier:
- Cold boot emitted no intentional sound: pass/fail
- Applied speaker volume after boot is 0%: pass/fail
- Mooncake launcher opens: pass/fail
- Avatar updates: pass/fail
- Yaw and pitch servos respond: pass/fail
- Setup microphone waveform updates: pass/fail
- Setup microphone playback remains inaudible at 0%: pass/fail
- Entering Xiaozhi emitted no intentional sound before user volume change: pass/fail
- Returning to Mooncake retains 0% applied volume: pass/fail
- Free internal heap and largest DMA block before test:
- Free internal heap and largest DMA block after ten minutes:
- Reset, watchdog, or assertion observed: yes/no
```

- [ ] **Step 2: Build and flash without changing volume**

```powershell
idf.py -C firmware build
idf.py -C firmware flash monitor
```

Expected: flash succeeds, the device boots, and no intentional audible sound is produced. Do not touch the volume control during this smoke test.

- [ ] **Step 3: Exercise only silent paths**

Open the launcher, move the avatar/servos through existing controls, open the setup microphone page, observe the waveform, run the microphone test, enter AI mode, and return to Mooncake. Record all checklist fields. Do not authorize or perform an audible sine, music, notification, or microphone-playback test.

- [ ] **Step 4: Review the checkpoint**

Stop if any of these occur:

- applied volume is above 0% without a direct user adjustment;
- any intentional audio is audible;
- AI, avatar, or servos regress;
- I2S initialization is duplicated;
- internal heap continuously decreases during the ten-minute observation.

The SD/WAV plan may begin only when this checkpoint passes.

- [ ] **Step 5: Commit the filled smoke-test record**

```powershell
git add firmware/docs/MEDIA_FOUNDATION_SMOKE_TEST.md
git commit -m "test: record muted media foundation smoke test"
```

## Final Verification

- [ ] `ctest --test-dir firmware/build-host-tests --output-on-failure` reports zero failed tests.
- [ ] `idf.py -C firmware build` reports `Project build complete`.
- [ ] `git diff --check` reports no whitespace errors.
- [ ] `git status --short` is empty after the final commit.
- [ ] Boot, microphone diagnostics, and AI entry remain at applied volume 0%.
- [ ] No audible test was performed without a new, explicit user authorization.
- [ ] AI entry, avatar updates, and servo behavior match the target baseline.
- [ ] The filled smoke-test record contains no blank result fields.
