// decode_mediafoundation.cpp - AAC / M4A decoding through Windows Media Foundation.
//
// The original Python application listed .m4a as a supported input but decoded
// through miniaudio, which has no AAC backend, so those files always failed.
// Media Foundation ships with Windows, so this restores the advertised support
// without adding a third-party dependency.
#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <ole2.h>
#include <propvarutil.h>

#include <cstring>
#include <mutex>

#include "audio.h"
#include "util.h"

namespace ac {
namespace {

// ---------------------------------------------------------------------------
// Media Foundation / COM lifetime helpers
// ---------------------------------------------------------------------------
std::once_flag g_mfStartupOnce;
bool g_mfAvailable = false;

bool ensureMediaFoundation() {
    std::call_once(g_mfStartupOnce, [] {
        g_mfAvailable = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_FULL));
    });
    return g_mfAvailable;
}

// Media Foundation requires COM on the calling thread. Worker threads call this
// once and keep the apartment for their whole lifetime.
void ensureThreadCom() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (hr == RPC_E_CHANGED_MODE) return; // already initialised with another model
    // S_FALSE means it was already initialised for this thread; both are fine.
    (void)hr;
}

// Thin RAII wrapper so early returns cannot leak COM interfaces.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** put() { reset(); return &pointer_; }
    T* get() const { return pointer_; }
    T* operator->() const { return pointer_; }
    void reset() {
        if (pointer_) { pointer_->Release(); pointer_ = nullptr; }
    }
    explicit operator bool() const { return pointer_ != nullptr; }

private:
    T* pointer_ = nullptr;
};

// ---------------------------------------------------------------------------
// Decoder
// ---------------------------------------------------------------------------
class MediaFoundationSource final : public AudioSource {
public:
    ~MediaFoundationSource() override {
        reader_.reset();
        if (!tempPath_.empty()) removeFile(tempPath_);
    }

    bool initWithFile(const std::wstring& path, std::wstring* error) {
        if (!ensureMediaFoundation()) {
            if (error) *error = L"系统不支持 Media Foundation 解码";
            return false;
        }
        ensureThreadCom();

        if (FAILED(MFCreateSourceReaderFromURL(path.c_str(), nullptr, reader_.put()))) {
            if (error) *error = L"无法打开 M4A/AAC 文件";
            return false;
        }
        return configure(error);
    }

    bool initWithMemory(std::vector<uint8_t> data, std::wstring* error) {
        // Media Foundation needs a container it can seek in; spill to a temp file.
        wchar_t tempDirectory[MAX_PATH] = {};
        wchar_t tempFile[MAX_PATH] = {};
        if (GetTempPathW(MAX_PATH, tempDirectory) == 0 ||
            GetTempFileNameW(tempDirectory, L"acn", 0, tempFile) == 0) {
            if (error) *error = L"无法创建临时文件";
            return false;
        }
        tempPath_ = std::wstring(tempFile) + L".m4a";
        DeleteFileW(tempFile);
        if (!writeAllBytes(tempPath_, data.data(), data.size(), error)) {
            removeFile(tempPath_);
            tempPath_.clear();
            return false;
        }
        return initWithFile(tempPath_, error);
    }

    uint32_t read(int16_t* destination, uint32_t frames) override {
        if (frames == 0) return 0;

        uint32_t framesWritten = 0;
        while (framesWritten < frames) {
            if (pendingOffset_ < pending_.size()) {
                const size_t availableFrames = (pending_.size() - pendingOffset_) / channels_;
                if (availableFrames == 0) { pending_.clear(); pendingOffset_ = 0; continue; }
                const uint32_t take = static_cast<uint32_t>(
                    (frames - framesWritten) < availableFrames ? (frames - framesWritten)
                                                              : availableFrames);
                memcpy(destination + static_cast<size_t>(framesWritten) * channels_,
                       pending_.data() + pendingOffset_,
                       static_cast<size_t>(take) * channels_ * sizeof(int16_t));
                pendingOffset_ += static_cast<size_t>(take) * channels_;
                framesWritten += take;
                continue;
            }
            pending_.clear();
            pendingOffset_ = 0;
            if (endOfStream_ || !readNextSample()) break;
        }
        return framesWritten;
    }

private:
    bool configure(std::wstring* error) {
        ComPtr<IMFMediaType> requested;
        if (FAILED(MFCreateMediaType(requested.put()))) {
            if (error) *error = L"Media Foundation 初始化失败";
            return false;
        }
        requested->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        requested->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);

