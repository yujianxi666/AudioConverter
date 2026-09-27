// i18n.cpp - bilingual (Chinese / English) UI strings.
//
// The table mirrors the original application's T dictionary, including the
// literal strings used for the ♫ title and the language toggle button.
#include "i18n.h"

#include <cstdio>

#include "util.h"

namespace ac {
namespace {

struct Entry {
    const wchar_t* chinese;
    const wchar_t* english;
};

// Indexed by Key. Keep in sync with the enum in i18n.h.
const Entry kEntries[] = {
    /* Title              */ { L"\u266B 音频转换工具",                 L"\u266B Audio Converter" },
    /* SourceFolder       */ { L"源文件夹",                            L"Source folder" },
    /* SourcePlaceholder  */ { L"选择包含音乐文件的文件夹…",           L"Select folder containing music files\u2026" },
    /* Browse             */ { L"浏览…",                              L"Browse\u2026" },
    /* FileName           */ { L"文件名",                              L"File name" },
    /* Format             */ { L"格式",                                L"Format" },
    /* Size               */ { L"大小",                                L"Size" },
    /* OutputFolder       */ { L"输出文件夹",                          L"Output folder" },
    /* OutputPlaceholder  */ { L"选择目标文件夹…",                     L"Select destination folder\u2026" },
    /* OutputFormat       */ { L"输出格式",                            L"Output format" },
    /* Start              */ { L"开始转换",                            L"Start Conversion" },
    /* Converting         */ { L"转换中…",                            L"Converting\u2026" },
    /* Ready              */ { L"就绪",                                L"Ready" },
    /* Preparing          */ { L"准备中…",                            L"Preparing\u2026" },
    /* NoFiles            */ { L"没有可转换的文件",                    L"No files to convert." },
    /* NoOutput           */ { L"请先选择输出文件夹",                  L"Select an output folder first." },
    /* Info               */ { L"提示",                                L"Info" },
    /* DoneTitle          */ { L"转换完成",                            L"Complete" },
    /* FailedPrefix       */ { L"失败: ",                              L"Failed: " },
    /* Cancelled          */ { L"已取消",                              L"Cancelled" },
    /* ThemeTip           */ { L"切换深色/浅色模式",                   L"Toggle dark / light mode" },
    /* LanguageSwitch     */ { L"EN",                                  L"\u4E2D" },
    /* TooltipLanguage    */ { L"切换中英文界面",                      L"Switch Chinese / English" },
    /* ErrorDecode        */ { L"音频解码失败",                        L"Audio decoding failed" },
    /* ErrorWrite         */ { L"写入文件失败",                        L"Failed to write file" },
    /* ErrorUnsupported   */ { L"不支持的格式",                        L"Unsupported format" },
    /* ErrorNcm           */ { L"NCM解密失败",                         L"NCM decryption failed" },
    /* ErrorTooShort      */ { L"输出文件过短",                        L"Output file is too short" },
    /* ErrorUnknown       */ { L"未知错误",                            L"Unknown error" },
};

constexpr size_t kEntryCount = sizeof(kEntries) / sizeof(kEntries[0]);

} // namespace

const wchar_t* tr(Language language, Key key) {
    const size_t index = static_cast<size_t>(key);
    if (index >= kEntryCount) return L"";
    return (language == Language::Chinese) ? kEntries[index].chinese
                                           : kEntries[index].english;
}

std::wstring formatFilesCount(Language language, size_t count) {
    const std::wstring number = std::to_wstring(count);
    return (language == Language::Chinese) ? number + L" 个文件" : number + L" files";
}

std::wstring formatProgress(Language language, size_t completed, size_t total) {
    const std::wstring done = std::to_wstring(completed);
    const std::wstring all = std::to_wstring(total);
    return (language == Language::Chinese)
               ? L"正在转换 (" + done + L"/" + all + L")"
               : L"Converting (" + done + L"/" + all + L")";
}

std::wstring formatDone(Language language, size_t success, size_t failed,
                        const std::wstring& detail) {
    const std::wstring ok = std::to_wstring(success);
    const std::wstring bad = std::to_wstring(failed);
    std::wstring result = (language == Language::Chinese)
                              ? L"成功: " + ok + L"  失败: " + bad
                              : L"Success: " + ok + L"  Failed: " + bad;
    if (!detail.empty()) result += L"\n\n" + detail;
    return result;
}

std::wstring formatScanError(Language language, const std::wstring& message) {
    return (language == Language::Chinese) ? L"扫描失败: " + message
                                           : L"Scan failed: " + message;
}

std::wstring formatError(Language language, int errorKind, const std::wstring& detail) {
    switch (static_cast<ErrorKind>(errorKind)) {
        case ErrorKind::Decode:      return tr(language, Key::ErrorDecode);
        case ErrorKind::Write:       return tr(language, Key::ErrorWrite);
        case ErrorKind::Unsupported: return tr(language, Key::ErrorUnsupported);
        case ErrorKind::NcmDecrypt:  return tr(language, Key::ErrorNcm);
        case ErrorKind::Other:
        default:
            break;
    }
    if (detail.empty()) return tr(language, Key::ErrorUnknown);
    return shorten(detail, 80);
}

} // namespace ac
