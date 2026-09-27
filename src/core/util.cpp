// util.cpp - string, path and file helpers for AudioConverter.
#include "util.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwctype>

namespace ac {

// ---------------------------------------------------------------- conversion
std::wstring toWide(const std::string& utf8) {
    if (utf8.empty()) return std::wstring();
    const int needed = MultiByteToWideChar(CP_UTF8, 0, utf8.data(),
                                           static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) {
        // Not valid UTF-8; fall back to the active ANSI code page.
        const int ansiNeeded = MultiByteToWideChar(CP_ACP, 0, utf8.data(),
                                                   static_cast<int>(utf8.size()), nullptr, 0);
        if (ansiNeeded <= 0) return std::wstring();
        std::wstring ansi(static_cast<size_t>(ansiNeeded), L'\0');
        MultiByteToWideChar(CP_ACP, 0, utf8.data(), static_cast<int>(utf8.size()),
                            ansi.data(), ansiNeeded);
        return ansi;
    }
    std::wstring result(static_cast<size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                        result.data(), needed);
    return result;
}

std::string toUtf8(const std::wstring& wide) {
    if (wide.empty()) return std::string();
    const int needed = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                                           static_cast<int>(wide.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string result(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                        result.data(), needed, nullptr, nullptr);
    return result;
}

// ------------------------------------------------------------------ strings
std::wstring formatDouble1(double value) {
    wchar_t buffer[64];
    swprintf(buffer, 64, L"%.1f", value);
    return buffer;
}

std::wstring formatSize(uint64_t bytes) {
    if (bytes < 1024) {
        return std::to_wstring(bytes) + L" B";
    }
    if (bytes < 1024ull * 1024ull) {
        return formatDouble1(static_cast<double>(bytes) / 1024.0) + L" KB";
    }
    return formatDouble1(static_cast<double>(bytes) / (1024.0 * 1024.0)) + L" MB";
}

int compareNoCase(const std::wstring& a, const std::wstring& b) {
    const int n = CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()),
                                       b.c_str(), static_cast<int>(b.size()), TRUE);
    if (n == CSTR_LESS_THAN) return -1;
    if (n == CSTR_GREATER_THAN) return 1;
    return 0;
}

bool startsWithNoCase(const std::wstring& text, const std::wstring& prefix) {
    if (text.size() < prefix.size()) return false;
    return compareNoCase(text.substr(0, prefix.size()), prefix) == 0;
}

std::wstring trim(const std::wstring& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && iswspace(text[begin])) ++begin;
    while (end > begin && iswspace(text[end - 1])) --end;
    return text.substr(begin, end - begin);
}

// --------------------------------------------------------------------- paths
static bool isSeparator(wchar_t c) { return c == L'\\' || c == L'/'; }

std::wstring pathExtension(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    const size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return std::wstring();
    if (slash != std::wstring::npos && dot < slash) return std::wstring();
    std::wstring ext = path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return ext;
}

std::wstring pathFileName(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? path : path.substr(slash + 1);
}

std::wstring pathStem(const std::wstring& path) {
    const std::wstring name = pathFileName(path);
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return name;
    return name.substr(0, dot);
}

std::wstring pathParent(const std::wstring& path) {
    size_t end = path.size();
    while (end > 0 && isSeparator(path[end - 1])) --end;
    const size_t slash = path.find_last_of(L"\\/", end == 0 ? 0 : end - 1);
    if (slash == std::wstring::npos) return std::wstring();
    if (slash == 2 && path.size() > 2 && path[1] == L':') return path.substr(0, 3); // "C:\"
    return path.substr(0, slash);
}

std::wstring pathJoin(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    std::wstring result = a;
    if (!isSeparator(result.back())) result.push_back(L'\\');
    size_t start = 0;
    while (start < b.size() && isSeparator(b[start])) ++start;
    result.append(b, start, std::wstring::npos);
    return result;
}

std::wstring pathRelative(const std::wstring& path, const std::wstring& base) {
    if (base.empty()) return pathFileName(path);
    std::wstring normalizedBase = base;
    while (!normalizedBase.empty() && isSeparator(normalizedBase.back()))
        normalizedBase.pop_back();

    if (path.size() > normalizedBase.size() + 1 &&
        isSeparator(path[normalizedBase.size()]) &&
        compareNoCase(path.substr(0, normalizedBase.size()), normalizedBase) == 0) {
        return path.substr(normalizedBase.size() + 1);
    }
    return pathFileName(path);
}

std::wstring pathReplaceExtension(const std::wstring& path, const std::wstring& extWithDot) {
    const std::wstring name = pathFileName(path);
    const size_t dot = name.find_last_of(L'.');
    const std::wstring parent = pathParent(path);
    const std::wstring stem = (dot == std::wstring::npos || dot == 0) ? name : name.substr(0, dot);
    const std::wstring newName = stem + extWithDot;
    return parent.empty() ? newName : pathJoin(parent, newName);
}

