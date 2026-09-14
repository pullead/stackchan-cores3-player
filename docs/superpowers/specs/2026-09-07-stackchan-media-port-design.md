# StackChan Local Music and Radio Port Design

Date: 2026-09-07
Status: Approved architecture, ready for implementation planning

## 1. Purpose

Port the local music player and internet radio capabilities, including their user interface, from `pullead/esp32-hifi` into the official `m5stack/StackChan` firmware for personal use.

The result must preserve StackChan's existing AI voice mode, avatar expressions, and servo behavior. Media playback and AI voice do not run concurrently. Entering AI mode saves media state and uses the existing warm-reboot/runtime handoff; returning to the normal application runtime may restore the saved media session. OTA is outside the initial scope.

## 2. Fixed Baselines

- Media source baseline: `pullead/esp32-hifi`, commit `55868f50adfcd63d872a82a429e5f2e4a83d1e5b`, branch `codex/local-library-v2-20260905`.
- Target baseline: `m5stack/StackChan`, commit `1b5765599fba8aaad1811d9a79358ccc7051f5f3`.
- Hardware: M5Stack CoreS3 with StackChan base.
- Target framework: ESP-IDF 5.5.4, LVGL 9, Mooncake application lifecycle.
- First output device: CoreS3 AW88298 internal speaker.
- Optional later output device: Waveshare ESP32-S3 HiFi board acting as a UAC1 USB audio device while retaining its existing UAC2 profile.

## 3. Scope

### Required

- Local files from CoreS3 microSD.
- WAV/PCM, MP3, AAC, and FLAC playback, introduced in that order.
- Folder/library browsing, metadata, favorites, play queue, play modes, seek, volume, and resume state.
- Internet radio over HTTP and HTTPS, playlist URL resolution, ICY metadata, station presets, reconnect, and buffering state.
- Media home, local library, radio list, and shared now-playing screens adapted to 320x240 LVGL 9.
- Preservation of avatar and servo update behavior in both normal and AI runtimes.
- Safe, explicit transition between media and Xiaozhi AI runtime.
- Hard-muted startup and test behavior: every boot and every automated audio test starts at 0% output volume.

### Deferred

- OTA and app-center firmware downloads.
- Simultaneous AI conversation and media playback.
- Cloud music, DLNA, FTP, USB MSC, scheduled discovery downloads, and automatic file deletion.
- Synchronized lyrics and high-refresh spectrum animation until playback stability is proven.
- External USB HiFi output until the built-in speaker path is stable.

## 4. Architectural Decision

Do not transplant the upstream `main.cpp` or LVGL 8 UI monolith. Extract media behavior behind target-owned interfaces and implement a native Mooncake application.

```text
AppMedia / LVGL 9 views
        |
MediaController  <---->  MediaStateStore
   |       |
   |       +---- RadioSource ---- NetworkPort
   +------------ LocalFileSource - StoragePort
        |
DecoderPipeline -> Resampler/Mixer -> AudioSink
                                      |
                         +------------+------------+
                         |                         |
                 CoreS3SpeakerSink          UsbHiFiSink (later)
```

All UI operations consume immutable snapshots and send commands. UI callbacks never perform filesystem, network, decode, or blocking audio work.

## 5. Runtime Modes

### Normal application runtime

Mooncake installs the launcher and `AppMedia`. Media playback may continue while switching between ordinary Mooncake screens, subject to an explicit background-play setting.

### Entering AI runtime

1. `AppMedia` receives a prepare-for-AI request.
2. The controller stops accepting new transport commands.
3. Current source, stable media identifier, position, queue, play mode, volume, and output sink are saved to NVS.
4. Decoder, network connection, files, and audio output are stopped in that order.
5. The existing Xiaozhi start request is issued only after the audio path reports idle.
6. Mooncake applications are uninstalled using the existing firmware flow.
7. Xiaozhi starts and owns the codec/I2S path.

### Returning from AI runtime

The existing warm-reboot path returns to Mooncake. `AppMedia` reads the saved session and shows a resumable card. Automatic audio resume is disabled by default; the user taps Resume to avoid unexpected sound after a reboot. The restored session does not restore an audible hardware volume: output remains at 0% until the user explicitly raises it.

This design preserves the existing Xiaozhi internals and avoids simultaneous ownership of I2S0.

