// flac_encoder.cpp - streaming FLAC encoder for 16-bit PCM.
//
// The original application delegated FLAC writing to libsndfile. This is a
// self-contained encoder built on the FLAC format specification:
//
//   * STREAMINFO with a real MD5 signature, patched once the stream is closed
//   * optional VORBIS_COMMENT and PICTURE metadata blocks
//   * fixed predictors of order 0..4, partitioned Rice coding, and the four
//     stereo decorrelation modes, each chosen by estimated bit cost
#include "flac_encoder.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "crypto.h"
#include "util.h"

namespace ac {
namespace {

constexpr uint32_t kBlockSize = 4096;
constexpr int kBitsPerSample = 16;
constexpr int kMaxFixedOrder = 4;
constexpr int kMaxPartitionOrder = 6;
constexpr int kRiceParameterBits = 5;      // coding method 1: 5-bit parameters
constexpr uint32_t kRiceEscapeParameter = 31;
constexpr uint32_t kMaxFlacChannels = 8;

// ---------------------------------------------------------------------------
// CRC tables
// ---------------------------------------------------------------------------
struct CrcTables {
    uint8_t crc8[256];
    uint16_t crc16[256];
    CrcTables() {
        for (int i = 0; i < 256; ++i) {
            uint8_t value = static_cast<uint8_t>(i);
            for (int bit = 0; bit < 8; ++bit)
                value = static_cast<uint8_t>((value << 1) ^ ((value & 0x80) ? 0x07 : 0x00));
            crc8[i] = value;
        }
        for (int i = 0; i < 256; ++i) {
            uint16_t value = static_cast<uint16_t>(i << 8);
            for (int bit = 0; bit < 8; ++bit)
                value = static_cast<uint16_t>((value << 1) ^ ((value & 0x8000) ? 0x8005 : 0x0000));
            crc16[i] = value;
        }
    }
};

const CrcTables& crcTables() {
    static const CrcTables tables;
    return tables;
}

uint8_t crc8(const uint8_t* data, size_t size) {
    const CrcTables& tables = crcTables();
    uint8_t crc = 0;
    for (size_t i = 0; i < size; ++i) crc = tables.crc8[crc ^ data[i]];
    return crc;
}

uint16_t crc16(const uint8_t* data, size_t size) {
    const CrcTables& tables = crcTables();
    uint16_t crc = 0;
    for (size_t i = 0; i < size; ++i)
        crc = static_cast<uint16_t>((crc << 8) ^ tables.crc16[((crc >> 8) ^ data[i]) & 0xFF]);
    return crc;
}

// ---------------------------------------------------------------------------
// MSB-first bit writer
// ---------------------------------------------------------------------------
class BitWriter {
public:
    explicit BitWriter(std::vector<uint8_t>& out) : out_(out) {}

    void writeBits(uint64_t value, int bits) {
        if (bits <= 0) return;
        if (bits < 64) value &= (1ull << bits) - 1;
        // Discard bits that were already flushed, otherwise they would leak back
        // into the next byte boundary crossing.
        accumulator_ = (bitCount_ == 0) ? 0 : (accumulator_ & ((1ull << bitCount_) - 1));
        accumulator_ = (accumulator_ << bits) | value;
        bitCount_ += bits;
        while (bitCount_ >= 8) {
            bitCount_ -= 8;
            out_.push_back(static_cast<uint8_t>(accumulator_ >> bitCount_));
        }
    }

    void writeSigned(int64_t value, int bits) {
        writeBits(static_cast<uint64_t>(value), bits);
    }

    // FLAC unary: `value` zero bits followed by a single one bit.
    void writeUnary(uint32_t value) {
        while (value >= 32) {
            writeBits(0, 32);
            value -= 32;
        }
        writeBits(1, static_cast<int>(value) + 1);
    }

