# StackChan Media Apps Design

**Status:** Approved for implementation on 2026-09-08.

## Goal

Add two personal-use Mooncake applications to the official M5Stack StackChan CoreS3 firmware: **Local Music** for files on a microSD card and **Internet Radio** for saved stations. Preserve the existing AI voice, expression, and servo behavior by making media and AI mutually exclusive. All audio output remains hard-muted at 0 percent unless the user separately authorizes an audible test in that same conversation turn.

## Non-goals

- Do not retain OTA functionality.
- Do not port the ESP32-HiFi Arduino application or its GPL-3.0 `ESP32-audioI2S` runtime.
- Do not implement a universal media player, playlist service, Radio Browser search, station logos, HLS, or lossless codecs in the first releases.
- Do not provide a user-visible nonzero volume control.

## Shared architecture

`media::MediaStateMachine` remains the sole state authority. A `MediaController` owns the active source, decoder, worker task, and `AudioSink`; Mooncake views consume a copy-only `MediaSnapshot` and do not access codec, files, sockets, or decoder state. The existing CoreS3 sink is the only permitted route to the audio codec.

Every media path obeys the mute invariant:

1. Set codec output volume to `media::kMutedVolumePercent` before enabling output.
2. Clamp all requested media volume to zero.
3. Reapply zero volume on pause, stop, errors, and close.
4. Restore selection and progress only; never restore active output or playback automatically.

Switching to AI sends stop, re-mutes, flushes and closes the media pipeline, then releases the worker/source resources. Only after `PreparingForAi -> Stopping -> Idle` completes may the existing Xiaozhi start path run. Returning from AI enters the launcher with the last media selection preserved but stopped.

## Mooncake integration

Register `AppLocalMusic` and `AppInternetRadio` from `firmware/main/main.cpp`, beside the existing Mooncake app registrations. Each app follows `AppAbility` lifecycle conventions:

- `onCreate`: construct durable controller/repository state only.
- `onOpen`: create its LVGL view and request a bounded scan or connection.
- `onRunning`: refresh a snapshot; do not scan files or perform network reads here.
- `onClose`: synchronously request stop, destroy UI, and release its external resources.

All LVGL actions use `LvglLockGuard`. Apps use compact CoreS3-sized list and now-playing screens, rather than attempting to transplant HiFi's display layout.

## Local Music release sequence

### Hardware gate: microSD browse only

CoreS3 uses SPI3 pins CLK 36, MOSI 37, MISO 35, CS 4 for microSD, while the current display configuration uses SPI3 and GPIO 35 for LCD D/C. The implementation therefore requires a board-owned `Spi3DisplayHandoff` in addition to `SdCardPort`. Storage code must never access SPI3 outside this handoff and must not reinitialize the bus or format a card on mount failure.

Before every SD transaction, the handoff locks LVGL, drains queued LCD DMA, and leaves GPIO35 as an input for SD MISO. SD CS must be high before the handoff releases the display lock; the next LCD transaction may then drive GPIO35 as D/C through the ESP-IDF LCD pre/post callbacks. Directory scans mount, scan, and unmount while the display is paused. Later file streaming uses bounded 4-16 KiB reads through the same handoff so the display can refresh between reads.

The first device checkpoint mounts a FAT32 microSD card, lists a bounded number of supported files, unmounts it, and verifies the LCD/touch stay usable through repeated scans. No `AudioSink::open` or PCM output is allowed in this checkpoint. Any LCD corruption, SD CRC/I/O error, reset, or failure to restore touch/display blocks playback work.

### Silent WAV vertical slice

After the hardware gate, support only RIFF/WAVE PCM containing 16-bit little-endian, one-channel, 24,000 Hz samples. `WavReader` parses chunks safely, skips unknown chunks, rejects malformed/truncated files and unsupported formats, and streams bounded PCM blocks through the existing muted `CoreS3SpeakerSink`.

`LocalPlaybackController` maps open, buffering, playing, EOF, user stop, error, and AI handoff into the existing media state machine. A real-device check uses state labels and frame counters only; silence is expected and required.

## Internet Radio release sequence

Internet Radio follows Local Music, reusing `MediaController` and snapshot/UI patterns.

1. `StationRepository` supplies a small firmware-default station catalog plus NVS-saved personal stations. It validates IDs, labels, and HTTP(S) URLs with bounded lengths. Its first app version browses/selects stations but does not decode or output audio.
2. `HttpStreamSource` performs bounded HTTP connection and redirect handling in a worker task, has header/body/idle timeouts and a bounded ring buffer, and reports connection/buffer/error state through the snapshot. UI never manipulates a socket.
3. Add a license-reviewed MP3 decoder, PCM downmix/resampling to 24 kHz mono S16LE, and silent frame-count/buffer verification. AAC, FLAC, Opus, playlist formats, ICY title metadata, HTTPS exceptions, Radio Browser lookup, and artwork each remain separate later increments.

## Verification

Host tests cover WAV headers (valid, extended/unknown chunks, truncation and rejected formats), catalog validation, controller state transitions, and fake sink call order. Tests assert every observed volume request is zero.

Device checkpoints are manual and silent:

1. Boot into the Mooncake function menu and open/close both media apps.
2. With a FAT32 card inserted, mount/list files and verify screen/touch are stable.
3. Select a compliant WAV and observe `Playing` plus increasing frame count with no audible sound; stop and return to launcher.
4. Connect a saved radio station and observe connection/buffer/decoder status with no audible sound.

Any audible test is excluded from these checkpoints and needs a fresh explicit user authorization.
