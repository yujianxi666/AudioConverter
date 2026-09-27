// crypto.cpp - self-contained AES, MD5 and base64.
#include "crypto.h"

#include <cstring>

namespace ac {

// ============================================================================
// AES
// ============================================================================
namespace {

const uint8_t kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,
    0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,
    0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,
    0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,
    0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,
    0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,
    0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,
    0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,
    0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,
    0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,
    0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16
};

const uint8_t kInvSbox[256] = {
    0x52,0x09,0x6a,0xd5,0x30,0x36,0xa5,0x38,0xbf,0x40,0xa3,0x9e,0x81,0xf3,0xd7,0xfb,
    0x7c,0xe3,0x39,0x82,0x9b,0x2f,0xff,0x87,0x34,0x8e,0x43,0x44,0xc4,0xde,0xe9,0xcb,
    0x54,0x7b,0x94,0x32,0xa6,0xc2,0x23,0x3d,0xee,0x4c,0x95,0x0b,0x42,0xfa,0xc3,0x4e,
    0x08,0x2e,0xa1,0x66,0x28,0xd9,0x24,0xb2,0x76,0x5b,0xa2,0x49,0x6d,0x8b,0xd1,0x25,
    0x72,0xf8,0xf6,0x64,0x86,0x68,0x98,0x16,0xd4,0xa4,0x5c,0xcc,0x5d,0x65,0xb6,0x92,
    0x6c,0x70,0x48,0x50,0xfd,0xed,0xb9,0xda,0x5e,0x15,0x46,0x57,0xa7,0x8d,0x9d,0x84,
    0x90,0xd8,0xab,0x00,0x8c,0xbc,0xd3,0x0a,0xf7,0xe4,0x58,0x05,0xb8,0xb3,0x45,0x06,
    0xd0,0x2c,0x1e,0x8f,0xca,0x3f,0x0f,0x02,0xc1,0xaf,0xbd,0x03,0x01,0x13,0x8a,0x6b,
    0x3a,0x91,0x11,0x41,0x4f,0x67,0xdc,0xea,0x97,0xf2,0xcf,0xce,0xf0,0xb4,0xe6,0x73,
    0x96,0xac,0x74,0x22,0xe7,0xad,0x35,0x85,0xe2,0xf9,0x37,0xe8,0x1c,0x75,0xdf,0x6e,
    0x47,0xf1,0x1a,0x71,0x1d,0x29,0xc5,0x89,0x6f,0xb7,0x62,0x0e,0xaa,0x18,0xbe,0x1b,
    0xfc,0x56,0x3e,0x4b,0xc6,0xd2,0x79,0x20,0x9a,0xdb,0xc0,0xfe,0x78,0xcd,0x5a,0xf4,
    0x1f,0xdd,0xa8,0x33,0x88,0x07,0xc7,0x31,0xb1,0x12,0x10,0x59,0x27,0x80,0xec,0x5f,
    0x60,0x51,0x7f,0xa9,0x19,0xb5,0x4a,0x0d,0x2d,0xe5,0x7a,0x9f,0x93,0xc9,0x9c,0xef,
    0xa0,0xe0,0x3b,0x4d,0xae,0x2a,0xf5,0xb0,0xc8,0xeb,0xbb,0x3c,0x83,0x53,0x99,0x61,
    0x17,0x2b,0x04,0x7e,0xba,0x77,0xd6,0x26,0xe1,0x69,0x14,0x63,0x55,0x21,0x0c,0x7d
};

const uint8_t kRcon[11] = { 0x00,0x01,0x02,0x04,0x08,0x10,0x20,0x40,0x80,0x1b,0x36 };

inline uint8_t xtime(uint8_t value) {
    return static_cast<uint8_t>((value << 1) ^ ((value & 0x80) ? 0x1b : 0x00));
}

// Multiply in GF(2^8) modulo the AES polynomial.
uint8_t gfMultiply(uint8_t a, uint8_t b) {
    uint8_t result = 0;
    while (b) {
        if (b & 1) result ^= a;
        a = xtime(a);
        b >>= 1;
    }
    return result;
}

// State layout follows FIPS-197: state[r][c] == block[r + 4 * c].
void shiftRows(uint8_t* s) {
    uint8_t t[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            t[r + 4 * c] = s[r + 4 * ((c + r) & 3)];
    memcpy(s, t, 16);
}

void inverseShiftRows(uint8_t* s) {
    uint8_t t[16];
    for (int c = 0; c < 4; ++c)
        for (int r = 0; r < 4; ++r)
            t[r + 4 * c] = s[r + 4 * ((c - r + 4) & 3)];
    memcpy(s, t, 16);
}

void mixColumns(uint8_t* s) {
    for (int c = 0; c < 4; ++c) {
        uint8_t* column = s + 4 * c;
        const uint8_t a0 = column[0], a1 = column[1], a2 = column[2], a3 = column[3];
        column[0] = static_cast<uint8_t>(xtime(a0) ^ (xtime(a1) ^ a1) ^ a2 ^ a3);
        column[1] = static_cast<uint8_t>(a0 ^ xtime(a1) ^ (xtime(a2) ^ a2) ^ a3);
        column[2] = static_cast<uint8_t>(a0 ^ a1 ^ xtime(a2) ^ (xtime(a3) ^ a3));
        column[3] = static_cast<uint8_t>((xtime(a0) ^ a0) ^ a1 ^ a2 ^ xtime(a3));
    }
}

void inverseMixColumns(uint8_t* s) {
    for (int c = 0; c < 4; ++c) {
        uint8_t* column = s + 4 * c;
        const uint8_t a0 = column[0], a1 = column[1], a2 = column[2], a3 = column[3];
        column[0] = static_cast<uint8_t>(gfMultiply(a0,14) ^ gfMultiply(a1,11) ^
                                         gfMultiply(a2,13) ^ gfMultiply(a3,9));
        column[1] = static_cast<uint8_t>(gfMultiply(a0,9)  ^ gfMultiply(a1,14) ^
                                         gfMultiply(a2,11) ^ gfMultiply(a3,13));
        column[2] = static_cast<uint8_t>(gfMultiply(a0,13) ^ gfMultiply(a1,9)  ^
                                         gfMultiply(a2,14) ^ gfMultiply(a3,11));
        column[3] = static_cast<uint8_t>(gfMultiply(a0,11) ^ gfMultiply(a1,13) ^
                                         gfMultiply(a2,9)  ^ gfMultiply(a3,14));
    }
}

} // namespace

Aes::Aes(const uint8_t* key, size_t keyLength) {
    const int nk = static_cast<int>(keyLength / 4);   // 4, 6 or 8
    rounds_ = nk + 6;

    const int totalWords = 4 * (rounds_ + 1);
    uint8_t* w = roundKeys_;
    memcpy(w, key, keyLength);

    uint8_t temp[4];
    for (int i = nk; i < totalWords; ++i) {
        temp[0] = w[(i - 1) * 4 + 0];
        temp[1] = w[(i - 1) * 4 + 1];
        temp[2] = w[(i - 1) * 4 + 2];
        temp[3] = w[(i - 1) * 4 + 3];

        if (i % nk == 0) {
            // RotWord + SubWord + Rcon
            const uint8_t t0 = temp[0];
            temp[0] = static_cast<uint8_t>(kSbox[temp[1]] ^ kRcon[i / nk]);
            temp[1] = kSbox[temp[2]];
            temp[2] = kSbox[temp[3]];
            temp[3] = kSbox[t0];
        } else if (nk > 6 && i % nk == 4) {
            temp[0] = kSbox[temp[0]];
            temp[1] = kSbox[temp[1]];
            temp[2] = kSbox[temp[2]];
            temp[3] = kSbox[temp[3]];
        }

        for (int b = 0; b < 4; ++b)
            w[i * 4 + b] = static_cast<uint8_t>(w[(i - nk) * 4 + b] ^ temp[b]);
    }
}

void Aes::addRoundKey(uint8_t* state, int round) const {
    const uint8_t* rk = roundKeys_ + round * 16;
    for (int i = 0; i < 16; ++i) state[i] ^= rk[i];
}

void Aes::encryptBlock(const uint8_t input[16], uint8_t output[16]) const {
    uint8_t state[16];
    memcpy(state, input, 16);

    addRoundKey(state, 0);
    for (int round = 1; round < rounds_; ++round) {
        for (int i = 0; i < 16; ++i) state[i] = kSbox[state[i]];
        shiftRows(state);
        mixColumns(state);
        addRoundKey(state, round);
    }
    for (int i = 0; i < 16; ++i) state[i] = kSbox[state[i]];
    shiftRows(state);
    addRoundKey(state, rounds_);

    memcpy(output, state, 16);
}

void Aes::decryptBlock(const uint8_t input[16], uint8_t output[16]) const {
    uint8_t state[16];
    memcpy(state, input, 16);

    addRoundKey(state, rounds_);
    for (int round = rounds_ - 1; round >= 1; --round) {
        inverseShiftRows(state);
        for (int i = 0; i < 16; ++i) state[i] = kInvSbox[state[i]];
        addRoundKey(state, round);
        inverseMixColumns(state);
    }
    inverseShiftRows(state);
    for (int i = 0; i < 16; ++i) state[i] = kInvSbox[state[i]];
    addRoundKey(state, 0);

    memcpy(output, state, 16);
}

void Aes::decryptEcb(uint8_t* data, size_t length) const {
    for (size_t offset = 0; offset + 16 <= length; offset += 16)
        decryptBlock(data + offset, data + offset);
}

void Aes::encryptEcb(uint8_t* data, size_t length) const {
    for (size_t offset = 0; offset + 16 <= length; offset += 16)
        encryptBlock(data + offset, data + offset);
}

bool aesSelfTest() {
    // FIPS-197 appendix C.1 AES-128 known answer.
    const uint8_t key[16] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,
                              0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f };
    const uint8_t plain[16] = { 0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,
                                0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff };
    const uint8_t expected[16] = { 0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,
                                   0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a };
    Aes aes(key, 16);

