// Host-only stub for the ESP-IDF MP3 decoder backend.
//
// The real backend links against espressif/esp_audio_codec, which only exists
// in the ESP-IDF build.  Host tests therefore cannot exercise it; this stub
// exists so that code depending on the factory can still be linked and tested
// on the host.  A null return here means "no backend in the host build" and
// says nothing about device behaviour.
#include "media/decoder/esp_mp3_decoder_backend.h"

namespace media {

std::unique_ptr<HifiDecoderBackend> create_esp_mp3_decoder_backend() noexcept { return nullptr; }

}  // namespace media
