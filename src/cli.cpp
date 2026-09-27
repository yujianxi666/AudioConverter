// cli.cpp - headless command line mode and self-tests.
//
// The original project was GUI only. A command line mode makes the converter
// scriptable and, just as importantly, makes the decoding/encoding paths
// testable without a window.
#include "cli.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "core/audio.h"
#include "core/converter.h"
#include "core/crypto.h"
#include "core/flac_encoder.h"
#include "core/i18n.h"
#include "core/ncm.h"
#include "core/util.h"

namespace ac {
namespace {

// A GUI subsystem binary has no console of its own. Keep any inherited standard
// handle so redirected output (pipes, files) keeps working, and only borrow the
// parent console when there is nothing usable.
void attachConsole() {
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output && output != INVALID_HANDLE_VALUE &&
        GetFileType(output) != FILE_TYPE_UNKNOWN) {
        return;
    }
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
        FILE* stream = nullptr;
        freopen_s(&stream, "CONOUT$", "w", stdout);
        freopen_s(&stream, "CONOUT$", "w", stderr);
        freopen_s(&stream, "CONIN$", "r", stdin);
    }
}

void printHelp() {
    wprintf(L"AudioConverter - batch audio converter (MP3 / FLAC / M4A / NCM -> FLAC / WAV)\n");
    wprintf(L"\n");
    wprintf(L"Usage:\n");
    wprintf(L"  AudioConverter.exe                          launch the graphical interface\n");
    wprintf(L"  AudioConverter.exe --cli -i <dir> -o <dir> [-f flac|wav] [-j <n>] [--lang zh|en]\n");
    wprintf(L"  AudioConverter.exe --selftest               run the built-in encoder tests\n");
    wprintf(L"  AudioConverter.exe --version\n");
    wprintf(L"\n");
    wprintf(L"Options:\n");
    wprintf(L"  -i, --input <path>    folder (scanned recursively) or a single audio file\n");
    wprintf(L"  -o, --output <dir>    destination folder; sub-folders are recreated\n");
    wprintf(L"  -f, --format <fmt>    output format: flac (default) or wav\n");
    wprintf(L"  -j, --jobs <n>        worker threads (default: min(cores, 8))\n");
    wprintf(L"      --lang <lang>     message language: zh (default) or en\n");
    wprintf(L"  -q, --quiet           only print the summary\n");
    wprintf(L"\n");
    wprintf(L"Exit codes: 0 success, 1 one or more files failed, 2 bad arguments.\n");
}

void printVersion() {
    wprintf(L"AudioConverter 2.0 (C++/Win32)\n");
    wprintf(L"Decoders: miniaudio %hs (WAV/FLAC/MP3), Windows Media Foundation (M4A/AAC)\n",
            "0.11.25");
    wprintf(L"Encoders: built-in FLAC, PCM 16-bit WAV\n");
}

// ---------------------------------------------------------------------------
// Self-tests
// ---------------------------------------------------------------------------
std::wstring tempFilePath(const wchar_t* name) {
    wchar_t directory[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, directory);
    return pathJoin(directory, name);
}

bool comparePcm(const std::wstring& label, const std::wstring& path,
                const std::vector<int16_t>& expected, uint32_t channels) {
    std::wstring error;
    auto source = openAudioFile(path, &error);
    if (!source) {
        wprintf(L"  [FAIL] %ls: cannot reopen (%ls)\n", label.c_str(), error.c_str());
        return false;
    }
    if (source->channels() != channels || source->totalFrames() * channels != expected.size()) {
        wprintf(L"  [FAIL] %ls: expected %zu frames x %u channels, got %llu x %u\n",
                label.c_str(), expected.size() / channels, channels,
                static_cast<unsigned long long>(source->totalFrames()), source->channels());
        return false;
    }
    std::vector<int16_t> decoded(expected.size());
    uint32_t total = 0;
    while (total < decoded.size() / channels) {
        const uint32_t read = source->read(decoded.data() + static_cast<size_t>(total) * channels,
                                           static_cast<uint32_t>(decoded.size() / channels) - total);
        if (read == 0) break;
        total += read;
    }
    if (static_cast<size_t>(total) * channels != decoded.size()) {
        wprintf(L"  [FAIL] %ls: short read (%u frames)\n", label.c_str(), total);
        return false;
    }
    size_t mismatches = 0;
    int worst = 0;
    for (size_t i = 0; i < decoded.size(); ++i) {
        const int difference = std::abs(static_cast<int>(decoded[i]) - static_cast<int>(expected[i]));
        if (difference != 0) ++mismatches;
        worst = std::max(worst, difference);
    }
    if (mismatches != 0) {
        wprintf(L"  [FAIL] %ls: %zu/%zu samples differ (max delta %d)\n", label.c_str(),
                mismatches, decoded.size(), worst);
        return false;
    }
    wprintf(L"  [ ok ] %ls: %zu samples identical\n", label.c_str(), decoded.size());
    return true;
}