    uint8_t cipher[16];
    aes.encryptBlock(plain, cipher);
    if (memcmp(cipher, expected, 16) != 0) return false;

    uint8_t roundTrip[16];
    aes.decryptBlock(cipher, roundTrip);
    return memcmp(roundTrip, plain, 16) == 0;
}

// ============================================================================
// MD5
// ============================================================================
namespace {

const uint32_t kMd5Shift[64] = {
    7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
    5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
    4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
    6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
};

const uint32_t kMd5Table[64] = {
    0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,0xa8304613,0xfd469501,
    0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,0x6b901122,0xfd987193,0xa679438e,0x49b40821,
    0xf61e2562,0xc040b340,0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
    0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,0x676f02d9,0x8d2a4c8a,
    0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,
    0x289b7ec6,0xeaa127fa,0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
    0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,0xffeff47d,0x85845dd1,
    0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
};

inline uint32_t rotateLeft(uint32_t value, uint32_t count) {
    return (value << count) | (value >> (32 - count));
}

} // namespace

void Md5::reset() {
    state_[0] = 0x67452301;
    state_[1] = 0xefcdab89;
    state_[2] = 0x98badcfe;
    state_[3] = 0x10325476;
    bitCount_ = 0;
    bufferLength_ = 0;
}

void Md5::processBlock(const uint8_t* block) {
    uint32_t m[16];
    for (int i = 0; i < 16; ++i) {
        m[i] = static_cast<uint32_t>(block[i * 4]) |
               (static_cast<uint32_t>(block[i * 4 + 1]) << 8) |
               (static_cast<uint32_t>(block[i * 4 + 2]) << 16) |
               (static_cast<uint32_t>(block[i * 4 + 3]) << 24);
    }

    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t f;
        uint32_t g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) & 15;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) & 15;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) & 15;
        }
        const uint32_t temp = d;
        d = c;
        c = b;
        b = b + rotateLeft(a + f + kMd5Table[i] + m[g], kMd5Shift[i]);
        a = temp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
}

