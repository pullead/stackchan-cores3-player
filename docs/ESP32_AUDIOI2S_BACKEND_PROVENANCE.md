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

## Verification status

The upstream application baseline is `pullead/esp32-hifi` commit `1b9185e` (latest `main` at the time of the migration; functional player parent `a7f57b4`).  Network access was unavailable while attempting to resolve the dependency repository's current immutable commit, so no new third-party source was vendored and no unverified SHA was added.  The existing reference SHA in `firmware/dependencies.lock` remains reference-only.

The next implementation step is to fetch and audit an immutable `ESP32-audioI2S` revision, then add a decoder-only bridge and a real MP3 fixture test.  Until that happens, firmware builds and unit tests validate the boundary and error semantics, not compressed-audio playback.