    void alignToByte() {
        if (bitCount_ > 0) writeBits(0, 8 - bitCount_);
    }

private:
    std::vector<uint8_t>& out_;
    uint64_t accumulator_ = 0;
    int bitCount_ = 0;
};

// Writes `bits` of `value` MSB-first into a byte array (used for STREAMINFO).
void packBits(uint8_t* out, size_t& bitPosition, uint64_t value, int bits) {
    for (int i = bits - 1; i >= 0; --i) {
        const uint64_t bit = (value >> i) & 1;
        const size_t byteIndex = bitPosition >> 3;
        const int bitIndex = 7 - static_cast<int>(bitPosition & 7);
        if (bit) out[byteIndex] |= static_cast<uint8_t>(1u << bitIndex);
        ++bitPosition;
    }
}

// FLAC's UTF-8 style coding of the frame number.
void writeUtf8Number(BitWriter& writer, uint64_t value) {
    if (value < 0x80) {
        writer.writeBits(value, 8);
        return;
    }
    int bytes = 2;
    while (bytes < 7 && value >= (1ull << (5 * bytes + 1))) ++bytes;

    const int payloadBits = 7 - bytes;
    uint8_t first = static_cast<uint8_t>((0xFFu << (8 - bytes)) & 0xFF);
    if (payloadBits > 0)
        first |= static_cast<uint8_t>((value >> (6 * (bytes - 1))) &
                                      ((1u << payloadBits) - 1));
    writer.writeBits(first, 8);
    for (int i = bytes - 2; i >= 0; --i)
        writer.writeBits(0x80u | ((value >> (6 * i)) & 0x3F), 8);
}

// ---------------------------------------------------------------------------
// Subframe planning
// ---------------------------------------------------------------------------
struct RicePartition {
    uint32_t parameter = 0;
    bool escape = false;
    int escapeBits = 0;
};

struct SubframePlan {
    enum Kind { Constant, Verbatim, Fixed };
    Kind kind = Verbatim;
    int order = 0;
    int partitionOrder = 0;
    std::vector<RicePartition> partitions;
    std::vector<int32_t> residual;
    int32_t constantValue = 0;
    uint64_t bits = 0;
};

inline uint32_t zigzag(int32_t value) {
    return (static_cast<uint32_t>(value) << 1) ^ static_cast<uint32_t>(value >> 31);
}

// Cost of one Rice-coded partition in bits, including its parameter field.
uint64_t ricePartitionCost(const int32_t* values, size_t count, int bits,
                           RicePartition& out) {
    uint64_t sum = 0;
    for (size_t i = 0; i < count; ++i) sum += zigzag(values[i]);

    // Escape (raw) coding is always available as a fallback.
    uint64_t bestCost = static_cast<uint64_t>(count) * bits;
    RicePartition best;
    best.parameter = kRiceEscapeParameter;
    best.escape = true;
    best.escapeBits = bits;

    if (sum == 0) {
        RicePartition zero;
        zero.parameter = 0;
        if (count < bestCost) { best = zero; bestCost = count; }
        out = best;
        return bestCost + kRiceParameterBits;
    }

    // Estimate the optimal parameter from the mean magnitude, then refine.
    uint32_t estimate = 0;
    while (estimate < 30 && (sum >> estimate) >= count) ++estimate;

    const uint32_t first = (estimate > 2) ? estimate - 2 : 0;
    const uint32_t last = (estimate + 2 < 30) ? estimate + 2 : 30;
    for (uint32_t parameter = first; parameter <= last; ++parameter) {
        uint64_t cost = static_cast<uint64_t>(count) * (1 + parameter);
        for (size_t i = 0; i < count; ++i)
            cost += zigzag(values[i]) >> parameter;
        if (cost < bestCost) {
            bestCost = cost;
            best.parameter = parameter;
            best.escape = false;
            best.escapeBits = 0;
        }
    }

    out = best;
    return bestCost + kRiceParameterBits;
}

// Chooses the partition layout for a residual signal and returns its bit cost
// (including the 2-bit method and 4-bit partition order fields).
uint64_t planResidual(const std::vector<int32_t>& residual, uint32_t blockSize,
                      int order, int bits, SubframePlan& plan) {
    const size_t total = residual.size();
    uint64_t bestBits = 0;
    int bestPartitionOrder = -1;
    std::vector<RicePartition> bestPartitions;

    const int maxOrder = std::min(kMaxPartitionOrder, 7);
    for (int partitionOrder = 0; partitionOrder <= maxOrder; ++partitionOrder) {
        const uint32_t partitionCount = 1u << partitionOrder;
        if (blockSize % partitionCount != 0) continue;
        const uint32_t partitionSize = blockSize / partitionCount;
        if (partitionSize <= static_cast<uint32_t>(order)) continue;

        std::vector<RicePartition> partitions;
        partitions.reserve(partitionCount);
        uint64_t bits6 = 0;
        size_t index = 0;
        bool valid = true;

        for (uint32_t p = 0; p < partitionCount; ++p) {
            const size_t count = (p == 0) ? static_cast<size_t>(partitionSize - order)
                                          : static_cast<size_t>(partitionSize);
            if (index + count > total) { valid = false; break; }
            RicePartition partition;
            bits6 += ricePartitionCost(residual.data() + index, count, bits, partition);
            partitions.push_back(partition);
            index += count;
        }
        if (!valid || index != total) continue;

        if (bestPartitionOrder < 0 || bits6 < bestBits) {
            bestBits = bits6;
            bestPartitionOrder = partitionOrder;
            bestPartitions = std::move(partitions);
        }
    }

    if (bestPartitionOrder < 0) {
        // No partition layout was valid; fall back to verbatim coding.
        plan.kind = SubframePlan::Verbatim;
        plan.partitions.clear();
        return 0;
    }

    plan.kind = SubframePlan::Fixed;
    plan.order = order;
    plan.partitionOrder = bestPartitionOrder;
    plan.partitions = std::move(bestPartitions);
    return bestBits + 2 + 4;
}

void computeResidual(const int32_t* samples, uint32_t count, int order,
                     std::vector<int32_t>& residual) {
    const size_t n = (count > static_cast<uint32_t>(order))
                         ? (count - static_cast<uint32_t>(order)) : 0;
    residual.resize(n);
    switch (order) {
        case 0:
            for (size_t i = 0; i < n; ++i) residual[i] = samples[i];
            break;
        case 1:
            for (size_t i = 0; i < n; ++i) residual[i] = samples[i + 1] - samples[i];
            break;
        case 2:
            for (size_t i = 0; i < n; ++i)
                residual[i] = samples[i + 2] - 2 * samples[i + 1] + samples[i];
            break;
        case 3:
            for (size_t i = 0; i < n; ++i)
                residual[i] = samples[i + 3] - 3 * samples[i + 2] + 3 * samples[i + 1] -
                              samples[i];
            break;
        default:
            for (size_t i = 0; i < n; ++i)
                residual[i] = samples[i + 4] - 4 * samples[i + 3] + 6 * samples[i + 2] -
                              4 * samples[i + 1] + samples[i];
            break;
    }
}

// Decides how a single channel will be coded and returns the plan.
SubframePlan planSubframe(const int32_t* samples, uint32_t count, int bits) {
    SubframePlan plan;

    bool allEqual = true;
    for (uint32_t i = 1; i < count; ++i) {
        if (samples[i] != samples[0]) { allEqual = false; break; }
    }
    if (allEqual) {
        plan.kind = SubframePlan::Constant;
        plan.constantValue = samples[0];
        plan.bits = 8 + bits;
        return plan;
    }

    plan.kind = SubframePlan::Verbatim;
    plan.bits = 8 + static_cast<uint64_t>(count) * bits;

    // Pass 1: pick the fixed predictor order using unpartitioned Rice coding.
    int bestOrder = -1;
    uint64_t bestOrderBits = 0;
    for (int order = 0; order <= kMaxFixedOrder; ++order) {
        if (count <= static_cast<uint32_t>(order)) continue;
        std::vector<int32_t> residual;
        computeResidual(samples, count, order, residual);

        SubframePlan unpartitioned;
        unpartitioned.partitionOrder = 0;
        uint64_t residualBits = planResidual(residual, count, order, bits, unpartitioned);
        if (unpartitioned.kind != SubframePlan::Fixed) continue;

        const uint64_t total = 8 + static_cast<uint64_t>(order) * bits + residualBits;
        if (bestOrder < 0 || total < bestOrderBits) {
            bestOrder = order;
            bestOrderBits = total;
        }
    }

    if (bestOrder < 0) return plan;

    // Pass 2: search partition orders for the winning predictor order.
    std::vector<int32_t> residual;
    computeResidual(samples, count, bestOrder, residual);

    SubframePlan best;
    const uint64_t residualBits = planResidual(residual, count, bestOrder, bits, best);
    if (best.kind != SubframePlan::Fixed) return plan;
    best.residual = std::move(residual);
    best.bits = 8 + static_cast<uint64_t>(bestOrder) * bits + residualBits;

    if (best.bits < plan.bits) return best;
    return plan;
}

void writeSubframe(BitWriter& writer, const int32_t* samples, uint32_t count, int bits,
                   const SubframePlan& plan) {
    writer.writeBits(0, 1);                       // mandatory zero bit
    switch (plan.kind) {
        case SubframePlan::Constant:
            writer.writeBits(0, 6);               // 000000
            break;
        case SubframePlan::Verbatim:
            writer.writeBits(1, 6);               // 000001
            break;
        case SubframePlan::Fixed:
            writer.writeBits(8 | plan.order, 6);  // 001xxx
            break;
    }
    writer.writeBits(0, 1);                       // no wasted bits

    switch (plan.kind) {
        case SubframePlan::Constant:
            writer.writeSigned(plan.constantValue, bits);
            return;
        case SubframePlan::Verbatim:
            for (uint32_t i = 0; i < count; ++i) writer.writeSigned(samples[i], bits);
            return;
        case SubframePlan::Fixed:
            break;
    }

    for (int i = 0; i < plan.order; ++i) writer.writeSigned(samples[i], bits);

    writer.writeBits(1, 2);                       // residual coding method 1
    writer.writeBits(static_cast<uint64_t>(plan.partitionOrder), 4);

    const uint32_t partitionCount = 1u << plan.partitionOrder;
    const uint32_t partitionSize = count / partitionCount;
    size_t index = 0;
    for (uint32_t p = 0; p < partitionCount; ++p) {
        const size_t partitionSamples =
            (p == 0) ? static_cast<size_t>(partitionSize - plan.order)
                     : static_cast<size_t>(partitionSize);
        const RicePartition& partition = plan.partitions[p];
        writer.writeBits(partition.parameter, kRiceParameterBits);

        if (partition.escape) {
            writer.writeBits(static_cast<uint64_t>(partition.escapeBits), 5);
            for (size_t i = 0; i < partitionSamples; ++i)
                writer.writeSigned(plan.residual[index + i], partition.escapeBits);
        } else {
            for (size_t i = 0; i < partitionSamples; ++i) {
                const uint32_t value = zigzag(plan.residual[index + i]);
                writer.writeUnary(value >> partition.parameter);
                if (partition.parameter > 0)
                    writer.writeBits(value & ((1u << partition.parameter) - 1),
                                     static_cast<int>(partition.parameter));
            }
        }
        index += partitionSamples;
    }
}

// ---------------------------------------------------------------------------
// Metadata blocks
// ---------------------------------------------------------------------------
void appendBigEndian(std::vector<uint8_t>& out, uint64_t value, int bytes) {
    for (int i = bytes - 1; i >= 0; --i)
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFF));
}

