// ncm.cpp - NetEase Cloud Music (.ncm) container decryptor.
//
// Ported from the original Python ncm_decryptor.py. The container layout is:
//
//   [0,8)     magic "CTENFDAM"
//   [8,10)    gap
//   u32       RC4 key length, then that many bytes (XOR 0x64, AES-ECB, unpad, [17:])
//   u32       metadata length, then that many bytes (XOR 0x63, base64, AES-ECB,
//             unpad, UTF-8, skip the "music:" prefix, JSON)
//   4 + 5     CRC + gap
//   u32       cover image length, then the image bytes
//   rest      audio payload XORed with a keystream derived from the key box
#include "ncm.h"

#include <windows.h>

#include <cstring>
#include <stdexcept>

#include "crypto.h"
#include "util.h"

namespace ac {
namespace {

// "hzHRAmso5kInbaxW"
const uint8_t kCoreKey[16] = { 0x68,0x7A,0x48,0x52,0x41,0x6D,0x73,0x6F,
                               0x35,0x6B,0x49,0x6E,0x62,0x61,0x78,0x57 };
// "#14ljk_!\]&0U<'" + NUL
const uint8_t kMetaKey[16] = { 0x23,0x31,0x34,0x6C,0x6A,0x6B,0x5F,0x21,
                               0x5C,0x5D,0x26,0x30,0x55,0x3C,0x27,0x28 };

const uint8_t kMagic[8] = { 'C','T','E','N','F','D','A','M' };

uint32_t readU32LE(const uint8_t* data, size_t size, size_t offset) {
    if (offset + 4 > size) throw std::runtime_error("NCM file is truncated");
    return static_cast<uint32_t>(data[offset]) |
           (static_cast<uint32_t>(data[offset + 1]) << 8) |
           (static_cast<uint32_t>(data[offset + 2]) << 16) |
           (static_cast<uint32_t>(data[offset + 3]) << 24);
}

// Python's `_unpad`: drop the last `n` bytes where n is the final byte value.
void removePadding(std::vector<uint8_t>& data) {
    if (data.empty()) return;
    const size_t pad = data.back();
    if (pad == 0 || pad > data.size()) return;
    data.resize(data.size() - pad);
}

void appendUtf8(std::string& out, uint32_t codePoint) {
    if (codePoint <= 0x7F) {
        out.push_back(static_cast<char>(codePoint));
    } else if (codePoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else if (codePoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
}

// Parses a JSON string literal starting at `pos` (which must point at '"').
// Returns false when the literal is malformed.
bool parseJsonString(const std::string& json, size_t& pos, std::string& out) {
    if (pos >= json.size() || json[pos] != '"') return false;
    ++pos;
    out.clear();
    while (pos < json.size()) {
        const char c = json[pos++];
        if (c == '"') return true;
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (pos >= json.size()) return false;
        const char escape = json[pos++];
        switch (escape) {
            case '"':  out.push_back('"');  break;
            case '\\': out.push_back('\\'); break;
            case '/':  out.push_back('/');  break;
            case 'b':  out.push_back('\b'); break;
            case 'f':  out.push_back('\f'); break;
            case 'n':  out.push_back('\n'); break;
            case 'r':  out.push_back('\r'); break;
            case 't':  out.push_back('\t'); break;
            case 'u': {
                if (pos + 4 > json.size()) return false;
                uint32_t code = 0;
                for (int i = 0; i < 4; ++i) {
                    const char h = json[pos++];
                    code <<= 4;
                    if (h >= '0' && h <= '9')      code |= static_cast<uint32_t>(h - '0');
                    else if (h >= 'a' && h <= 'f') code |= static_cast<uint32_t>(h - 'a' + 10);
                    else if (h >= 'A' && h <= 'F') code |= static_cast<uint32_t>(h - 'A' + 10);
                    else return false;
                }
                // Combine a surrogate pair when present.
                if (code >= 0xD800 && code <= 0xDBFF && pos + 6 <= json.size() &&
                    json[pos] == '\\' && json[pos + 1] == 'u') {
                    uint32_t low = 0;
                    size_t probe = pos + 2;
                    bool valid = true;
                    for (int i = 0; i < 4; ++i) {
                        const char h = json[probe++];
                        low <<= 4;
                        if (h >= '0' && h <= '9')      low |= static_cast<uint32_t>(h - '0');
                        else if (h >= 'a' && h <= 'f') low |= static_cast<uint32_t>(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') low |= static_cast<uint32_t>(h - 'A' + 10);
                        else { valid = false; break; }
                    }
                    if (valid && low >= 0xDC00 && low <= 0xDFFF) {
                        code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                        pos = probe;
                    }
                }
                appendUtf8(out, code);
                break;
            }
            default:
                out.push_back(escape);
                break;
        }
    }
    return false;
}

// Finds "key" at the top level of the JSON object and returns its string value.
std::string jsonStringValue(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    size_t search = 0;
    while ((search = json.find(needle, search)) != std::string::npos) {
        size_t pos = search + needle.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                     json[pos] == '\r' || json[pos] == '\n')) {
            ++pos;
        }
        if (pos < json.size() && json[pos] == ':') {
            ++pos;
            while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' ||
                                         json[pos] == '\r' || json[pos] == '\n')) {
                ++pos;
            }
            std::string value;
            if (pos < json.size() && json[pos] == '"' && parseJsonString(json, pos, value))
                return value;
            return std::string();
        }
        search += needle.size();
    }
    return std::string();
}

// NCM stores the artist as [["name", id], ...]; return the first name.
std::string jsonFirstArrayString(const std::string& json, const std::string& key) {
    const std::string needle = "\"" + key + "\"";
    const size_t search = json.find(needle);
    if (search == std::string::npos) return std::string();
    size_t pos = json.find('[', search + needle.size());
    if (pos == std::string::npos) return std::string();
    pos = json.find('"', pos);
    if (pos == std::string::npos) return std::string();
    std::string value;
    if (!parseJsonString(json, pos, value)) return std::string();
    return value;
}

std::string mimeFromImageBytes(const std::vector<uint8_t>& data) {
    if (data.size() >= 3 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF)
        return "image/jpeg";
    if (data.size() >= 8 && memcmp(data.data(), "\x89PNG\r\n\x1a\n", 8) == 0)
        return "image/png";
    if (data.size() >= 12 && memcmp(data.data(), "GIF8", 4) == 0)
        return "image/gif";
    return "image/jpeg";
}

} // namespace

