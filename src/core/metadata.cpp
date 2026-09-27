// metadata.cpp - reads tags and cover art from FLAC, MP3, M4A and decrypted
// NCM payloads, and writes them into FLAC output as Vorbis comments + pictures.
//
// The original application used mutagen for this. The readers below cover the
// frames/atoms that mutagen's dict-style access exposed, including the raw JPEG
// scan fallback used when a container has no usable picture frame.
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "audio.h"
#include "ncm.h"
#include "util.h"

namespace ac {
namespace {

constexpr size_t kMaxTagBytes = 32u * 1024 * 1024;   // guard against bogus sizes
constexpr size_t kJpegScanBytes = 128u * 1024;       // matches the original fallback

// ---------------------------------------------------------------- primitives
uint32_t readLE32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t readBE16(const uint8_t* p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}
uint32_t readBE32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
}
uint32_t readSynchsafe(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0] & 0x7F) << 21) |
           (static_cast<uint32_t>(p[1] & 0x7F) << 14) |
           (static_cast<uint32_t>(p[2] & 0x7F) << 7) |
            static_cast<uint32_t>(p[3] & 0x7F);
}

std::string upperKey(const std::string& text) {
    std::string result = text;
    for (char& c : result) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return result;
}

std::string trimTrailingNuls(const std::string& text) {
    size_t end = text.size();
    while (end > 0 && (text[end - 1] == '\0' || text[end - 1] == ' ')) --end;
    return text.substr(0, end);
}

void appendUtf8(std::string& out, uint32_t codePoint) {
    if (codePoint == 0) return;
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

std::string latin1ToUtf8(const uint8_t* data, size_t size) {
    std::string out;
    out.reserve(size);
    for (size_t i = 0; i < size; ++i) appendUtf8(out, data[i]);
    return out;
}

std::string utf16ToUtf8(const uint8_t* data, size_t size) {
    bool littleEndian = true;
    size_t offset = 0;
    if (size >= 2) {
        if (data[0] == 0xFF && data[1] == 0xFE) { littleEndian = true; offset = 2; }
        else if (data[0] == 0xFE && data[1] == 0xFF) { littleEndian = false; offset = 2; }
    }
    std::string out;
    out.reserve(size / 2);
    for (size_t i = offset; i + 1 < size; i += 2) {
        const uint16_t unit = littleEndian
            ? static_cast<uint16_t>(data[i] | (data[i + 1] << 8))
            : static_cast<uint16_t>((data[i] << 8) | data[i + 1]);
        if (unit == 0) break;
        if (unit >= 0xD800 && unit <= 0xDBFF && i + 3 < size) {
            const uint16_t low = littleEndian
                ? static_cast<uint16_t>(data[i + 2] | (data[i + 3] << 8))
                : static_cast<uint16_t>((data[i + 2] << 8) | data[i + 3]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                appendUtf8(out, 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00));
                i += 2;
                continue;
            }
        }
        appendUtf8(out, unit);
    }
    return out;
}

std::string decodeText(const uint8_t* data, size_t size, uint8_t encoding) {
    switch (encoding) {
        case 1: return utf16ToUtf8(data, size);
        case 2: {
            // UTF-16BE without a BOM.
            std::string out;
            out.reserve(size / 2);
            for (size_t i = 0; i + 1 < size; i += 2) {
                const uint16_t unit = static_cast<uint16_t>((data[i] << 8) | data[i + 1]);
                if (unit == 0) break;
                appendUtf8(out, unit);
            }
            return out;
        }
        case 3: return std::string(reinterpret_cast<const char*>(data), size);
        default: return latin1ToUtf8(data, size);
    }
}

// Size of a NUL-terminated string inside an ID3 frame body, honouring the
// frame encoding (1 or 2 bytes per character).
size_t terminatedLength(const uint8_t* data, size_t size, uint8_t encoding) {
    if (encoding == 1 || encoding == 2) {
        for (size_t i = 0; i + 1 < size; i += 2) {
            if (data[i] == 0 && data[i + 1] == 0) return i;
        }
        return size;
    }
    for (size_t i = 0; i < size; ++i) {
        if (data[i] == 0) return i;
    }
    return size;
}

