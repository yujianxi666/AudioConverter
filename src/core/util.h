// util.h - string, path and file helpers for AudioConverter.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace ac {

// ---------------------------------------------------------------- conversion
std::wstring toWide(const std::string& utf8);
std::string  toUtf8(const std::wstring& wide);

// ------------------------------------------------------------------ strings
std::wstring formatSize(uint64_t bytes);   // "512 B", "12.3 KB", "4.5 MB"
std::wstring formatDouble1(double value);  // one fractional digit, like "%.1f"
int          compareNoCase(const std::wstring& a, const std::wstring& b);
bool         startsWithNoCase(const std::wstring& text, const std::wstring& prefix);
std::wstring trim(const std::wstring& text);

// --------------------------------------------------------------------- paths
std::wstring pathExtension(const std::wstring& path);   // ".flac" (lower case)
std::wstring pathFileName(const std::wstring& path);    // "song.flac"
std::wstring pathStem(const std::wstring& path);        // "song"
std::wstring pathParent(const std::wstring& path);      // "C:\music"
std::wstring pathJoin(const std::wstring& a, const std::wstring& b);
std::wstring pathRelative(const std::wstring& path, const std::wstring& base);
std::wstring pathReplaceExtension(const std::wstring& path, const std::wstring& extWithDot);
std::wstring normalizeSlashes(const std::wstring& path); // '/' separators, for display

// --------------------------------------------------------------------- files
bool     fileExists(const std::wstring& path);
uint64_t fileSize(const std::wstring& path);
bool     removeFile(const std::wstring& path);
bool     ensureDirectory(const std::wstring& dir);
bool     ensureParentDirectory(const std::wstring& filePath);
bool     readAllBytes(const std::wstring& path, std::vector<uint8_t>& out, std::wstring* error);
bool     writeAllBytes(const std::wstring& path, const uint8_t* data, size_t size, std::wstring* error);

// Recursively collect files whose extension is in `extensions` (lower case, with dot).
// Hidden directories (leading '.') are skipped, matching the original behaviour.
void collectFiles(const std::wstring& root,
                  const std::vector<std::wstring>& extensions,
                  std::vector<std::wstring>& out);

// -------------------------------------------------------------- error mapping
// Mirrors the original _friendly_error() classification so the UI can show the
// same localised text for the same failure.
enum class ErrorKind { Decode, Write, Unsupported, NcmDecrypt, Other };

ErrorKind   classifyError(const std::wstring& message);
std::wstring shorten(const std::wstring& text, size_t maxChars);

} // namespace ac