        if (FAILED(reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, nullptr,
                                                requested.get()))) {
            if (error) *error = L"系统缺少可用的 AAC 解码器";
            return false;
        }
        if (!readCurrentType(error)) return false;

        // Duration is only used to report progress, so a failure is not fatal.
        PROPVARIANT duration;
        memset(&duration, 0, sizeof(duration));
        if (SUCCEEDED(reader_->GetPresentationAttribute(MF_SOURCE_READER_MEDIASOURCE,
                                                       MF_PD_DURATION, &duration)) &&
            duration.vt == VT_UI8 && sampleRate_ > 0) {
            totalFrames_ = duration.uhVal.QuadPart * sampleRate_ / 10000000ull;
        }
        PropVariantClear(&duration);
        return true;
    }

    bool readCurrentType(std::wstring* error) {
        ComPtr<IMFMediaType> actual;
        if (FAILED(reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM,
                                                actual.put()))) {
            if (error) *error = L"无法读取音频格式";
            return false;
        }

        UINT32 channels = 0;
        UINT32 sampleRate = 0;
        UINT32 blockAlign = 0;
        GUID subtype = {};
        if (FAILED(actual->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels)) ||
            FAILED(actual->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate)) ||
            channels == 0 || sampleRate == 0) {
            if (error) *error = L"音频格式无效";
            return false;
        }
        actual->GetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, &blockAlign);
        actual->GetGUID(MF_MT_SUBTYPE, &subtype);

        channels_ = channels;
        sampleRate_ = sampleRate;
        blockAlign_ = blockAlign ? blockAlign : channels * 2;
        isFloat_ = (subtype == MFAudioFormat_Float);
        return true;
    }

    bool readNextSample() {
        DWORD flags = 0;
        ComPtr<IMFSample> sample;
        if (FAILED(reader_->ReadSample(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, nullptr,
                                       &flags, nullptr, sample.put()))) {
            endOfStream_ = true;
            return false;
        }
        if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
            std::wstring ignored;
            readCurrentType(&ignored);
        }
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
            endOfStream_ = true;
            return false;
        }
        if (!sample) return false;

        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) return false;

        BYTE* raw = nullptr;
        DWORD currentLength = 0;
        if (FAILED(buffer->Lock(&raw, nullptr, &currentLength))) return false;

        const size_t sampleCount = currentLength / sizeof(int16_t);
        pending_.resize(sampleCount);
        if (isFloat_) {
            // Defensive: convert 32-bit float in case the converter MFT was skipped.
            const float* source = reinterpret_cast<const float*>(raw);
            for (size_t i = 0; i < sampleCount; ++i) {
                float value = source[i];
                if (value > 1.0f) value = 1.0f;
                if (value < -1.0f) value = -1.0f;
                pending_[i] = static_cast<int16_t>(value * 32767.0f);
            }
        } else {
            memcpy(pending_.data(), raw, sampleCount * sizeof(int16_t));
        }
        pendingOffset_ = 0;

        buffer->Unlock();
        return true;
    }

    ComPtr<IMFSourceReader> reader_;
    std::vector<int16_t> pending_;
    size_t pendingOffset_ = 0;
    uint32_t blockAlign_ = 0;
    bool isFloat_ = false;
    bool endOfStream_ = false;
    std::wstring tempPath_;
};

} // namespace

std::unique_ptr<AudioSource> openMediaFoundationFile(const std::wstring& path, std::wstring* error) {
    auto source = std::make_unique<MediaFoundationSource>();
    if (!source->initWithFile(path, error)) return nullptr;
    return source;
}

std::unique_ptr<AudioSource> openMediaFoundationMemory(std::vector<uint8_t> data,
                                                       std::wstring* error) {
    auto source = std::make_unique<MediaFoundationSource>();
    if (!source->initWithMemory(std::move(data), error)) return nullptr;
    return source;
}

} // namespace ac