bool hasField(const AudioTags& tags, const std::string& key) {
    for (const auto& field : tags.fields) {
        if (field.first == key) return true;
    }
    return false;
}

void addIfMissing(AudioTags& tags, const std::string& key, const std::string& value) {
    if (!hasField(tags, key)) tags.set(key, value);
}

// ------------------------------------------------------- image introspection
void parseImageDimensions(Picture& picture) {
    const std::vector<uint8_t>& d = picture.data;
    if (d.size() >= 24 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G') {
        picture.width = readBE32(d.data() + 16);
        picture.height = readBE32(d.data() + 20);
        const uint8_t bitDepth = d[24];
        const uint8_t colorType = d[25];
        uint32_t channels = 1;
        switch (colorType) {
            case 2: channels = 3; break;
            case 3: channels = 1; picture.colors = 1u << bitDepth; break;
            case 4: channels = 2; break;
            case 6: channels = 4; break;
            default: channels = 1; break;
        }
        picture.depth = static_cast<uint32_t>(bitDepth) * channels;
        return;
    }
    if (d.size() >= 4 && d[0] == 0xFF && d[1] == 0xD8) {
        size_t pos = 2;
        while (pos + 4 <= d.size()) {
            if (d[pos] != 0xFF) { ++pos; continue; }
            const uint8_t marker = d[pos + 1];
            if (marker == 0xD8 || marker == 0x01 || (marker >= 0xD0 && marker <= 0xD7)) {
                pos += 2;
                continue;
            }
            if (marker == 0xD9 || marker == 0xDA) break;
            const uint16_t segmentLength = readBE16(d.data() + pos + 2);
            if (segmentLength < 2) break;
            const bool isStartOfFrame =
                (marker >= 0xC0 && marker <= 0xCF) && marker != 0xC4 && marker != 0xC8 &&
                marker != 0xCC;
            if (isStartOfFrame && pos + 9 <= d.size()) {
                const uint8_t precision = d[pos + 4];
                picture.height = readBE16(d.data() + pos + 5);
                picture.width = readBE16(d.data() + pos + 7);
                const uint8_t components = d[pos + 9];
                picture.depth = static_cast<uint32_t>(precision) * components;
                return;
            }
            pos += 2 + segmentLength;
        }
    }
}

// ------------------------------------------------------------ FLAC / Vorbis
void parseVorbisComment(const uint8_t* data, size_t size, AudioTags& tags) {
    if (size < 8) return;
    size_t pos = 0;
    const uint32_t vendorLength = readLE32(data + pos);
    pos += 4;
    if (pos + vendorLength > size) return;
    pos += vendorLength;
    if (pos + 4 > size) return;

    const uint32_t count = readLE32(data + pos);
    pos += 4;
    for (uint32_t i = 0; i < count && pos + 4 <= size; ++i) {
        const uint32_t length = readLE32(data + pos);
        pos += 4;
        if (pos + length > size) break;
        const uint8_t* entry = data + pos;
        pos += length;

        size_t equals = 0;
        while (equals < length && entry[equals] != '=') ++equals;
        if (equals == 0 || equals >= length) continue;
        const std::string key = upperKey(std::string(reinterpret_cast<const char*>(entry), equals));
        const std::string value(reinterpret_cast<const char*>(entry + equals + 1),
                                length - equals - 1);
        tags.set(key, value);
    }
}

void parseFlacPicture(const uint8_t* data, size_t size, AudioTags& tags) {
    size_t pos = 0;
    auto need = [&](size_t bytes) { return pos + bytes <= size; };

    if (!need(8)) return;
    Picture picture;
    picture.type = readBE32(data + pos); pos += 4;

    const uint32_t mimeLength = readBE32(data + pos); pos += 4;
    if (!need(mimeLength)) return;
    picture.mime.assign(reinterpret_cast<const char*>(data + pos), mimeLength);
    pos += mimeLength;

    if (!need(4)) return;
    const uint32_t descriptionLength = readBE32(data + pos); pos += 4;
    if (!need(descriptionLength)) return;
    picture.description.assign(reinterpret_cast<const char*>(data + pos), descriptionLength);
    pos += descriptionLength;

    if (!need(20)) return;
    picture.width = readBE32(data + pos); pos += 4;
    picture.height = readBE32(data + pos); pos += 4;
    picture.depth = readBE32(data + pos); pos += 4;
    picture.colors = readBE32(data + pos); pos += 4;
    const uint32_t dataLength = readBE32(data + pos); pos += 4;
    if (!need(dataLength)) return;

    picture.data.assign(data + pos, data + pos + dataLength);
    if (picture.mime.empty()) picture.mime = "image/jpeg";
    if (picture.width == 0 || picture.height == 0) parseImageDimensions(picture);
    tags.pictures.push_back(std::move(picture));
}