void appendLittleEndian32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((value >> 24) & 0xFF));
}

struct MetadataBlock {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
};

void writeMetadataBlocks(FILE* file, std::vector<MetadataBlock>& blocks) {
    for (size_t i = 0; i < blocks.size(); ++i) {
        const bool last = (i + 1 == blocks.size());
        const uint32_t length = static_cast<uint32_t>(
            std::min<size_t>(blocks[i].payload.size(), 0xFFFFFF));
        const uint8_t header[4] = {
            static_cast<uint8_t>((last ? 0x80 : 0x00) | (blocks[i].type & 0x7F)),
            static_cast<uint8_t>((length >> 16) & 0xFF),
            static_cast<uint8_t>((length >> 8) & 0xFF),
            static_cast<uint8_t>(length & 0xFF)
        };
        fwrite(header, 1, 4, file);
        if (length > 0) fwrite(blocks[i].payload.data(), 1, length, file);
    }
}

MetadataBlock makeVorbisComment(const AudioTags& tags) {
    MetadataBlock block;
    block.type = 4;
    const std::string vendor = "AudioConverter (C++)";

    appendLittleEndian32(block.payload, static_cast<uint32_t>(vendor.size()));
    block.payload.insert(block.payload.end(), vendor.begin(), vendor.end());
    appendLittleEndian32(block.payload, static_cast<uint32_t>(tags.fields.size()));
    for (const auto& field : tags.fields) {
        const std::string entry = field.first + "=" + field.second;
        appendLittleEndian32(block.payload, static_cast<uint32_t>(entry.size()));
        block.payload.insert(block.payload.end(), entry.begin(), entry.end());
    }
    return block;
}

