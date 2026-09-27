// decoders.h - internal decoder factory declarations.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "audio.h"

namespace ac {

// miniaudio backend: WAV, FLAC, MP3, Ogg Vorbis.
std::unique_ptr<AudioSource> openMiniaudioFile(const std::wstring& path, std::wstring* error);
std::unique_ptr<AudioSource> openMiniaudioMemory(std::vector<uint8_t> data, std::wstring* error);

// Media Foundation backend: M4A / AAC and anything else Windows can decode.
std::unique_ptr<AudioSource> openMediaFoundationFile(const std::wstring& path, std::wstring* error);
std::unique_ptr<AudioSource> openMediaFoundationMemory(std::vector<uint8_t> data,
                                                       std::wstring* error);

} // namespace ac