void Md5::update(const uint8_t* data, size_t length) {
    bitCount_ += static_cast<uint64_t>(length) * 8;
    while (length > 0) {
        const size_t take = (length < 64 - bufferLength_) ? length : (64 - bufferLength_);
        memcpy(buffer_ + bufferLength_, data, take);
        bufferLength_ += take;
        data += take;
        length -= take;
        if (bufferLength_ == 64) {
            processBlock(buffer_);
            bufferLength_ = 0;
        }
    }
}

void Md5::finish(uint8_t digest[16]) {
    const uint64_t totalBits = bitCount_;

    uint8_t lengthBytes[8];
    for (int i = 0; i < 8; ++i)
        lengthBytes[i] = static_cast<uint8_t>((totalBits >> (8 * i)) & 0xff);

    // Pad with 0x80 followed by zeros until the length field fits in this block.
    // padBuffer[0] is the 0x80 marker, the remaining bytes are already zero.
    uint8_t padBuffer[64] = { 0x80 };
    const size_t padLength = (bufferLength_ < 56) ? (56 - bufferLength_)
                                                  : (120 - bufferLength_);
    update(padBuffer, padLength);
    update(lengthBytes, 8);

    for (int i = 0; i < 4; ++i) {
        digest[i * 4 + 0] = static_cast<uint8_t>(state_[i] & 0xff);
        digest[i * 4 + 1] = static_cast<uint8_t>((state_[i] >> 8) & 0xff);
        digest[i * 4 + 2] = static_cast<uint8_t>((state_[i] >> 16) & 0xff);
        digest[i * 4 + 3] = static_cast<uint8_t>((state_[i] >> 24) & 0xff);
    }
}

