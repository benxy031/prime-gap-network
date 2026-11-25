// Copyright (c) 2026 The PGN developers. MIT.
// Phase 1 gold-block coverage proof, in the header rather than in the payload.
//
// Today a gold block carries 1,182 recorded gaps so that an audit can recount
// the 100 partitions the report claims. The audit only ever compares the five
// partitions it is assigned, so 100 one-byte counts are sufficient evidence and
// 4,198 payload bytes become 100 header bytes. Two consequences follow, and both
// are implemented here:
//
//   * the range is addressed by its number, so no ticket object has to exist;
//   * the audit seed is rooted at sha256(counts) instead of a report root, so
//     the commitment is fixed at inclusion and needs no reveal.
//
// Counts are accumulated while the scan runs (GoldCountsAccumulator), never
// recomputed from a retained gap list afterwards.
#pragma once

#include <pgn/range_commitment.h>

#include <crypto/sha256.h>
#include <uint256.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pgn::core {

// range_origin + range_number * ticket_width, with the origin and width pinned
// by the chain's range policy. 48 bits covers the published testnet domain.
inline constexpr unsigned GoldRangeNumberBits = 48;
inline constexpr size_t GoldRangeNumberBytes = GoldRangeNumberBits / 8;

// One byte per partition. 255 gaps inside 1/100 of a range is far above every
// measured density, and reaching it fails closed rather than wrapping.
inline constexpr size_t GoldPartitionCount = 100;
using GoldPartitionCounts = std::array<uint8_t, GoldPartitionCount>;
inline constexpr size_t GoldCountsHeaderBytes = GoldRangeNumberBytes + GoldPartitionCount;

// Composition on this chain. The native header is 112 bytes (nVersion 4,
// hashPrevBlock 32, hashMerkleRoot 32, nTime 8, nBits 4, nNonce 32). The
// section goes immediately before nNonce, so the counter family stays the last
// ten bytes of the header and tools/pgn_local_blocks.py keeps its offset.
inline constexpr size_t GoldNativeHeaderBytes = 112;
inline constexpr size_t GoldNonceBytes = 32;
inline constexpr size_t GoldCountsSectionOffset = GoldNativeHeaderBytes - GoldNonceBytes;
inline constexpr size_t GoldHeaderBytes = GoldNativeHeaderBytes + GoldCountsHeaderBytes;

// The number is the only range declaration a block needs. Anything that does
// not land exactly on a range boundary has no number.
inline std::optional<uint64_t> GoldRangeNumber(const Integer& origin, const Integer& width, const Integer& start)
{
    if (width <= 0 || start < origin) return {};
    const Integer offset = start - origin;
    if (offset % width != 0) return {};
    const Integer number = offset / width;
    if (number < 0 || number >= (Integer{1} << GoldRangeNumberBits)) return {};
    return number.convert_to<uint64_t>();
}

inline std::optional<Integer> GoldRangeStart(const Integer& origin, const Integer& width, uint64_t number)
{
    if (width <= 0 || number >= (uint64_t{1} << GoldRangeNumberBits)) return {};
    return origin + Integer{number} * width;
}

// Partition i owns [start + length*i/100, start + length*(i+1)/100), which is
// the split the report decoder, the native session index and the audit share.
inline std::optional<size_t> GoldPartitionIndex(const Integer& start, const Integer& length, const Integer& lower)
{
    if (length <= 0 || lower < start || lower >= start + length) return {};
    return (((lower - start + 1) * GoldPartitionCount - 1) / length).convert_to<size_t>();
}

// Odd positions a partition can hold at all; a count above this is impossible.
inline Integer GoldPartitionCapacity(const Integer& start, const Integer& length, size_t index)
{
    const Integer first = start + length * Integer{static_cast<unsigned long long>(index)} / GoldPartitionCount;
    const Integer end = start + length * Integer{static_cast<unsigned long long>(index + 1)} / GoldPartitionCount;
    return end / 2 - first / 2;
}

inline bool GoldCountsBounded(const GoldPartitionCounts& counts, const Integer& start, const Integer& length)
{
    if (length <= 0) return false;
    for (size_t i = 0; i < GoldPartitionCount; ++i) {
        if (Integer{counts[i]} > GoldPartitionCapacity(start, length, i)) return false;
    }
    return true;
}

inline uint64_t GoldCountsTotal(const GoldPartitionCounts& counts)
{
    uint64_t total{};
    for (const auto value : counts) total += value;
    return total;
}

// Single SHA-256 of the 100-byte count vector. Shared with the report-root
// selector so both roots hash through the same primitive.
inline Digest AuditPartitionSha256(std::span<const uint8_t> bytes)
{
    Digest out;
    CSHA256{}.Write(bytes.data(), bytes.size()).Finalize(out.data());
    return out;
}

// The audit root with counts in the header is exactly sha256(counts): 100 input
// bytes cannot be confused with a 32-byte report root, so no domain tag is
// needed to keep the two apart.
inline Digest GoldCountsRoot(const GoldPartitionCounts& counts)
{
    return AuditPartitionSha256(counts);
}

// The digest is read as a big-endian identifier, the order the audit root has
// always had: HexStr(GoldCountsRoot(counts)) == GoldCountsRootId(counts).GetHex().
inline uint256 GoldCountsRootId(const GoldPartitionCounts& counts)
{
    const auto digest = GoldCountsRoot(counts);
    uint256 id;
    std::reverse_copy(digest.begin(), digest.end(), id.begin());
    return id;
}

