// converter.h - the batch conversion engine.
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "util.h"

namespace ac {

struct ConversionItem {
    std::wstring sourcePath;
    std::wstring relativeName;   // path relative to the scanned folder, '/' separated
    std::wstring formatLabel;    // "MP3", "FLAC", "M4A", "NCM"
    uint64_t sizeBytes = 0;
};

struct FailureInfo {
    std::wstring name;
    ErrorKind kind = ErrorKind::Other;
    std::wstring detail;
};

struct ConversionOptions {
    std::vector<ConversionItem> items;
    std::wstring outputDirectory;
    std::string outputFormat;    // "flac" or "wav"
    int workerCount = 0;         // 0 selects min(hardware threads, 8)
};

struct ConversionSummary {
    size_t total = 0;
    size_t success = 0;
    std::vector<FailureInfo> failures;
    bool cancelled = false;
};

// Called from worker threads after each finished item.
using ProgressCallback = std::function<void(size_t completed, size_t total)>;

// Converts every item. `cancel` may be null. The callback must be thread-safe.
ConversionSummary runConversion(const ConversionOptions& options,
                                const std::atomic<bool>* cancel,
                                const ProgressCallback& onProgress);

// Converts one item; used by the GUI list and by the command line mode.
// On failure `failure` describes why.
bool convertItem(const ConversionItem& item,
                 const std::wstring& outputDirectory,
                 const std::string& outputFormat,
                 FailureInfo& failure);

// Builds the destination path for an item (keeps the relative sub-directories).
std::wstring buildOutputPath(const ConversionItem& item,
                             const std::wstring& outputDirectory,
                             const std::string& outputFormat);

// The input extensions the tool accepts, mirroring the original application.
const std::vector<std::wstring>& supportedExtensions();

// Upper-case format label for an extension, e.g. ".flac" -> "FLAC".
std::wstring formatLabelForExtension(const std::wstring& extension);

} // namespace ac
