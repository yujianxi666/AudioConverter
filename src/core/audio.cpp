// audio.cpp - decoder factory and the PCM16 WAV writer.
#include "audio.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "decoders.h"
#include "flac_encoder.h"
#include "ncm.h"

namespace ac {

// Frames decoded per iteration while streaming into a writer.
constexpr uint32_t kEncodeBlockFrames = 8192;

void AudioTags::set(const std::string& key, const std::string& value) {
    if (value.empty()) return;
    for (auto& field : fields) {
        if (field.first == key) { field.second = value; return; }
    }
    fields.emplace_back(key, value);
}

namespace {

// Reads the first bytes of a file so the container format can be sniffed.
std::string sniffFileFormat(const std::wstring& path, const std::wstring& extension) {
    if (extension == L".m4a" || extension == L".mp4" || extension == L".aac") return "m4a";

    FILE* file = _wfopen(path.c_str(), L"rb");
    if (!file) return std::string();
    uint8_t header[16] = {};
    const size_t read = fread(header, 1, sizeof(header), file);
    fclose(file);
    return detectAudioFormat(header, read);
}

} // namespace

std::unique_ptr<AudioSource> openAudioMemory(std::vector<uint8_t> data,
                                             const std::string& format,
                                             std::wstring* error) {
    if (data.empty()) {
        if (error) *error = L"音频数据为空";
        return nullptr;
    }
    const std::string detected =
        format.empty() ? detectAudioFormat(data.data(), data.size()) : format;

    if (detected == "m4a" || detected == "mp4" || detected == "aac") {
        std::wstring mediaFoundationError;
        if (auto source = openMediaFoundationMemory(std::move(data), &mediaFoundationError))
            return source;
        if (error) *error = mediaFoundationError;
        return nullptr;
    }

    std::wstring miniaudioError;
    auto source = openMiniaudioMemory(std::move(data), &miniaudioError);
    if (source) return source;
    if (error) *error = miniaudioError;
    return nullptr;
}

std::unique_ptr<AudioSource> openAudioFile(const std::wstring& path, std::wstring* error) {
    const std::wstring extension = pathExtension(path);
    const std::string format = sniffFileFormat(path, extension);

    if (format == "m4a") {
        std::wstring mediaFoundationError;
        auto source = openMediaFoundationFile(path, &mediaFoundationError);
        if (source) return source;
        // Fall through so the caller sees the more specific error below.
        std::wstring miniaudioError;
        if (auto fallback = openMiniaudioFile(path, &miniaudioError)) return fallback;
        if (error) *error = mediaFoundationError;
        return nullptr;
    }

    std::wstring miniaudioError;
    if (auto source = openMiniaudioFile(path, &miniaudioError)) return source;

    // Last resort: let Media Foundation try, it understands a few extra containers.
    std::wstring mediaFoundationError;
    if (auto fallback = openMediaFoundationFile(path, &mediaFoundationError)) return fallback;
    if (error) *error = miniaudioError;
    return nullptr;
}

// ---------------------------------------------------------------------------
// WAV writer (canonical 44-byte PCM header, sizes patched on close)
// ---------------------------------------------------------------------------
namespace {

class WavWriter final : public AudioWriter {
public:
    ~WavWriter() override {
        if (file_) fclose(file_);
    }

    bool open(const std::wstring& path, uint32_t channels, uint32_t sampleRate,
              const AudioTags& tags, std::wstring* error) override {
        (void)tags; // WAV output carries no tags, matching the original application
        if (channels == 0 || sampleRate == 0) {
            if (error) *error = L"音频格式无效";
            return false;
        }
        if (!ensureParentDirectory(path)) {
            if (error) *error = L"无法创建输出目录";
            return false;
        }
        file_ = _wfopen(path.c_str(), L"wb");
        if (!file_) {
            if (error) *error = L"写入文件失败";
            return false;
        }
        channels_ = channels;
        sampleRate_ = sampleRate;
        dataBytes_ = 0;

        std::vector<uint8_t> header;
        header.reserve(44);
        header.insert(header.end(), { 'R', 'I', 'F', 'F' });
        appendU32(header, 36);                       // patched in close()
        header.insert(header.end(), { 'W', 'A', 'V', 'E' });
        header.insert(header.end(), { 'f', 'm', 't', ' ' });
        appendU32(header, 16);
        appendU16(header, 1);                        // PCM
        appendU16(header, static_cast<uint16_t>(channels));
        appendU32(header, sampleRate);
        appendU32(header, sampleRate * channels * 2);
        appendU16(header, static_cast<uint16_t>(channels * 2));
        appendU16(header, 16);
        header.insert(header.end(), { 'd', 'a', 't', 'a' });
        appendU32(header, 0);                        // patched in close()

        if (fwrite(header.data(), 1, header.size(), file_) != header.size()) {
            if (error) *error = L"写入文件失败";
            return false;
        }
        return true;
    }