void parseFlacBlocks(const uint8_t* data, size_t size, AudioTags& tags) {
    if (size < 4 || memcmp(data, "fLaC", 4) != 0) return;
    size_t pos = 4;
    int guard = 0;
    while (pos + 4 <= size && guard++ < 512) {
        const uint8_t header = data[pos];
        const uint32_t length = (static_cast<uint32_t>(data[pos + 1]) << 16) |
                                (static_cast<uint32_t>(data[pos + 2]) << 8) |
                                 static_cast<uint32_t>(data[pos + 3]);
        pos += 4;
        if (pos + length > size) break;
        const uint8_t type = header & 0x7F;
        if (type == 4) parseVorbisComment(data + pos, length, tags);
        else if (type == 6) parseFlacPicture(data + pos, length, tags);
        pos += length;
        if (header & 0x80) break;
    }
}

// -------------------------------------------------------------------- ID3v2
void handleId3Frame(const std::string& id, const uint8_t* body, size_t size,
                    uint8_t version, AudioTags& tags);

void parseId3v2(const uint8_t* data, size_t size, AudioTags& tags) {
    if (size < 10 || memcmp(data, "ID3", 3) != 0) return;
    const uint8_t version = data[3];
    const uint8_t flags = data[5];
    const uint32_t tagSize = readSynchsafe(data + 6);
    size_t pos = 10;
    const size_t end = std::min<size_t>(size, static_cast<size_t>(10) + tagSize);

    if (flags & 0x40) { // extended header
        if (version >= 4) {
            if (pos + 4 > end) return;
            pos += readSynchsafe(data + pos);
        } else {
            if (pos + 4 > end) return;
            pos += 4 + readBE32(data + pos);
        }
    }

    if (version == 2) {
        while (pos + 6 <= end) {
            if (data[pos] == 0) break;
            const std::string id(reinterpret_cast<const char*>(data + pos), 3);
            const uint32_t frameSize = (static_cast<uint32_t>(data[pos + 3]) << 16) |
                                       (static_cast<uint32_t>(data[pos + 4]) << 8) |
                                        static_cast<uint32_t>(data[pos + 5]);
            pos += 6;
            if (frameSize == 0 || pos + frameSize > end) break;
            handleId3Frame(id, data + pos, frameSize, version, tags);
            pos += frameSize;
        }
        return;
    }

    while (pos + 10 <= end) {
        if (data[pos] == 0) break; // padding
        const std::string id(reinterpret_cast<const char*>(data + pos), 4);
        const uint32_t frameSize = (version >= 4) ? readSynchsafe(data + pos + 4)
                                                  : readBE32(data + pos + 4);
        pos += 10;
        if (frameSize == 0 || pos + frameSize > end) break;
        handleId3Frame(id, data + pos, frameSize, version, tags);
        pos += frameSize;
    }
}

