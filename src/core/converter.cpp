// converter.cpp - the batch conversion engine.
#include "converter.h"

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <windows.h>
#include <ole2.h>

#include "audio.h"
#include "i18n.h"
#include "ncm.h"

namespace ac {
namespace {

// Media Foundation needs COM on the decoding thread.
void prepareWorkerThread() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    (void)hr; // S_FALSE and RPC_E_CHANGED_MODE are both acceptable
}

void fail(FailureInfo& failure, ErrorKind kind, const std::wstring& detail) {
    failure.kind = kind;
    failure.detail = detail;
}

// Fills in metadata the decrypted payload did not carry itself. Tags that are
// already present win, so the payload's own tagging is never overwritten.
void applyNcmFallback(AudioTags& tags, const NcmTrackInfo& info) {
    auto addIfMissing = [&tags](const char* key, const std::string& value) {
        if (value.empty()) return;
        for (const auto& field : tags.fields) {
            if (field.first == key) return;
        }
        tags.set(key, value);
    };

    addIfMissing("TITLE", info.musicName);
    addIfMissing("ARTIST", info.artist);
    addIfMissing("ALBUM", info.album);

    if (tags.pictures.empty() && !info.cover.empty()) {
        Picture picture;
        picture.type = 3;
        picture.mime = info.coverMime.empty() ? std::string("image/jpeg") : info.coverMime;
        picture.data = info.cover;
        describeImage(picture);
        tags.pictures.push_back(std::move(picture));
    }
}

} // namespace

const std::vector<std::wstring>& supportedExtensions() {
    static const std::vector<std::wstring> extensions = { L".mp3", L".flac", L".m4a", L".ncm" };
    return extensions;
}

std::wstring formatLabelForExtension(const std::wstring& extension) {
    if (extension.size() < 2) return std::wstring();
    std::wstring label = extension.substr(1);
    for (wchar_t& c : label) c = static_cast<wchar_t>(towupper(c));
    return label;
}

std::wstring buildOutputPath(const ConversionItem& item,
                             const std::wstring& outputDirectory,
                             const std::string& outputFormat) {
    const std::wstring extension = (outputFormat == "wav") ? L".wav" : L".flac";
    const std::wstring relative = pathReplaceExtension(item.relativeName, extension);
    return pathJoin(outputDirectory, relative);
}

bool convertItem(const ConversionItem& item,
                 const std::wstring& outputDirectory,
                 const std::string& outputFormat,
                 FailureInfo& failure) {
    failure.name = item.relativeName;
    failure.kind = ErrorKind::Other;
    failure.detail.clear();

    const std::wstring inputLabel = formatLabelForExtension(pathExtension(item.sourcePath));
    const std::wstring outputPath = buildOutputPath(item, outputDirectory, outputFormat);

    if (!ensureParentDirectory(outputPath)) {
        fail(failure, ErrorKind::Write, L"无法创建输出目录");
        return false;
    }

    // ---- lossless pass-through: FLAC -> FLAC keeps the original bytes --------
    if (inputLabel == L"FLAC" && outputFormat == "flac") {
        if (!CopyFileW(item.sourcePath.c_str(), outputPath.c_str(), FALSE)) {
            fail(failure, ErrorKind::Write, L"复制文件失败");
            return false;
        }
        return true;
    }

    // ---- NCM: decrypt first, then either pass the payload through or encode --
    if (inputLabel == L"NCM") {
        NcmResult ncm;
        try {
            ncm = decryptNcmFile(item.sourcePath);
        } catch (const std::exception& error) {
            const std::wstring message = toWide(error.what());
            fail(failure, classifyError(message), message);
            return false;
        } catch (...) {
            fail(failure, ErrorKind::NcmDecrypt, L"NCM 解密失败");
            return false;
        }

        std::string payloadFormat = detectAudioFormat(ncm.audio.data(), ncm.audio.size());
        if (payloadFormat == "unknown" || payloadFormat.empty()) payloadFormat = ncm.info.format;
        if (payloadFormat.empty()) payloadFormat = "mp3";

        // A payload that already matches the requested format is written verbatim,
        // so its own tags and cover art survive untouched.
        if (payloadFormat == outputFormat) {
            std::wstring error;
            if (!writeAllBytes(outputPath, ncm.audio.data(), ncm.audio.size(), &error)) {
                fail(failure, ErrorKind::Write, error);
                return false;
            }
            if (fileSize(outputPath) < 44) {
                removeFile(outputPath);
                fail(failure, ErrorKind::Other, L"输出文件过短");
                return false;
            }
            return true;
        }

        AudioTags tags = readTagsFromPayload(ncm.audio, payloadFormat);
        applyNcmFallback(tags, ncm.info);

        std::wstring error;
        auto source = openAudioMemory(std::move(ncm.audio), payloadFormat, &error);
        if (!source) {
            fail(failure, classifyError(error), error);
            return false;
        }
        if (!encodeAudio(*source, outputFormat, outputPath, tags, &error)) {
            fail(failure, classifyError(error), error);
            return false;
        }
        return true;
    }

    // ---- MP3 / M4A (and any FLAC -> WAV): decode and re-encode ---------------
    AudioTags tags = (outputFormat == "flac") ? readTagsFromFile(item.sourcePath) : AudioTags();

    std::wstring error;
    auto source = openAudioFile(item.sourcePath, &error);
    if (!source) {
        fail(failure, classifyError(error), error);
        return false;
    }
    if (!encodeAudio(*source, outputFormat, outputPath, tags, &error)) {
        fail(failure, classifyError(error), error);
        return false;
    }
    if (fileSize(outputPath) < 44) {
        removeFile(outputPath);
        fail(failure, ErrorKind::Other, L"输出文件过短");
        return false;
    }
    return true;
}

ConversionSummary runConversion(const ConversionOptions& options,
                                const std::atomic<bool>* cancel,
                                const ProgressCallback& onProgress) {
    ConversionSummary summary;
    summary.total = options.items.size();
    if (options.items.empty()) return summary;

    unsigned int hardware = std::thread::hardware_concurrency();
    if (hardware == 0) hardware = 4;
    int workers = options.workerCount;
    if (workers <= 0) workers = static_cast<int>(std::min<unsigned int>(hardware, 8));
    workers = std::max(1, std::min<int>(workers, static_cast<int>(options.items.size())));

    std::atomic<size_t> nextIndex{ 0 };
    std::atomic<size_t> completed{ 0 };
    std::atomic<bool> cancelled{ false };
    std::mutex failureMutex;
    std::vector<FailureInfo> failures;

    auto worker = [&]() {
        prepareWorkerThread();
        for (;;) {
            if (cancel && cancel->load()) { cancelled.store(true); break; }
            const size_t index = nextIndex.fetch_add(1);
            if (index >= options.items.size()) break;

            FailureInfo failure;
            if (!convertItem(options.items[index], options.outputDirectory,
                             options.outputFormat, failure)) {
                std::lock_guard<std::mutex> lock(failureMutex);
                failures.push_back(std::move(failure));
            }
            const size_t done = completed.fetch_add(1) + 1;
            if (onProgress) onProgress(done, options.items.size());
        }
    };

    std::vector<std::thread> threads;
    threads.reserve(static_cast<size_t>(workers));
    for (int i = 0; i < workers; ++i) threads.emplace_back(worker);
    for (std::thread& thread : threads) thread.join();

    summary.success = completed.load() - failures.size();
    summary.failures = std::move(failures);
    summary.cancelled = cancelled.load();
    return summary;
}

} // namespace ac