int runSelfTest() {
    int failures = 0;
    auto report = [&failures](const wchar_t* name, bool ok) {
        wprintf(L"  [%ls] %ls\n", ok ? L" ok " : L"FAIL", name);
        if (!ok) ++failures;
    };

    wprintf(L"Self-test\n");
    report(L"AES-128 FIPS-197 known answer", aesSelfTest());
    report(L"MD5 RFC-1321 vectors", md5SelfTest());

    // A signal that exercises every subframe type: silence (constant), a tone
    // (fixed predictor) and noise (verbatim/escape partitions).
    const uint32_t frameRate = 44100;
    const uint32_t channels = 2;
    const uint32_t frames = frameRate * 3;
    std::vector<int16_t> pcm(static_cast<size_t>(frames) * channels);
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / frameRate;
        seed = seed * 1664525u + 1013904223u;
        int16_t left = 0;
        int16_t right = 0;
        if (i < frameRate) {
            left = static_cast<int16_t>(std::sin(2.0 * 3.14159265358979 * 440.0 * t) * 20000.0);
            right = static_cast<int16_t>(std::sin(2.0 * 3.14159265358979 * 554.0 * t) * 12000.0);
        } else if (i < frameRate * 2) {
            left = static_cast<int16_t>((seed >> 16) - 32768);   // broadband noise
            right = static_cast<int16_t>((seed & 0xFFFF) - 32768);
        }
        pcm[static_cast<size_t>(i) * 2] = left;
        pcm[static_cast<size_t>(i) * 2 + 1] = right;
    }

    const std::wstring flacPath = tempFilePath(L"ac_selftest.flac");
    const std::wstring wavPath = tempFilePath(L"ac_selftest.wav");
    removeFile(flacPath);
    removeFile(wavPath);

    {
        AudioTags tags;
        tags.set("TITLE", "Self test");
        tags.set("ARTIST", "AudioConverter");
        auto writer = createFlacWriter();
        std::wstring error;
        bool ok = writer->open(flacPath, channels, frameRate, tags, &error);
        if (ok) ok = writer->write(pcm.data(), frames, &error);
        if (ok) ok = writer->close(&error);
        if (!ok) {
            wprintf(L"  [FAIL] FLAC encode: %ls\n", error.c_str());
            ++failures;
        } else {
            report(L"FLAC encode -> decode round trip",
                   comparePcm(L"FLAC", flacPath, pcm, channels));
        }
    }

    {
        auto writer = createAudioWriter("wav");
        std::wstring error;
        bool ok = writer->open(wavPath, channels, frameRate, AudioTags(), &error);
        if (ok) ok = writer->write(pcm.data(), frames, &error);
        if (ok) ok = writer->close(&error);
        if (!ok) {
            wprintf(L"  [FAIL] WAV write: %ls\n", error.c_str());
            ++failures;
        } else {
            report(L"WAV write -> decode round trip",
                   comparePcm(L"WAV", wavPath, pcm, channels));
        }
    }

    // NCM error handling must be a clean failure, never a crash.
    {
        std::vector<uint8_t> garbage(4096, 0x5A);
        bool threw = false;
        try {
            decryptNcm(garbage);
        } catch (const std::exception&) {
            threw = true;
        }
        report(L"NCM rejects a container with a bad magic", threw);
    }

    removeFile(flacPath);
    removeFile(wavPath);

    if (failures == 0) {
        wprintf(L"All self-tests passed.\n");
    } else {
        wprintf(L"%d self-test(s) failed.\n", failures);
    }
    return failures == 0 ? 0 : 1;
}

// ---------------------------------------------------------------------------
// Batch conversion
// ---------------------------------------------------------------------------
struct CliOptions {
    std::wstring input;
    std::wstring output;
    std::string format = "flac";
    int jobs = 0;
    Language language = Language::Chinese;
    bool quiet = false;
};