    bool write(const int16_t* samples, uint32_t frames, std::wstring* error) override {
        if (frames == 0) return true;
        const size_t bytes = static_cast<size_t>(frames) * channels_ * sizeof(int16_t);
        if (fwrite(samples, 1, bytes, file_) != bytes) {
            if (error) *error = L"写入文件失败";
            return false;
        }
        dataBytes_ += bytes;
        return true;
    }

    bool close(std::wstring* error) override {
        if (!file_) return true;
        bool ok = true;
        const uint32_t dataSize = static_cast<uint32_t>(std::min<uint64_t>(dataBytes_, 0xFFFFFFFFull));
        const uint32_t riffSize = static_cast<uint32_t>(std::min<uint64_t>(dataBytes_ + 36, 0xFFFFFFFFull));

        if (_fseeki64(file_, 4, SEEK_SET) != 0 || !writeU32AtCurrent(riffSize)) ok = false;
        if (_fseeki64(file_, 40, SEEK_SET) != 0 || !writeU32AtCurrent(dataSize)) ok = false;
        _fseeki64(file_, 0, SEEK_END);

        if (fclose(file_) != 0) ok = false;
        file_ = nullptr;
        if (!ok && error) *error = L"写入文件失败";
        return ok;
    }

private:
    static void appendU32(std::vector<uint8_t>& out, uint32_t value) {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
    }
    static void appendU16(std::vector<uint8_t>& out, uint16_t value) {
        out.push_back(static_cast<uint8_t>(value & 0xFF));
        out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    }
    bool writeU32AtCurrent(uint32_t value) {
        uint8_t bytes[4] = {
            static_cast<uint8_t>(value & 0xFF),
            static_cast<uint8_t>((value >> 8) & 0xFF),
            static_cast<uint8_t>((value >> 16) & 0xFF),
            static_cast<uint8_t>((value >> 24) & 0xFF)
        };
        return fwrite(bytes, 1, 4, file_) == 4;
    }

    FILE* file_ = nullptr;
    uint32_t channels_ = 0;
    uint32_t sampleRate_ = 0;
    uint64_t dataBytes_ = 0;
};

} // namespace

std::unique_ptr<AudioWriter> createAudioWriter(const std::string& format) {
    if (format == "wav") return std::make_unique<WavWriter>();
    return createFlacWriter();
}

// ---------------------------------------------------------------------------
// Decode + encode
// ---------------------------------------------------------------------------
bool encodeAudio(AudioSource& source,
                 const std::string& outputFormat,
                 const std::wstring& outputPath,
                 const AudioTags& tags,
                 std::wstring* error) {
    auto writer = createAudioWriter(outputFormat);
    if (!writer->open(outputPath, source.channels(), source.sampleRate(), tags, error))
        return false;

    std::vector<int16_t> buffer(static_cast<size_t>(kEncodeBlockFrames) * source.channels());
    uint64_t framesWritten = 0;
    for (;;) {
        const uint32_t framesRead = source.read(buffer.data(), kEncodeBlockFrames);
        if (framesRead == 0) break;
        if (!writer->write(buffer.data(), framesRead, error)) {
            removeFile(outputPath);
            return false;
        }
        framesWritten += framesRead;
    }

    if (framesWritten == 0) {
        if (error) *error = L"音频解码失败";
        removeFile(outputPath);
        return false;
    }
    if (!writer->close(error)) {
        removeFile(outputPath);
        return false;
    }
    return true;
}

} // namespace ac