MetadataBlock makePictureBlock(const Picture& picture) {
    MetadataBlock block;
    block.type = 6;
    appendBigEndian(block.payload, picture.type, 4);
    appendBigEndian(block.payload, picture.mime.size(), 4);
    block.payload.insert(block.payload.end(), picture.mime.begin(), picture.mime.end());
    appendBigEndian(block.payload, picture.description.size(), 4);
    block.payload.insert(block.payload.end(), picture.description.begin(),
                         picture.description.end());
    appendBigEndian(block.payload, picture.width, 4);
    appendBigEndian(block.payload, picture.height, 4);
    appendBigEndian(block.payload, picture.depth, 4);
    appendBigEndian(block.payload, picture.colors, 4);
    appendBigEndian(block.payload, picture.data.size(), 4);
    block.payload.insert(block.payload.end(), picture.data.begin(), picture.data.end());
    return block;
}

// ---------------------------------------------------------------------------
// The writer
// ---------------------------------------------------------------------------
class FlacWriter final : public AudioWriter {
public:
    ~FlacWriter() override {
        if (file_) { fclose(file_); file_ = nullptr; }
    }

    bool open(const std::wstring& path, uint32_t channels, uint32_t sampleRate,
              const AudioTags& tags, std::wstring* error) override {
        if (channels == 0 || channels > kMaxFlacChannels) {
            if (error) *error = L"FLAC 不支持该声道数";
            return false;
        }
        if (sampleRate == 0) {
            if (error) *error = L"采样率无效";
            return false;
        }
        if (!ensureParentDirectory(path)) {
            if (error) *error = L"无法创建输出目录";
            return false;
        }

        file_ = _wfopen(path.c_str(), L"wb");
        if (!file_) {
            if (error) *error = L"写入文件失败";
            return false;
        }

        channels_ = channels;
        sampleRate_ = sampleRate;
        frameIndex_ = 0;
        totalFrames_ = 0;
        minBlockSize_ = 0xFFFF;
        maxBlockSize_ = 0;
        minFrameSize_ = 0xFFFFFF;
        maxFrameSize_ = 0;
        pending_.clear();
        md5_.reset();

        const uint8_t marker[4] = { 'f', 'L', 'a', 'C' };
        if (fwrite(marker, 1, 4, file_) != 4) {
            if (error) *error = L"写入文件失败";
            return false;
        }

        // STREAMINFO placeholder; real values are patched in close().
        std::vector<MetadataBlock> blocks;
        MetadataBlock streamInfo;
        streamInfo.type = 0;
        streamInfo.payload.assign(34, 0);
        blocks.push_back(std::move(streamInfo));

        if (!tags.fields.empty()) blocks.push_back(makeVorbisComment(tags));
        for (const Picture& picture : tags.pictures) {
            if (picture.data.empty()) continue;
            if (picture.data.size() > 0xFFFFFF - 64) continue; // does not fit a block
            blocks.push_back(makePictureBlock(picture));
        }

        streamInfoOffset_ = 4; // block header (4 bytes) + payload starts at 8
        writeMetadataBlocks(file_, blocks);
        if (ferror(file_)) {
            if (error) *error = L"写入文件失败";
            return false;
        }
        return true;
    }