void md5Buffer(const uint8_t* data, size_t length, uint8_t digest[16]) {
    Md5 md5;
    md5.update(data, length);
    md5.finish(digest);
}

bool md5SelfTest() {
    // RFC 1321 test suite, first three vectors.
    struct Vector { const char* text; uint8_t digest[16]; };
    const Vector vectors[] = {
        { "",
          { 0xd4,0x1d,0x8c,0xd9,0x8f,0x00,0xb2,0x04,0xe9,0x80,0x09,0x98,0xec,0xf8,0x42,0x7e } },
        { "abc",
          { 0x90,0x01,0x50,0x98,0x3c,0xd2,0x4f,0xb0,0xd6,0x96,0x3f,0x7d,0x28,0xe1,0x7f,0x72 } },
        { "message digest",
          { 0xf9,0x6b,0x69,0x7d,0x7c,0xb7,0x93,0x8d,0x52,0x5a,0x2f,0x31,0xaa,0xf1,0x61,0xd0 } },
    };

    for (const Vector& vector : vectors) {
        uint8_t digest[16];
        md5Buffer(reinterpret_cast<const uint8_t*>(vector.text), strlen(vector.text), digest);
        if (memcmp(digest, vector.digest, 16) != 0) return false;
    }
    return true;
}

// ============================================================================
// base64
// ============================================================================
bool base64Decode(const uint8_t* data, size_t length, std::vector<uint8_t>& out) {
    out.clear();
    out.reserve(length / 4 * 3);

    int accumulator = 0;
    int bitCount = 0;
    for (size_t i = 0; i < length; ++i) {
        const uint8_t c = data[i];
        int value;
        if (c >= 'A' && c <= 'Z')      value = c - 'A';
        else if (c >= 'a' && c <= 'z') value = c - 'a' + 26;
        else if (c >= '0' && c <= '9') value = c - '0' + 52;
        else if (c == '+')             value = 62;
        else if (c == '/')             value = 63;
        else continue;                 // ignore '=', whitespace and stray bytes

        accumulator = (accumulator << 6) | value;
        bitCount += 6;
        if (bitCount >= 8) {
            bitCount -= 8;
            out.push_back(static_cast<uint8_t>((accumulator >> bitCount) & 0xff));
        }
    }
    return true;
}

} // namespace ac
