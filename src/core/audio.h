// audio.h - audio decoding, encoding and tag handling.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "util.h"

namespace ac {

// ---------------------------------------------------------------------------
// PCM source
// ---------------------------------------------------------------------------
// An interleaved signed 16-bit PCM stream. Both the miniaudio file/memory
// decoders and the Media Foundation decoder implement this, so the conversion
// engine never needs to know which backend decoded the data.
class AudioSource {
public:
    virtual ~AudioSource() = default;

    uint32_t channels() const { return channels_; }
    uint32_t sampleRate() const { return sampleRate_; }
    uint64_t totalFrames() const { return totalFrames_; } // 0 when unknown

    // Fills `destination` with up to `frames` interleaved frames.
    // Returns the number of frames written; 0 marks the end of the stream.
    virtual uint32_t read(int16_t* destination, uint32_t frames) = 0;

protected:
    uint32_t channels_ = 0;
    uint32_t sampleRate_ = 0;
    uint64_t totalFrames_ = 0;
};

// Opens a decoder over an in-memory payload. `format` is a hint such as "mp3",
// "flac" or "m4a" (see detectAudioFormat); "m4a" is routed to Media Foundation.
// Returns nullptr and fills `error` when no decoder can handle the payload.
std::unique_ptr<AudioSource> openAudioMemory(std::vector<uint8_t> data,
                                             const std::string& format,
                                             std::wstring* error);

// Opens a decoder over a file on disk.
std::unique_ptr<AudioSource> openAudioFile(const std::wstring& path, std::wstring* error);

// ---------------------------------------------------------------------------
// Tags
// ---------------------------------------------------------------------------
struct Picture {
    std::vector<uint8_t> data;
    std::string mime;
    std::string description;
    uint32_t type = 3;      // 3 == front cover
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t depth = 0;
    uint32_t colors = 0;
};

struct AudioTags {
    // Vorbis-comment style fields, all UTF-8: ("TITLE", "...").
    std::vector<std::pair<std::string, std::string>> fields;
    std::vector<Picture> pictures;

    void set(const std::string& key, const std::string& value);
    bool empty() const { return fields.empty() && pictures.empty(); }
};

// Reads tags from a file, sniffing the container format from its header.
AudioTags readTagsFromFile(const std::wstring& path);

// Reads tags from an in-memory payload; `format` comes from detectAudioFormat.
AudioTags readTagsFromPayload(const std::vector<uint8_t>& data, const std::string& format);

// Fills in a picture's width/height/depth/colors from its PNG or JPEG header.
void describeImage(Picture& picture);

// ---------------------------------------------------------------------------
// Writers
// ---------------------------------------------------------------------------
class AudioWriter {
public:
    virtual ~AudioWriter() = default;
    virtual bool open(const std::wstring& path, uint32_t channels, uint32_t sampleRate,
                      const AudioTags& tags, std::wstring* error) = 0;
    virtual bool write(const int16_t* samples, uint32_t frames, std::wstring* error) = 0;
    // Finishes the stream and patches the header/sizes. Returns the byte size.
    virtual bool close(std::wstring* error) = 0;
};

// `format` is "flac" or "wav".
std::unique_ptr<AudioWriter> createAudioWriter(const std::string& format);

// Decodes `source` and encodes it to `outputPath`. `tags` are written into FLAC
// output only, which matches the original application's behaviour.
bool encodeAudio(AudioSource& source,
                 const std::string& outputFormat,
                 const std::wstring& outputPath,
                 const AudioTags& tags,
                 std::wstring* error);

} // namespace ac