    bool write(const int16_t* samples, uint32_t frames, std::wstring* error) override {
        if (frames == 0) return true;

        md5_.update(reinterpret_cast<const uint8_t*>(samples),
                    static_cast<size_t>(frames) * channels_ * sizeof(int16_t));
        totalFrames_ += frames;

        const size_t scratchBase = pending_.size();
        pending_.resize(scratchBase + static_cast<size_t>(frames) * channels_);
        memcpy(pending_.data() + scratchBase, samples,
               static_cast<size_t>(frames) * channels_ * sizeof(int16_t));

        size_t consumed = 0;
        const size_t framesAvailable = pending_.size() / channels_;
        while (framesAvailable - consumed >= kBlockSize) {
            if (!encodeBlock(pending_.data() + consumed * channels_, kBlockSize, error))
                return false;
            consumed += kBlockSize;
        }
        if (consumed > 0)
            pending_.erase(pending_.begin(),
                           pending_.begin() + static_cast<ptrdiff_t>(consumed * channels_));
        return true;
    }

    bool close(std::wstring* error) override {
        if (!file_) return true;

        const size_t remainingFrames = pending_.size() / channels_;
        if (remainingFrames > 0) {
            if (!encodeBlock(pending_.data(), static_cast<uint32_t>(remainingFrames), error))
                return false;
            pending_.clear();
        }

        bool ok = patchStreamInfo(error);
        if (fclose(file_) != 0 && ok) {
            if (error) *error = L"写入文件失败";
            ok = false;
        }
        file_ = nullptr;
        return ok;
    }

private:
    bool encodeBlock(const int16_t* interleaved, uint32_t frames, std::wstring* error) {
        // De-interleave into per-channel sample planes.
        std::vector<std::vector<int32_t>> planes(channels_);
        for (uint32_t c = 0; c < channels_; ++c) planes[c].resize(frames);
        for (uint32_t i = 0; i < frames; ++i) {
            for (uint32_t c = 0; c < channels_; ++c)
                planes[c][i] = interleaved[static_cast<size_t>(i) * channels_ + c];
        }

        int channelAssignment = static_cast<int>(channels_ - 1);
        std::vector<std::vector<int32_t>> coded;
        std::vector<SubframePlan> plans;

        if (channels_ == 2) {
            // Evaluate the four stereo decorrelation modes.
            std::vector<int32_t> side(frames);
            std::vector<int32_t> mid(frames);
            for (uint32_t i = 0; i < frames; ++i) {
                const int32_t left = planes[0][i];
                const int32_t right = planes[1][i];
                side[i] = left - right;
                mid[i] = (left + right) >> 1;
            }

            const SubframePlan planLeft = planSubframe(planes[0].data(), frames, kBitsPerSample);
            const SubframePlan planRight = planSubframe(planes[1].data(), frames, kBitsPerSample);
            const SubframePlan planSide = planSubframe(side.data(), frames, kBitsPerSample + 1);
            const SubframePlan planMid = planSubframe(mid.data(), frames, kBitsPerSample);

            struct Candidate {
                int assignment;
                uint64_t bits;
            };
            // Independent stereo uses the plain "channels - 1" assignment code,
            // not 0 (which means mono).
            const Candidate candidates[4] = {
                { static_cast<int>(channels_) - 1, planLeft.bits + planRight.bits },
                { 8, planLeft.bits + planSide.bits },   // left / side
                { 9, planSide.bits + planRight.bits },  // side / right
                { 10, planMid.bits + planSide.bits },   // mid / side
            };
            int best = 0;
            for (int i = 1; i < 4; ++i) {
                if (candidates[i].bits < candidates[best].bits) best = i;
            }
            channelAssignment = candidates[best].assignment;

            switch (channelAssignment) {
                case 8:
                    coded = { planes[0], side };
                    plans = { planLeft, planSide };
                    break;
                case 9:
                    coded = { side, planes[1] };
                    plans = { planSide, planRight };
                    break;
                case 10:
                    coded = { mid, side };
                    plans = { planMid, planSide };
                    break;
                default:
                    coded = { planes[0], planes[1] };
                    plans = { planLeft, planRight };
                    break;
            }
        } else {
            for (uint32_t c = 0; c < channels_; ++c) {
                coded.push_back(planes[c]);
                plans.push_back(planSubframe(planes[c].data(), frames, kBitsPerSample));
            }
        }

        // ---- frame header ------------------------------------------------
        std::vector<uint8_t> header;
        {
            BitWriter writer(header);
            writer.writeBits(0x3FFE, 14);          // sync code
            writer.writeBits(0, 1);                // reserved
            writer.writeBits(0, 1);                // fixed block size stream
            writer.writeBits(0x7, 4);              // block size: 16-bit value follows
            writer.writeBits(0, 4);                // sample rate: from STREAMINFO
            writer.writeBits(static_cast<uint64_t>(channelAssignment), 4);
            writer.writeBits(0x4, 3);              // 16 bits per sample
            writer.writeBits(0, 1);                // reserved
            writeUtf8Number(writer, frameIndex_);
            writer.writeBits(frames - 1, 16);      // block size - 1
            writer.alignToByte();
        }
        header.push_back(crc8(header.data(), header.size()));

        // ---- subframes + CRC --------------------------------------------
        std::vector<uint8_t> frame = header;
        {
            BitWriter writer(frame);
            for (size_t c = 0; c < coded.size(); ++c) {
                // The difference channel of a decorrelated stereo pair carries one
                // extra bit.
                const bool isSideChannel =
                    (channelAssignment == 8 && c == 1) ||
                    (channelAssignment == 9 && c == 0) ||
                    (channelAssignment == 10 && c == 1);
                const int bits = isSideChannel ? kBitsPerSample + 1 : kBitsPerSample;
                writeSubframe(writer, coded[c].data(), frames, bits, plans[c]);
            }
            writer.alignToByte();
        }

        const uint16_t footer = crc16(frame.data(), frame.size());
        frame.push_back(static_cast<uint8_t>((footer >> 8) & 0xFF));
        frame.push_back(static_cast<uint8_t>(footer & 0xFF));

        if (fwrite(frame.data(), 1, frame.size(), file_) != frame.size()) {
            if (error) *error = L"写入文件失败";
            return false;
        }

        const uint32_t frameSize = static_cast<uint32_t>(frame.size());
        if (frameSize < minFrameSize_) minFrameSize_ = frameSize;
        if (frameSize > maxFrameSize_) maxFrameSize_ = frameSize;
        if (frames < minBlockSize_) minBlockSize_ = static_cast<uint16_t>(frames);
        if (frames > maxBlockSize_) maxBlockSize_ = static_cast<uint16_t>(frames);
        ++frameIndex_;
        return true;
    }