std::string detectAudioFormat(const uint8_t* data, size_t size) {
    if (size >= 4 && memcmp(data, "fLaC", 4) == 0) return "flac";
    if (size >= 3 && memcmp(data, "ID3", 3) == 0) return "mp3";
    if (size >= 2 && data[0] == 0xFF && (data[1] & 0xE0) == 0xE0) return "mp3";
    if (size >= 4 && memcmp(data, "OggS", 4) == 0) return "ogg";
    if (size >= 12 && memcmp(data, "RIFF", 4) == 0 && memcmp(data + 8, "WAVE", 4) == 0)
        return "wav";
    if (size >= 12 && memcmp(data + 4, "ftyp", 4) == 0) return "m4a";
    return "unknown";
}

NcmResult decryptNcm(const std::vector<uint8_t>& container) {
    const uint8_t* data = container.data();
    const size_t size = container.size();

    if (size < 16 || memcmp(data, kMagic, 8) != 0)
        throw std::runtime_error("Not a valid NCM file (bad magic)");

    NcmResult result;
    size_t pos = 10; // skip magic + 2-byte gap

    // -------- RC4 key -----------------------------------------------------
    const uint32_t keyLength = readU32LE(data, size, pos);
    pos += 4;
    if (pos + keyLength > size) throw std::runtime_error("NCM key block is truncated");

    std::vector<uint8_t> keyData(data + pos, data + pos + keyLength);
    pos += keyLength;
    for (uint8_t& byte : keyData) byte ^= 0x64;

    if (keyData.empty() || keyData.size() % 16 != 0)
        throw std::runtime_error("NCM key block has an unexpected size");

    Aes coreCipher(kCoreKey, sizeof(kCoreKey));
    coreCipher.decryptEcb(keyData.data(), keyData.size());
    removePadding(keyData);
    if (keyData.size() <= 17) throw std::runtime_error("NCM key block is too short");
    keyData.erase(keyData.begin(), keyData.begin() + 17);

    // RC4-style key schedule.
    uint8_t keyBox[256];
    for (int i = 0; i < 256; ++i) keyBox[i] = static_cast<uint8_t>(i);
    {
        uint8_t lastByte = 0;
        size_t keyOffset = 0;
        for (int i = 0; i < 256; ++i) {
            const uint8_t swap = keyBox[i];
            const uint8_t c = static_cast<uint8_t>(swap + lastByte + keyData[keyOffset]);
            ++keyOffset;
            if (keyOffset >= keyData.size()) keyOffset = 0;
            keyBox[i] = keyBox[c];
            keyBox[c] = swap;
            lastByte = c;
        }
    }

    // -------- metadata ----------------------------------------------------
    const uint32_t metaLength = readU32LE(data, size, pos);
    pos += 4;
    if (pos + metaLength > size) throw std::runtime_error("NCM metadata block is truncated");

    std::vector<uint8_t> metaData(data + pos, data + pos + metaLength);
    pos += metaLength;
    for (uint8_t& byte : metaData) byte ^= 0x63;

    std::string metaJson;
    if (metaData.size() > 22 + 16) {
        std::vector<uint8_t> decoded;
        base64Decode(metaData.data() + 22, metaData.size() - 22, decoded);
        if (!decoded.empty() && decoded.size() % 16 == 0) {
            Aes metaCipher(kMetaKey, sizeof(kMetaKey));
            metaCipher.decryptEcb(decoded.data(), decoded.size());
            removePadding(decoded);
            std::string text(decoded.begin(), decoded.end());
            if (text.size() > 6) metaJson = text.substr(6); // skip the "music:" prefix
        }
    }

    if (!metaJson.empty()) {
        const std::string format = jsonStringValue(metaJson, "format");
        if (!format.empty()) result.info.format = format;
        result.info.musicName = jsonStringValue(metaJson, "musicName");
        result.info.album = jsonStringValue(metaJson, "album");
        result.info.artist = jsonFirstArrayString(metaJson, "artist");
    }
    if (result.info.format.empty()) result.info.format = "mp3";

    // -------- cover image -------------------------------------------------
    if (pos + 9 + 4 > size) throw std::runtime_error("NCM file is truncated");
    pos += 4 + 5; // CRC + gap
    const uint32_t imageSize = readU32LE(data, size, pos);
    pos += 4;
    if (pos + imageSize > size) throw std::runtime_error("NCM cover block is truncated");
    if (imageSize > 0) {
        result.info.cover.assign(data + pos, data + pos + imageSize);
        result.info.coverMime = mimeFromImageBytes(result.info.cover);
    }
    pos += imageSize;

    // -------- audio payload -----------------------------------------------
    if (pos >= size) throw std::runtime_error("NCM file contains no audio data");

    const size_t audioLength = size - pos;
    result.audio.resize(audioLength);
    for (size_t i = 1; i <= audioLength; ++i) {
        const size_t j = i & 0xFF;
        const uint8_t key = keyBox[(keyBox[j] + keyBox[(keyBox[j] + j) & 0xFF]) & 0xFF];
        result.audio[i - 1] = data[pos + i - 1] ^ key;
    }

    if (result.audio.empty()) throw std::runtime_error("Decrypted output is empty");
    return result;
}