inline Digest Uint256Digest(const uint256& value)
{
    Digest out;
    std::reverse_copy(value.begin(), value.end(), out.begin());
    return out;
}

// Counter-zero canonical section of the gold header: six little-endian bytes of
// range number followed by the 100 counts.
inline std::optional<std::array<uint8_t, GoldCountsHeaderBytes>> EncodeGoldCountsHeader(
    uint64_t number, const GoldPartitionCounts& counts)
{
    if (number >= (uint64_t{1} << GoldRangeNumberBits)) return {};
    std::array<uint8_t, GoldCountsHeaderBytes> out{};
    for (size_t i = 0; i < GoldRangeNumberBytes; ++i) out[i] = static_cast<uint8_t>(number >> (8 * i));
    std::copy(counts.begin(), counts.end(), out.begin() + GoldRangeNumberBytes);
    return out;
}

inline std::optional<std::pair<uint64_t, GoldPartitionCounts>> DecodeGoldCountsHeader(
    std::span<const uint8_t> bytes)
{
    if (bytes.size() != GoldCountsHeaderBytes) return {};
    uint64_t number{};
    for (size_t i = 0; i < GoldRangeNumberBytes; ++i) number |= uint64_t{bytes[i]} << (8 * i);
    GoldPartitionCounts counts{};
    std::copy(bytes.begin() + GoldRangeNumberBytes, bytes.end(), counts.begin());
    // Six bytes cannot carry more than 48 bits, so this only rejects a
    // non-canonical encoding.
    const auto encoded = EncodeGoldCountsHeader(number, counts);
    if (!encoded || !std::equal(encoded->begin(), encoded->end(), bytes.begin())) return {};
    return std::pair{number, counts};
}

// The section is 106 arbitrary bytes in the primitive header; this is the one
// place that decides whether they mean anything. A range number below 2^48 always
// re-encodes to itself, so only the counts can make the section non-canonical -
// and they cannot, so today the check is structural. It stays explicit so a future
// section version is refused rather than silently reinterpreted.
inline bool CanonicalGoldCountsSection(std::span<const uint8_t, GoldCountsHeaderBytes> bytes)
{
    return DecodeGoldCountsHeader(bytes).has_value();
}

// Scan-time accumulator. A partition that would overflow stops the range; the
// caller discards it rather than publishing a clamped count.
class GoldCountsAccumulator
{
public:
    GoldCountsAccumulator(Integer start, Integer length) : m_start{std::move(start)}, m_length{std::move(length)} {}

    bool Add(const Integer& lower)
    {
        const auto index = GoldPartitionIndex(m_start, m_length, lower);
        if (!index || m_counts[*index] == std::numeric_limits<uint8_t>::max()) {
            m_overflow = true;
            return false;
        }
        ++m_counts[*index];
        return true;
    }

    bool Overflowed() const { return m_overflow; }
    const GoldPartitionCounts& Counts() const { return m_counts; }

private:
    Integer m_start, m_length;
    GoldPartitionCounts m_counts{};
    bool m_overflow{};
};

// The selector both roots share. Kept here so the report-root path and the
// counts path cannot drift: only the 32 root bytes differ.
inline std::array<std::array<uint32_t, 5>, 2> SelectNativeAuditPartitions(const Digest& root, const Digest& seed)
{
    const std::string prefix = "PGN/audit-partitions/1";
    Bytes input{prefix.begin(), prefix.end()};
    input.push_back(0);
    input.insert(input.end(), root.begin(), root.end());
    input.insert(input.end(), seed.begin(), seed.end());
    std::vector<uint32_t> remaining(GoldPartitionCount);
    for (uint32_t i = 0; i < GoldPartitionCount; ++i) remaining[i] = i;
    std::array<std::array<uint32_t, 5>, 2> result;
    uint64_t counter{};
    unsigned selected{};
    const Integer space = Integer{1} << 256;
    while (selected < 10) {
        auto bytes = input;
        for (int i = 7; i >= 0; --i) bytes.push_back(uint8_t(counter >> (8 * i)));
        ++counter;
        const auto raw = AuditPartitionSha256(bytes);
        Integer value;
        import_bits(value, raw.begin(), raw.end(), 8, true);
        const Integer bound = space - space % remaining.size();
        if (value >= bound) continue;
        const auto position = (value % remaining.size()).convert_to<size_t>();
        result[selected / 5][selected % 5] = remaining[position];
        remaining.erase(remaining.begin() + position);
        ++selected;
    }
    for (auto& group : result) std::sort(group.begin(), group.end());
    return result;
}

// Phase 1 entry point: the five partitions each audit slot must recount, fixed
// when the block is included because the counts are already in its header.
inline std::array<std::array<uint32_t, 5>, 2> GoldAuditPartitions(const GoldPartitionCounts& counts,
                                                                  const Digest& seed)
{
    return SelectNativeAuditPartitions(GoldCountsRoot(counts), seed);
}

inline std::array<std::array<uint32_t, 5>, 2> GoldAuditPartitions(const GoldPartitionCounts& counts,
                                                                  const uint256& seed)
{
    return GoldAuditPartitions(counts, Uint256Digest(seed));
}

} // namespace pgn::core