void handleId3Frame(const std::string& id, const uint8_t* body, size_t size,
                    uint8_t version, AudioTags& tags) {
    (void)version;
    if (size == 0) return;

    static const std::pair<const char*, const char*> kTextFrames[] = {
        { "TIT2", "TITLE" }, { "TT2", "TITLE" },
        { "TPE1", "ARTIST" }, { "TP1", "ARTIST" },
        { "TPE2", "ALBUMARTIST" }, { "TP2", "ALBUMARTIST" },
        { "TALB", "ALBUM" }, { "TAL", "ALBUM" },
        { "TRCK", "TRACKNUMBER" }, { "TRK", "TRACKNUMBER" },
        { "TPOS", "DISCNUMBER" }, { "TPA", "DISCNUMBER" },
        { "TCON", "GENRE" }, { "TCO", "GENRE" },
        { "TDRC", "DATE" }, { "TYER", "DATE" }, { "TYE", "DATE" },
        { "TCOM", "COMPOSER" }, { "TCM", "COMPOSER" },
        { "TENC", "ENCODEDBY" }, { "TEN", "ENCODEDBY" },
        { "TSSE", "ENCODERSETTINGS" },
        { "TPUB", "ORGANIZATION" }, { "TPB", "ORGANIZATION" },
        { "TCOP", "COPYRIGHT" }, { "TCR", "COPYRIGHT" },
        { "TLAN", "LANGUAGE" }, { "TLA", "LANGUAGE" },
        { "TIT1", "GROUPING" }, { "TT1", "GROUPING" },
        { "TBPM", "BPM" }, { "TBP", "BPM" },
        { "TSRC", "ISRC" }, { "TRC", "ISRC" },
    };
    for (const auto& mapping : kTextFrames) {
        if (id == mapping.first) {
            const std::string text = trimTrailingNuls(decodeText(body + 1, size - 1, body[0]));
            if (!text.empty()) tags.set(mapping.second, text);
            return;
        }
    }

    if (id == "TXXX" || id == "TXX") {
        const uint8_t encoding = body[0];
        const size_t descLength = terminatedLength(body + 1, size - 1, encoding);
        const std::string description =
            upperKey(decodeText(body + 1, descLength, encoding));
        const size_t valueOffset = 1 + descLength + ((encoding == 1 || encoding == 2) ? 2 : 1);
        if (valueOffset >= size) return;
        const std::string value =
            trimTrailingNuls(decodeText(body + valueOffset, size - valueOffset, encoding));
        if (!description.empty() && !value.empty()) tags.set(description, value);
        return;
    }

    if (id == "COMM" || id == "COM") {
        if (size < 5) return;
        const uint8_t encoding = body[0];
        const size_t descLength = terminatedLength(body + 4, size - 4, encoding);
        const size_t valueOffset = 4 + descLength + ((encoding == 1 || encoding == 2) ? 2 : 1);
        if (valueOffset >= size) return;
        const std::string value =
            trimTrailingNuls(decodeText(body + valueOffset, size - valueOffset, encoding));
        if (!value.empty()) addIfMissing(tags, "COMMENT", value);
        return;
    }

    if (id == "USLT" || id == "ULT") {
        if (size < 5) return;
        const uint8_t encoding = body[0];
        const size_t descLength = terminatedLength(body + 4, size - 4, encoding);
        const size_t valueOffset = 4 + descLength + ((encoding == 1 || encoding == 2) ? 2 : 1);
        if (valueOffset >= size) return;
        const std::string value =
            trimTrailingNuls(decodeText(body + valueOffset, size - valueOffset, encoding));
        if (!value.empty()) addIfMissing(tags, "LYRICS", value);
        return;
    }

    if (id == "APIC" || id == "PIC") {
        Picture picture;
        size_t pos = 1;
        if (id == "APIC") {
            const size_t mimeLength = terminatedLength(body + pos, size - pos, 0);
            picture.mime.assign(reinterpret_cast<const char*>(body + pos), mimeLength);
            pos += mimeLength + 1;
        } else {
            if (pos + 3 > size) return;
            const std::string format(reinterpret_cast<const char*>(body + pos), 3);
            picture.mime = (format == "PNG") ? "image/png" : "image/jpeg";
            pos += 3;
        }
        if (pos >= size) return;
        picture.type = body[pos++];
        const uint8_t encoding = body[0];
        const size_t descLength = terminatedLength(body + pos, size - pos, encoding);
        picture.description = decodeText(body + pos, descLength, encoding);
        pos += descLength + ((encoding == 1 || encoding == 2) ? 2 : 1);
        if (pos >= size) return;
        picture.data.assign(body + pos, body + size);
        if (picture.mime.empty()) picture.mime = "image/jpeg";
        parseImageDimensions(picture);
        tags.pictures.push_back(std::move(picture));
        return;
    }
}

