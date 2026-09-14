# ESP32-audioI2S backend provenance

Status: **build-gated, not enabled in the default StackChan image**.

The latest `esp32-hifi` application declares `https://github.com/schreibfaul1/ESP32-audioI2S` through PlatformIO's moving `lib_deps` URL.  That is not a reproducible dependency declaration, so this port does not silently vendor the library or turn it on in the default firmware.

The decoder boundary in `firmware/main/media/decoder/hifi_decoder_adapter.*` is deliberately narrower than the upstream `Audio` class.  An eventual bridge must:

- consume only the supplied read-only `AudioStream`;
- return decoded PCM and metadata through `HifiDecoderBackend`;
- never create or configure I2S, an `AudioSink`, SD storage, or volume state;
- propagate EOF and input/I/O errors without treating an error as EOF.

This distinction is required because `ESP32-audioI2S`'s normal API opens an Arduino `FS` object and owns its own I2S output task.  Passing StackChan's `SdAudioStream` into that API would bypass the CoreS3 SPI3/display handoff and would create a second audio owner.  Reusing the decoder therefore requires an upstream-library adapter or a separately maintained decoder-only extraction; neither is safe to claim as implemented without compiling and testing the exact pinned source.

## Explicit enable contract

The optional build path is enabled only when both conditions hold:

```text
CONFIG_STACKCHAN_HIFI_AUDIOI2S_BACKEND=y
STACKCHAN_AUDIOI2S_BACKEND_TARGET=<audited CMake target>
```

The target must export `stackchan_create_audioi2s_backend()` and implement the interface above.  `firmware/main/CMakeLists.txt` rejects an enable request with no target.  The default build returns `nullptr`; this is intentional and prevents a fake MP3 success path.

## Existing Espressif decoder alternative

The repository already resolves `espressif/esp_audio_codec` **2.4.1**, with component provenance `4d5cbe02f59fb45e40112d63317a8ddd00019cd4` in `firmware/managed_components/espressif__esp_audio_codec/idf_component.yml` and the corresponding fixed component entry in `firmware/dependencies.lock`.  This component does include an MP3 decoder: `include/decoder/impl/esp_mp3_dec.h` exposes `esp_mp3_dec_open`, `esp_mp3_dec_decode`, `esp_mp3_dec_reset`, and `esp_mp3_dec_close`; the common API is declared in `include/decoder/esp_audio_dec.h`.  The ESP32-S3 prebuilt archive is already present in the managed component.

This is a materially better fit than importing the Arduino `Audio` runtime: the decoder accepts caller-owned encoded buffers and returns caller-owned PCM buffers, so a future StackChan backend can pull bytes from `AudioStream` and push PCM to the existing `AudioSink` without creating I2S or touching volume.  It still needs a small adapter for buffering, frame boundaries, metadata, and mapping `esp_audio_err_t` to `AudioDecodeStatus`; the existing component's API is frame-oriented and does not consume `AudioStream` directly.

The component's headers identify two license variants: the common decoder header uses Espressif's Modified MIT terms, while the MP3 implementation header uses Espressif MIT terms.  The project remains limited to Espressif hardware, which includes CoreS3.  Preserve these notices if any source is copied; prefer linking the managed component without copying implementation code.

## Verification status

The upstream application baseline is `pullead/esp32-hifi` commit `1b9185e` (latest `main` at the time of the migration; functional player parent `a7f57b4`).  Network access was unavailable while attempting to resolve the dependency repository's current immutable commit, so no new third-party source was vendored and no unverified SHA was added.  The existing reference SHA in `firmware/dependencies.lock` remains reference-only.

The next implementation step is now to add a decoder-only bridge over the already-fixed `esp_audio_codec` component and a real MP3 fixture test.  The Arduino `ESP32-audioI2S` gate can remain as an optional compatibility path; it is no longer the only route to real MP3 decoding.  Until the bridge and fixture are compiled and exercised, firmware builds and unit tests validate the boundary and error semantics, not compressed-audio playback.
