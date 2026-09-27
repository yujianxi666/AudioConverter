// decode_miniaudio.cpp - PCM decoding through miniaudio (WAV / FLAC / MP3 / Ogg).
//
// This is the same decoding backend the original Python application used
// (miniaudio's Python binding), so MP3/FLAC/WAV output stays bit-identical.
#include <windows.h>

#include <cstring>

#include "audio.h"
#include "util.h"

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include "../third_party/miniaudio.h"

namespace ac {
namespace {

class MiniaudioSource final : public AudioSource {
public:
    ~MiniaudioSource() override {
        if (initialized_) ma_decoder_uninit(&decoder_);
    }

    bool initWithFile(const std::wstring& path, std::wstring* error) {
        ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
        if (ma_decoder_init_file_w(path.c_str(), &config, &decoder_) != MA_SUCCESS) {
            if (error) *error = L"无法解码音频文件";
            return false;
        }
        initialized_ = true;
        return finishInit();
    }

    // Takes ownership of the payload so the decoder can reference it.
    bool initWithMemory(std::vector<uint8_t> data, std::wstring* error) {
        data_ = std::move(data);
        ma_decoder_config config = ma_decoder_config_init(ma_format_s16, 0, 0);
        if (ma_decoder_init_memory(data_.data(), data_.size(), &config, &decoder_) != MA_SUCCESS) {
            if (error) *error = L"无法解码音频数据";
            return false;
        }
        initialized_ = true;
        return finishInit();
    }

    uint32_t read(int16_t* destination, uint32_t frames) override {
        if (!initialized_ || frames == 0) return 0;
        ma_uint64 framesRead = 0;
        if (ma_decoder_read_pcm_frames(&decoder_, destination, frames, &framesRead) != MA_SUCCESS)
            return 0;
        return static_cast<uint32_t>(framesRead);
    }

private:
    bool finishInit() {
        const ma_uint32 channels = decoder_.outputChannels;
        const ma_uint32 sampleRate = decoder_.outputSampleRate;
        if (channels == 0 || sampleRate == 0) return false;
        channels_ = channels;
        sampleRate_ = sampleRate;

        ma_uint64 length = 0;
        if (ma_decoder_get_length_in_pcm_frames(&decoder_, &length) == MA_SUCCESS)
            totalFrames_ = length;
        return true;
    }

    ma_decoder decoder_{};
    bool initialized_ = false;
    std::vector<uint8_t> data_;
};

} // namespace

std::unique_ptr<AudioSource> openMiniaudioFile(const std::wstring& path, std::wstring* error) {
    auto source = std::make_unique<MiniaudioSource>();
    if (!source->initWithFile(path, error)) return nullptr;
    return source;
}

std::unique_ptr<AudioSource> openMiniaudioMemory(std::vector<uint8_t> data, std::wstring* error) {
    auto source = std::make_unique<MiniaudioSource>();
    if (!source->initWithMemory(std::move(data), error)) return nullptr;
    return source;
}

} // namespace ac