void parseId3v1(const uint8_t* data, size_t size, AudioTags& tags) {
    if (size < 128 || memcmp(data, "TAG", 3) != 0) return;
    auto field = [&](size_t offset, size_t length) {
        size_t end = offset + length;
        while (end > offset && (data[end - 1] == 0 || data[end - 1] == ' ')) --end;
        return latin1ToUtf8(data + offset, end - offset);
    };
    addIfMissing(tags, "TITLE", field(3, 30));
    addIfMissing(tags, "ARTIST", field(33, 30));
    addIfMissing(tags, "ALBUM", field(63, 30));
    addIfMissing(tags, "DATE", field(93, 4));
    addIfMissing(tags, "COMMENT", field(97, 28));
    if (data[125] == 0 && data[126] != 0) {
        char buffer[8];
        snprintf(buffer, sizeof(buffer), "%u", static_cast<unsigned>(data[126]));
        addIfMissing(tags, "TRACKNUMBER", buffer);
    }
}

// ----------------------------------------------------------------- MP4 atoms
struct AtomInfo {
    char name[5] = {};
    size_t payloadBegin = 0;
    size_t payloadEnd = 0;
};

bool nextAtom(const uint8_t* data, size_t bound, size_t& pos, AtomInfo& info) {
    if (pos + 8 > bound) return false;
    uint64_t atomSize = readBE32(data + pos);
    size_t headerSize = 8;
    if (atomSize == 1) {
        if (pos + 16 > bound) return false;
        atomSize = (static_cast<uint64_t>(readBE32(data + pos + 8)) << 32) |
                    readBE32(data + pos + 12);
        headerSize = 16;
    } else if (atomSize == 0) {
        atomSize = bound - pos;
    }
    if (atomSize < headerSize || pos + atomSize > bound) return false;

    memcpy(info.name, data + pos + 4, 4);
    info.name[4] = '\0';
    info.payloadBegin = pos + headerSize;
    info.payloadEnd = pos + static_cast<size_t>(atomSize);
    pos += static_cast<size_t>(atomSize);
    return true;
}

bool isContainerAtom(const char* name) {
    static const char* kContainers[] = {
        "moov", "udta", "trak", "mdia", "minf", "stbl", "meta", "ilst", "edts"
    };
    for (const char* container : kContainers) {
        if (strcmp(name, container) == 0) return true;
    }
    return false;
}

void findIlstAtoms(const uint8_t* data, size_t bound, size_t begin, size_t end, int depth,
                   std::vector<AtomInfo>& found) {
    if (depth > 8) return;
    size_t pos = begin;
    AtomInfo info;
    while (pos < end && nextAtom(data, end > bound ? end : bound, pos, info)) {
        if (strcmp(info.name, "ilst") == 0) {
            found.push_back(info);
            continue;
        }
        if (isContainerAtom(info.name)) {
            size_t childBegin = info.payloadBegin;
            if (strcmp(info.name, "meta") == 0) childBegin += 4; // FullBox version + flags
            if (childBegin <= info.payloadEnd) {
                findIlstAtoms(data, bound, childBegin, info.payloadEnd, depth + 1, found);
            }
        }
    }
}