std::wstring normalizeSlashes(const std::wstring& path) {
    std::wstring result = path;
    std::replace(result.begin(), result.end(), L'\\', L'/');
    return result;
}

// --------------------------------------------------------------------- files
bool fileExists(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t fileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}

bool removeFile(const std::wstring& path) {
    if (path.empty()) return true;
    if (!fileExists(path)) return true;
    if (DeleteFileW(path.c_str())) return true;
    // A read-only or locked file: try once more without the read-only attribute.
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    return DeleteFileW(path.c_str()) != 0;
}

bool ensureDirectory(const std::wstring& dir) {
    if (dir.empty()) return false;
    const DWORD attributes = GetFileAttributesW(dir.c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }
    // Create parents first.
    const std::wstring parent = pathParent(dir);
    if (!parent.empty() && compareNoCase(parent, dir) != 0) {
        ensureDirectory(parent);
    }
    return CreateDirectoryW(dir.c_str(), nullptr) != 0 ||
           GetLastError() == ERROR_ALREADY_EXISTS;
}

bool ensureParentDirectory(const std::wstring& filePath) {
    const std::wstring parent = pathParent(filePath);
    if (parent.empty()) return true;
    return ensureDirectory(parent);
}

bool readAllBytes(const std::wstring& path, std::vector<uint8_t>& out, std::wstring* error) {
    out.clear();
    FILE* file = _wfopen(path.c_str(), L"rb");
    if (!file) {
        if (error) *error = L"无法打开文件: " + pathFileName(path);
        return false;
    }
    _fseeki64(file, 0, SEEK_END);
    const long long size = _ftelli64(file);
    _fseeki64(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        if (error) *error = L"无法读取文件大小";
        return false;
    }
    out.resize(static_cast<size_t>(size));
    if (size > 0) {
        const size_t read = fread(out.data(), 1, out.size(), file);
        if (read != out.size()) {
            fclose(file);
            out.clear();
            if (error) *error = L"读取文件失败";
            return false;
        }
    }
    fclose(file);
    return true;
}

bool writeAllBytes(const std::wstring& path, const uint8_t* data, size_t size, std::wstring* error) {
    if (!ensureParentDirectory(path)) {
        if (error) *error = L"无法创建输出目录";
        return false;
    }
    FILE* file = _wfopen(path.c_str(), L"wb");
    if (!file) {
        if (error) *error = L"无法写入文件: " + pathFileName(path);
        return false;
    }
    if (size > 0 && fwrite(data, 1, size, file) != size) {
        fclose(file);
        removeFile(path);
        if (error) *error = L"写入文件失败";
        return false;
    }
    fclose(file);
    return true;
}

void collectFiles(const std::wstring& root,
                  const std::vector<std::wstring>& extensions,
                  std::vector<std::wstring>& out) {
    out.clear();
    if (root.empty()) return;

    std::wstring pattern = pathJoin(root, L"*");
    WIN32_FIND_DATAW data{};
    HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) return;

    std::vector<std::wstring> subdirectories;
    do {
        const std::wstring name = data.cFileName;
        if (name == L"." || name == L"..") continue;

        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!name.empty() && name[0] == L'.') continue; // skip hidden, like the original
            if (data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) continue;
            subdirectories.push_back(pathJoin(root, name));
        } else {
            const std::wstring ext = pathExtension(name);
            if (ext.empty()) continue;
            for (const std::wstring& candidate : extensions) {
                if (ext == candidate) {
                    out.push_back(pathJoin(root, name));
                    break;
                }
            }
        }
    } while (FindNextFileW(handle, &data));
    FindClose(handle);

    for (const std::wstring& sub : subdirectories) {
        std::vector<std::wstring> nested;
        collectFiles(sub, extensions, nested);
        out.insert(out.end(), nested.begin(), nested.end());
    }
}

// -------------------------------------------------------------- error mapping
ErrorKind classifyError(const std::wstring& message) {
    std::wstring lower = message;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });

    auto contains = [&lower](const wchar_t* needle) {
        return lower.find(needle) != std::wstring::npos;
    };

    if (contains(L"decode") || contains(L"unexpected") || contains(L"corrupt") ||
        contains(L"解码")) {
        return ErrorKind::Decode;
    }
    if (contains(L"write") || contains(L"permission") || contains(L"写入") ||
        contains(L"拒绝访问")) {
        return ErrorKind::Write;
    }
    if (contains(L"unsupported") || contains(L"不支持") || contains(L"no decoder")) {
        return ErrorKind::Unsupported;
    }
    if (contains(L"decrypt") || contains(L"ncm") || contains(L"解密")) {
        return ErrorKind::NcmDecrypt;
    }
    return ErrorKind::Other;
}

std::wstring shorten(const std::wstring& text, size_t maxChars) {
    if (text.size() <= maxChars) return text;
    return text.substr(0, maxChars);
}

} // namespace ac
