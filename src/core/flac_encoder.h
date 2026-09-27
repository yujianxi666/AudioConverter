// flac_encoder.h - streaming FLAC encoder for 16-bit PCM.
#pragma once

#include <memory>

#include "audio.h"

namespace ac {

// Streaming FLAC writer (16-bit input). Registered with createAudioWriter(),
// exposed here so the self-test can drive it directly.
std::unique_ptr<AudioWriter> createFlacWriter();

} // namespace ac