bool parseOptions(const std::vector<std::wstring>& arguments, CliOptions& options) {
    for (size_t i = 0; i < arguments.size(); ++i) {
        const std::wstring& argument = arguments[i];
        auto value = [&](std::wstring& target) {
            if (i + 1 >= arguments.size()) return false;
            target = arguments[++i];
            return true;
        };
        if (argument == L"-i" || argument == L"--input") {
            if (!value(options.input)) return false;
        } else if (argument == L"-o" || argument == L"--output") {
            if (!value(options.output)) return false;
        } else if (argument == L"-f" || argument == L"--format") {
            std::wstring format;
            if (!value(format)) return false;
            std::transform(format.begin(), format.end(), format.begin(),
                           [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
            if (format != L"flac" && format != L"wav") return false;
            options.format = (format == L"wav") ? "wav" : "flac";
        } else if (argument == L"-j" || argument == L"--jobs") {
            std::wstring jobs;
            if (!value(jobs)) return false;
            options.jobs = _wtoi(jobs.c_str());
            if (options.jobs < 1) options.jobs = 1;
        } else if (argument == L"--lang") {
            std::wstring language;
            if (!value(language)) return false;
            options.language = (language == L"en" || language == L"EN") ? Language::English
                                                                        : Language::Chinese;
        } else if (argument == L"-q" || argument == L"--quiet") {
            options.quiet = true;
        } else {
            return false;
        }
    }
    return !options.input.empty() && !options.output.empty();
}

int runConversion(const CliOptions& options) {
    ConversionOptions conversion;
    conversion.outputDirectory = options.output;
    conversion.outputFormat = options.format;
    conversion.workerCount = options.jobs;

    if (fileExists(options.input)) {
        ConversionItem item;
        item.sourcePath = options.input;
        item.relativeName = pathFileName(options.input);
        item.formatLabel = formatLabelForExtension(pathExtension(options.input));
        item.sizeBytes = fileSize(options.input);
        conversion.items.push_back(std::move(item));
    } else {
        std::vector<std::wstring> files;
        collectFiles(options.input, supportedExtensions(), files);
        std::sort(files.begin(), files.end(),
                  [](const std::wstring& a, const std::wstring& b) {
                      return compareNoCase(a, b) < 0;
                  });
        for (const std::wstring& path : files) {
            ConversionItem item;
            item.sourcePath = path;
            item.relativeName = normalizeSlashes(pathRelative(path, options.input));
            item.formatLabel = formatLabelForExtension(pathExtension(path));
            item.sizeBytes = fileSize(path);
            conversion.items.push_back(std::move(item));
        }
    }

    if (conversion.items.empty()) {
        wprintf(L"No supported files found in: %ls\n", options.input.c_str());
        return 2;
    }

    const size_t total = conversion.items.size();
    if (!options.quiet) {
        wprintf(L"Converting %zu file(s) to %hs\n", total, conversion.outputFormat.c_str());
    }

    size_t lastReported = 0;
    ConversionSummary summary = runConversion(
        conversion, nullptr, [&](size_t completed, size_t) {
            if (options.quiet) return;
            if (completed == total || completed - lastReported >= 10) {
                lastReported = completed;
                wprintf(L"\r  %zu / %zu", completed, total);
                fflush(stdout);
            }
        });
    if (!options.quiet && total > 0) wprintf(L"\r  %zu / %zu\n", total, total);

    for (const FailureInfo& failure : summary.failures) {
        const std::wstring reason =
            formatError(options.language, static_cast<int>(failure.kind), failure.detail);
        const std::wstring suffix = reason.empty() ? std::wstring() : (L"(" + reason + L")");
        wprintf(L"  %ls%ls%ls\n", tr(options.language, Key::FailedPrefix), failure.name.c_str(),
                suffix.c_str());
    }

    wprintf(L"%ls\n",
            formatDone(options.language, summary.success, summary.failures.size(), L"").c_str());
    wprintf(L"Output: %ls\n", options.output.c_str());
    return summary.failures.empty() ? 0 : 1;
}

} // namespace

bool isCommandLineCommand(const wchar_t* argument) {
    if (!argument || !*argument) return false;
    static const wchar_t* commands[] = {
        L"--cli", L"--help", L"-h", L"--version", L"--selftest"
    };
    for (const wchar_t* command : commands) {
        if (wcscmp(argument, command) == 0) return true;
    }
    return false;
}

int runCommandLine(int argumentCount, wchar_t** arguments) {
    attachConsole();
    std::vector<std::wstring> args;
    args.reserve(static_cast<size_t>(std::max(0, argumentCount - 1)));
    for (int i = 1; i < argumentCount; ++i) args.emplace_back(arguments[i]);
    if (args.empty()) {
        printHelp();
        return 2;
    }

    const std::wstring command = args.front();
    if (command == L"--help" || command == L"-h") {
        printHelp();
        return 0;
    }
    if (command == L"--version") {
        printVersion();
        return 0;
    }
    if (command == L"--selftest") return runSelfTest();

    if (command == L"--cli") {
        CliOptions options;
        const std::vector<std::wstring> rest(args.begin() + 1, args.end());
        if (!parseOptions(rest, options)) {
            wprintf(L"Invalid arguments. Use --help for usage.\n");
            return 2;
        }
        return runConversion(options);
    }

    printHelp();
    return 2;
}

} // namespace ac