    bool patchStreamInfo(std::wstring* error) {
        uint8_t payload[34] = {};
        size_t bitPosition = 0;
        packBits(payload, bitPosition, minBlockSize_, 16);
        packBits(payload, bitPosition, maxBlockSize_, 16);
        const uint32_t minFrame = (totalFrames_ == 0) ? 0 : std::min<uint32_t>(minFrameSize_, 0xFFFFFF);
        const uint32_t maxFrame = (totalFrames_ == 0) ? 0 : std::min<uint32_t>(maxFrameSize_, 0xFFFFFF);
        packBits(payload, bitPosition, minFrame, 24);
        packBits(payload, bitPosition, maxFrame, 24);
        packBits(payload, bitPosition, sampleRate_, 20);
        packBits(payload, bitPosition, channels_ - 1, 3);
        packBits(payload, bitPosition, kBitsPerSample - 1, 5);
        packBits(payload, bitPosition, totalFrames_, 36);

        uint8_t digest[16];
        md5_.finish(digest);
        memcpy(payload + 18, digest, 16);

        if (_fseeki64(file_, streamInfoOffset_ + 4, SEEK_SET) != 0) {
            if (error) *error = L"写入文件失败";
            return false;
        }
        if (fwrite(payload, 1, sizeof(payload), file_) != sizeof(payload)) {
            if (error) *error = L"写入文件失败";
            return false;
        }
        _fseeki64(file_, 0, SEEK_END);
        if (ferror(file_)) {
            if (error) *error = L"写入文件失败";
            return false;
        }
        return true;
    }

    FILE* file_ = nullptr;
    uint32_t channels_ = 0;
    uint32_t sampleRate_ = 0;
    uint64_t frameIndex_ = 0;
    uint64_t totalFrames_ = 0;
    uint16_t minBlockSize_ = 0;
    uint16_t maxBlockSize_ = 0;
    uint32_t minFrameSize_ = 0;
    uint32_t maxFrameSize_ = 0;
    long long streamInfoOffset_ = 0;
    std::vector<int16_t> pending_;
    Md5 md5_;
};

} // namespace

std::unique_ptr<AudioWriter> createFlacWriter() {
    return std::make_unique<FlacWriter>();
}

} // namespace ac
