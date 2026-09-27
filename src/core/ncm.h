// ncm.h - NetEase Cloud Music (.ncm) container decryptor.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ac {

// Metadata carried inside the NCM container. Used as a fallback when the
// decrypted payload has no usable tags of its own.
struct NcmTrackInfo {
    std::string format;      // "mp3", "flac", "ogg", ...
    std::string musicName;   // UTF-8
    std::string album;       // UTF-8
    std::string artist;      // UTF-8, first artist only
    std::vector<uint8_t> cover;
    std::string coverMime;
};

struct NcmResult {
    std::vector<uint8_t> audio;   // decrypted payload (mp3/flac/... bytes)
    NcmTrackInfo info;
};

// Decrypts an NCM container that is already in memory. Throws std::runtime_error.
NcmResult decryptNcm(const std::vector<uint8_t>& container);

// Convenience wrapper that reads the file first.
NcmResult decryptNcmFile(const std::wstring& path);

// Sniffs the audio format of a payload: "flac", "mp3", "wav", "ogg", "m4a" or
// "unknown".
std::string detectAudioFormat(const uint8_t* data, size_t size);

// Low-level building blocks, exposed so the self-test can round-trip them.
std::vector<uint8_t> base64Encode(const uint8_t* data, size_t size);

} // namespace ac
