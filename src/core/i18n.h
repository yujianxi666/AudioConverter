// i18n.h - bilingual (Chinese / English) UI strings.
#pragma once

#include <cstddef>
#include <string>

namespace ac {

enum class Language { Chinese, English };

enum class Key {
    Title,
    SourceFolder,
    SourcePlaceholder,
    Browse,
    FileName,
    Format,
    Size,
    OutputFolder,
    OutputPlaceholder,
    OutputFormat,
    Start,
    Converting,
    Ready,
    Preparing,
    NoFiles,
    NoOutput,
    Info,
    DoneTitle,
    FailedPrefix,
    Cancelled,
    ThemeTip,
    LanguageSwitch,
    TooltipLanguage,
    ErrorDecode,
    ErrorWrite,
    ErrorUnsupported,
    ErrorNcm,
    ErrorTooShort,
    ErrorUnknown
};

const wchar_t* tr(Language language, Key key);

std::wstring formatFilesCount(Language language, size_t count);
std::wstring formatProgress(Language language, size_t completed, size_t total);
std::wstring formatDone(Language language, size_t success, size_t failed,
                        const std::wstring& detail);
std::wstring formatScanError(Language language, const std::wstring& message);

// Localised text for a classified error.
std::wstring formatError(Language language, int errorKind, const std::wstring& detail);

} // namespace ac