void parseIlst(const uint8_t* data, size_t bound, const AtomInfo& ilst, AudioTags& tags) {
    size_t pos = ilst.payloadBegin;
    AtomInfo tag;
    while (pos < ilst.payloadEnd && nextAtom(data, bound, pos, tag)) {
        size_t dataPos = tag.payloadBegin;
        AtomInfo dataAtom;
        bool found = false;
        while (dataPos < tag.payloadEnd && nextAtom(data, bound, dataPos, dataAtom)) {
            if (strcmp(dataAtom.name, "data") == 0 && dataAtom.payloadBegin + 8 <= dataAtom.payloadEnd) {
                found = true;
                break;
            }
        }
        if (!found) continue;

        const uint32_t wellKnownType = readBE32(data + dataAtom.payloadBegin) & 0x00FFFFFF;
        const uint8_t* content = data + dataAtom.payloadBegin + 8;
        const size_t contentSize = dataAtom.payloadEnd - dataAtom.payloadBegin - 8;

        const std::string name(tag.name, 4);
        if (name == "covr") {
            Picture picture;
            picture.type = 3;
            picture.mime = (wellKnownType == 14) ? "image/png" : "image/jpeg";
            picture.data.assign(content, content + contentSize);
            parseImageDimensions(picture);
            tags.pictures.push_back(std::move(picture));
            continue;
        }
        if (name == "trkn" || name == "disk") {
            if (contentSize >= 4) {
                const uint16_t number = readBE16(content + 2);
                const uint16_t total = (contentSize >= 6) ? readBE16(content + 4) : 0;
                if (number > 0) {
                    char buffer[32];
                    if (total > 0) snprintf(buffer, sizeof(buffer), "%u/%u", number, total);
                    else snprintf(buffer, sizeof(buffer), "%u", number);
                    tags.set(name == "trkn" ? "TRACKNUMBER" : "DISCNUMBER", buffer);
                }
            }
            continue;
        }

        // Note: the © atom names start with byte 0xA9, written as a separate
        // literal so the escape cannot swallow the following character.
        static const std::pair<const char*, const char*> kMp4Tags[] = {
            { "\xA9" "nam", "TITLE" },
            { "\xA9" "ART", "ARTIST" },
            { "aART", "ALBUMARTIST" },
            { "\xA9" "alb", "ALBUM" },
            { "\xA9" "day", "DATE" },
            { "\xA9" "gen", "GENRE" },
            { "\xA9" "cmt", "COMMENT" },
            { "\xA9" "wrt", "COMPOSER" },
            { "\xA9" "lyr", "LYRICS" },
            { "\xA9" "grp", "GROUPING" },
            { "\xA9" "too", "ENCODER" },
        };
        for (const auto& mapping : kMp4Tags) {
            if (name == mapping.first) {
                const std::string value = trimTrailingNuls(
                    std::string(reinterpret_cast<const char*>(content), contentSize));
                if (!value.empty()) tags.set(mapping.second, value);
                break;
            }
        }
    }
}

void parseMp4(const uint8_t* data, size_t size, AudioTags& tags) {
    std::vector<AtomInfo> ilstAtoms;
    findIlstAtoms(data, size, 0, size, 0, ilstAtoms);
    for (const AtomInfo& ilst : ilstAtoms) parseIlst(data, size, ilst, tags);
}

// ------------------------------------------------------------- misc fallback
void scanForJpeg(const uint8_t* data, size_t size, AudioTags& tags) {
    for (size_t i = 0; i + 1 < size; ++i) {
        if (data[i] != 0xFF || data[i + 1] != 0xD8) continue;
        for (size_t j = i + 2; j + 1 < size; ++j) {
            if (data[j] == 0xFF && data[j + 1] == 0xD9) {
                Picture picture;
                picture.type = 3;
                picture.mime = "image/jpeg";
                picture.data.assign(data + i, data + j + 2);
                parseImageDimensions(picture);
                tags.pictures.push_back(std::move(picture));
                return;
            }
        }
        return;
    }
}

// ------------------------------------------------------------- file reader
class FileReader {
public:
    ~FileReader() { if (file_) fclose(file_); }

    bool open(const std::wstring& path) {
        file_ = _wfopen(path.c_str(), L"rb");
        if (!file_) return false;
        _fseeki64(file_, 0, SEEK_END);
        size_ = static_cast<uint64_t>(_ftelli64(file_));
        return true;
    }

    size_t readAt(uint64_t offset, void* destination, size_t size) {
        if (!file_ || offset > size_) return 0;
        if (_fseeki64(file_, static_cast<long long>(offset), SEEK_SET) != 0) return 0;
        return fread(destination, 1, size, file_);
    }

    uint64_t size() const { return size_; }

private:
    FILE* file_ = nullptr;
    uint64_t size_ = 0;
};

} // namespace

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void describeImage(Picture& picture) {
    if (picture.width == 0 || picture.height == 0) parseImageDimensions(picture);
}