NcmResult decryptNcmFile(const std::wstring& path) {
    std::vector<uint8_t> container;
    std::wstring error;
    if (!readAllBytes(path, container, &error))
        throw std::runtime_error(toUtf8(error));
    return decryptNcm(container);
}

std::vector<uint8_t> base64Encode(const uint8_t* data, size_t size) {
    static const char* alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<uint8_t> out;
    out.reserve((size + 2) / 3 * 4);
    size_t i = 0;
    while (i + 3 <= size) {
        const uint32_t triple = (static_cast<uint32_t>(data[i]) << 16) |
                                (static_cast<uint32_t>(data[i + 1]) << 8) |
                                 static_cast<uint32_t>(data[i + 2]);
        out.push_back(static_cast<uint8_t>(alphabet[(triple >> 18) & 0x3F]));
        out.push_back(static_cast<uint8_t>(alphabet[(triple >> 12) & 0x3F]));
        out.push_back(static_cast<uint8_t>(alphabet[(triple >> 6) & 0x3F]));
        out.push_back(static_cast<uint8_t>(alphabet[triple & 0x3F]));
        i += 3;
    }
    if (i < size) {
        uint32_t triple = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < size) triple |= static_cast<uint32_t>(data[i + 1]) << 8;
        out.push_back(static_cast<uint8_t>(alphabet[(triple >> 18) & 0x3F]));
        out.push_back(static_cast<uint8_t>(alphabet[(triple >> 12) & 0x3F]));
        out.push_back((i + 1 < size) ? static_cast<uint8_t>(alphabet[(triple >> 6) & 0x3F]) : '=');
        out.push_back('=');
    }
    return out;
}

} // namespace ac
