// crypto.h - self-contained AES, MD5 and base64 used by the NCM decryptor and
// the FLAC encoder. No external crypto dependency is required.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ac {

// ------------------------------------------------------------------------ AES
// AES block cipher, ECB mode, for 128/192/256-bit keys (FIPS-197).
class Aes {
public:
    Aes(const uint8_t* key, size_t keyLength);

    void encryptBlock(const uint8_t input[16], uint8_t output[16]) const;
    void decryptBlock(const uint8_t input[16], uint8_t output[16]) const;

    // ECB helpers that operate on whole blocks. `length` must be a multiple of 16.
    void decryptEcb(uint8_t* data, size_t length) const;
    void encryptEcb(uint8_t* data, size_t length) const;

private:
    void addRoundKey(uint8_t* state, int round) const;

    uint8_t roundKeys_[240];
    int rounds_ = 10;
};

// FIPS-197 known-answer test; returns true when encryption and decryption agree.
bool aesSelfTest();

// ----------------------------------------------------------------------- MD5
// RFC 1321 MD5, used for the FLAC STREAMINFO audio signature.
class Md5 {
public:
    Md5() { reset(); }
    void reset();
    void update(const uint8_t* data, size_t length);
    void finish(uint8_t digest[16]);

private:
    void processBlock(const uint8_t* block);

    uint32_t state_[4];
    uint64_t bitCount_;
    uint8_t buffer_[64];
    size_t bufferLength_;
};

void md5Buffer(const uint8_t* data, size_t length, uint8_t digest[16]);
bool md5SelfTest();

// -------------------------------------------------------------------- base64
// Standard base64 decoder. Characters outside the alphabet are ignored, matching
// Python's base64.b64decode() default behaviour.
bool base64Decode(const uint8_t* data, size_t length, std::vector<uint8_t>& out);

} // namespace ac