AudioTags readTagsFromFile(const std::wstring& path) {
    AudioTags tags;
    FileReader reader;
    if (!reader.open(path)) return tags;

    uint8_t header[16] = {};
    const size_t headerSize = reader.readAt(0, header, sizeof(header));
    const std::string format = detectAudioFormat(header, headerSize);

    if (format == "flac") {
        uint64_t pos = 4;
        int guard = 0;
        while (guard++ < 512) {
            uint8_t blockHeader[4];
            if (reader.readAt(pos, blockHeader, 4) != 4) break;
            const uint8_t type = blockHeader[0] & 0x7F;
            const bool last = (blockHeader[0] & 0x80) != 0;
            const uint32_t length = (static_cast<uint32_t>(blockHeader[1]) << 16) |
                                    (static_cast<uint32_t>(blockHeader[2]) << 8) |
                                     static_cast<uint32_t>(blockHeader[3]);
            pos += 4;
            if ((type == 4 || type == 6) && length > 0 && length <= kMaxTagBytes) {
                std::vector<uint8_t> payload(length);
                if (reader.readAt(pos, payload.data(), length) == length) {
                    if (type == 4) parseVorbisComment(payload.data(), payload.size(), tags);
                    else parseFlacPicture(payload.data(), payload.size(), tags);
                }
            }
            pos += length;
            if (last) break;
        }
        return tags;
    }

    if (format == "mp3") {
        if (headerSize >= 10 && memcmp(header, "ID3", 3) == 0) {
            const uint64_t tagSize = static_cast<uint64_t>(readSynchsafe(header + 6)) + 10;
            if (tagSize <= kMaxTagBytes) {
                std::vector<uint8_t> payload(static_cast<size_t>(tagSize));
                const size_t got = reader.readAt(0, payload.data(), payload.size());
                parseId3v2(payload.data(), got, tags);
            }
        }
        if (reader.size() >= 128) {
            uint8_t tail[128];
            if (reader.readAt(reader.size() - 128, tail, sizeof(tail)) == sizeof(tail))
                parseId3v1(tail, sizeof(tail), tags);
        }
        if (tags.pictures.empty()) {
            const size_t want = static_cast<size_t>(
                std::min<uint64_t>(reader.size(), kJpegScanBytes));
            std::vector<uint8_t> prefix(want);
            const size_t got = reader.readAt(0, prefix.data(), want);
            if (got > 0) scanForJpeg(prefix.data(), got, tags);
        }
        return tags;
    }

    if (format == "m4a") {
        uint64_t pos = 0;
        const uint64_t fileSize = reader.size();
        while (pos + 8 <= fileSize) {
            uint8_t atomHeader[16] = {};
            if (reader.readAt(pos, atomHeader, sizeof(atomHeader)) < 8) break;
            uint64_t atomSize = readBE32(atomHeader);
            size_t headerLength = 8;
            if (atomSize == 1) {
                atomSize = (static_cast<uint64_t>(readBE32(atomHeader + 8)) << 32) |
                            readBE32(atomHeader + 12);
                headerLength = 16;
            } else if (atomSize == 0) {
                atomSize = fileSize - pos;
            }
            if (atomSize < headerLength || pos + atomSize > fileSize) break;

            if (memcmp(atomHeader + 4, "moov", 4) == 0 && atomSize <= kMaxTagBytes) {
                std::vector<uint8_t> payload(static_cast<size_t>(atomSize));
                if (reader.readAt(pos, payload.data(), payload.size()) == payload.size())
                    parseMp4(payload.data(), payload.size(), tags);
                break;
            }
            pos += atomSize;
        }
        return tags;
    }

    return tags;
}

AudioTags readTagsFromPayload(const std::vector<uint8_t>& data, const std::string& format) {
    AudioTags tags;
    if (data.empty()) return tags;

    if (format == "flac") {
        parseFlacBlocks(data.data(), data.size(), tags);
    } else if (format == "mp3") {
        parseId3v2(data.data(), data.size(), tags);
        if (tags.pictures.empty()) {
            const size_t limit = std::min<size_t>(data.size(), kJpegScanBytes);
            scanForJpeg(data.data(), limit, tags);
        }
    } else if (format == "m4a" || format == "mp4") {
        parseMp4(data.data(), data.size(), tags);
    } else if (format == "wav" || format == "ogg") {
        // Not produced by NCM containers; nothing to copy.
    }
    return tags;
}

} // namespace ac