## 6. Components

### AppMedia

- Implements the Mooncake `AppAbility` lifecycle.
- Owns LVGL objects only while open.
- Subscribes to controller snapshots and posts user commands.
- Releases subscriptions and transient views on close without destroying an allowed background media session.

### MediaController

- Sole owner of playback state and source transitions.
- Serializes `play`, `pause`, `seek`, `next`, source changes, and shutdown-for-AI commands.
- Exposes a compact immutable snapshot for UI updates.
- Enforces one active source and one active decoder pipeline.

Primary states:

```text
Idle -> Preparing -> Buffering -> Playing <-> Paused
                     |              |
                     +-> Error <----+
Any active state -> Stopping -> Idle
Any state -> PreparingForAI -> Idle
```

### LocalFileSource

- Opens files through `StoragePort`, not through direct SD globals.
- Supplies compressed bytes to the decoder.
- Implements seek only when the active format supports reliable byte/time mapping.
- Detects media removal and invalidates the active handle safely.

### RadioSource

- Resolves direct streams and M3U/PLS playlists.
- Handles HTTP redirects and TLS errors.
- Parses ICY metadata without blocking PCM consumption.
- Uses bounded reconnect with exponential backoff and a user-visible terminal error.
- Never runs discovery or download traffic while a station is active.

### DecoderPipeline

- Decoder code may be adapted from `esp32-hifi` for this personal-use build.
- The decoder must output PCM to a pull/buffer boundary and must not initialize or own I2S pins.
- Format rollout is WAV/PCM, MP3, AAC, then FLAC.
- Input PCM is normalized by a resampler/channel mixer before reaching the sink.

### AudioSink

```cpp
struct PcmFormat {
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits_per_sample;
};

class AudioSink {
public:
    virtual bool open(const PcmFormat& input) = 0;
    virtual size_t write(const int16_t* pcm, size_t frames) = 0;
    virtual void pause() = 0;
    virtual void flush() = 0;
    virtual void close() = 0;
    virtual ~AudioSink() = default;
};
```

`CoreS3SpeakerSink` feeds the existing target-owned codec path at 24 kHz, 16-bit mono. Stereo sources are downmixed and other sample rates are resampled before write. It does not recreate the target's I2S channels.

`UsbHiFiSink`, when implemented later, produces fixed 48 kHz, 16-bit stereo for the Waveshare UAC1 profile and falls back to the speaker after an orderly USB disconnect.

### StoragePort and LibraryRepository

- `StoragePort` owns CoreS3 microSD mount state, SPI serialization, file handles, and removal detection.
- LCD and SD share the SPI data/clock pins; long reads are chunked and never occur under an LVGL lock.
- `LibraryRepository` adapts the upstream fixed-record index and append-only event concepts.
- Scanning runs incrementally in a background task and yields frequently.
- A partially written index is committed by atomic rename.
- Favorites and resume events survive reset without rewriting the entire library.

### MediaStateStore

Persist to NVS only on meaningful transitions and with write coalescing:

- source type;
- file path hash or station identifier;
- playback position;
- queue and play mode;
- last requested volume for UI context, while applied hardware volume still resets to 0% on every boot and test start;
- last media screen;
- clean-shutdown marker for AI handoff.

Missing files or stations produce a nonfatal resumable-session error and clear only the invalid item.

## 7. UI Design

The UI is rebuilt for LVGL 9 and 320x240 while retaining the functional intent of the upstream screens.

### Media home

- Large Local Music and Radio entry cards.
- Compact Continue Playing card when a saved session exists.
- Persistent home/back affordance matching the official firmware.

### Local library

- Tabs or filters for Tracks, Artists, Albums, and Favorites.
- Virtualized/recycled rows; metadata and cover loading are asynchronous.
- Visible SD missing, scanning, and empty-library states.

### Radio list

- Station icon, name, and availability/status.
- Favorites first, followed by configured stations.
- One-tap play with immediate buffering feedback.

### Shared now-playing

- Source-specific artwork or station icon.
- Title and secondary metadata.
- Local progress/seek control or radio live indicator.
- Previous, play/pause, next/stop, volume, favorite, and output indicator.
- Optional low-rate VU/spectrum snapshot after stability milestones pass.

Avatar and servo updates remain active. Media screens do not directly command continuous servo motion; optional music-reactive behavior consumes rate-limited audio metrics through a separate adapter.

## 8. Concurrency and Resource Rules

- One media command queue serializes controller mutations.
- One decoder task owns codec state.
- One bounded PCM buffer separates decoding from output.
- Network and SD producers use bounded buffers and backpressure.
- Internal DMA-capable RAM is reserved for audio and USB; covers, library records, and UI caches prefer PSRAM.
- LVGL access always uses the existing target lock.
- No logging, floating-point FFT, file I/O, or UI work occurs in I2S/USB callbacks.
- Radio playback has priority over nonessential scans, metadata artwork fetches, and future downloads.
- Entering AI requires a confirmed idle audio sink before Xiaozhi starts.
- The codec output volume is set to 0% before output is enabled. Automated sine, PCM, DMA, decoder, USB, and long-run tests remain muted and verify counters/state rather than audible output.
- Producing audible test sound requires explicit user authorization at the time of that test; prior approval of this design is not authorization to unmute later.

## 9. Error Handling

- SD missing/removed: stop local playback, close handles, retain library metadata, and show a recoverable prompt.
- Unsupported/corrupt file: mark the item failed, advance according to play mode, and keep the UI responsive.
- Network loss: enter reconnecting state with bounded exponential backoff; allow immediate user cancellation.
- TLS/memory failure: free connection resources before retry and expose a concise diagnostic code.
- Decoder starvation: preserve buffered audio when possible, record underrun counters, and avoid restarting I2S.
- AI handoff timeout: cancel the handoff, return media to a stable paused state, and do not start Xiaozhi with live media resources.
- USB disconnect: flush USB output, switch to paused speaker state, then let the user resume.

## 10. Delivery Sequence

1. Baseline build, flash, resource measurements, and test scaffolding.
2. Audio ownership seam plus 24 kHz mono sine/PCM output through AW88298.
3. CoreS3 SPI microSD mount and sustained-read test while LVGL and servos update.
4. WAV local-playback vertical slice with minimal list and now-playing UI.
5. MP3, AAC, and FLAC decoder slices with seek and metadata.
6. Local library index, favorites, queue, play modes, and resume state.
7. Direct HTTP radio vertical slice, then HTTPS/playlists/ICY/reconnect.
8. Complete media UI, asynchronous artwork, diagnostics, and performance tuning.
9. AI handoff persistence, teardown, warm reboot, and resumable return card.
10. Long-run and fault-injection validation.
11. Optional Waveshare UAC1 profile and StackChan USB HiFi sink.

Each item is a separate implementation checkpoint; a failing checkpoint blocks later feature expansion.

## 11. Acceptance Criteria

- Existing AI voice mode starts and behaves as on the target baseline.
- Cold boot, warm reboot, AI return, media restore, and every automated audio test leave the applied output volume at 0%.
- No test raises hardware volume or produces intentional audible sound without explicit user authorization for that individual test run.
- Existing avatar expressions and servo updates work in normal and AI modes.
- Local playback runs for two hours without audible underrun or UI lockup.
- Radio runs for two hours and recovers from a temporary Wi-Fi interruption.
- Fifty local/radio source switches show no continuing loss of free internal RAM.
- SD removal, corrupt files, offline boot, TLS failure, and station failure do not reboot the device.
- Entering AI from media leaves no live decoder, file, network, or speaker owner.
- Returning from AI offers the saved media session and resumes only after user confirmation.
- Touch and servo behavior remain responsive during SD scan and radio buffering.
- UI callbacks perform no filesystem, network, or decoder operations.
- Optional USB output survives 30 minutes, disconnect/reconnect, and speaker fallback without duplicating the Waveshare ring/I2S backend.

## 12. Licensing and Distribution Boundary

This build is for personal use, so direct adaptation of GPL-3.0 upstream implementation is permitted for the intended use. Adapted files must retain upstream copyright and license notices. The repository must record which files or substantial sections originated from `esp32-hifi`.

If the firmware is later published, shared, sold, or bundled with hardware, distribution must stop until the combined-work GPL obligations and all third-party codec licenses are reviewed. The current personal-use decision must not be interpreted as permission to relicense adapted GPL implementation as MIT.
