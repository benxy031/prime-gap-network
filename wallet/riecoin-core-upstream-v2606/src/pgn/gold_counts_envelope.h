// Copyright (c) 2026 The PGN developers. MIT.
// The gold block's coverage declaration, and the rule that a range is paid once.
//
// Phase 1 wants the 100 partition counts and the range number to travel with the
// block instead of a 1,182-record gap list. The header placement is a
// serialization change this tree cannot make behind a chain gate yet, so the
// same 106 bytes are carried in a canonical coinbase envelope first. The payload
// is deliberately byte-identical to GoldCountsHeader, so moving the section into
// the header later changes where the bytes live and nothing else.
//
// Nothing here is consensus authority on its own: GoldCountsLedger only records
// which range numbers an ordered, authenticated block sequence has already
// claimed.
#pragma once

#include <pgn/gold_partition_counts.h>
#include <primitives/block.h>
#include <script/script.h>
#include <uint256.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace pgn::core {

inline constexpr std::string_view GoldCountsTag{"PGNGOLD-COUNTS-1"};
inline constexpr uint8_t GoldCountsVersion = 1;

// version, then the same six bytes of range number and 100 counts the header
// section carries.
inline constexpr size_t GoldCountsPayloadBytes = 1 + GoldCountsHeaderBytes;

// A zero-value OP_RETURN envelope. Canonical: round-tripping the decoded record
// must reproduce the exact script bytes.
inline constexpr uint64_t GoldCountsMaximumScriptBytes = 4096;

struct GoldCountsRecord {
    uint64_t range_number{};
    GoldPartitionCounts counts{};
    bool operator==(const GoldCountsRecord&) const = default;
};

inline bool IsGoldCountsScript(const CScript& script)
{
    auto cursor = script.begin();
    opcodetype op;
    Bytes tag;
    return script.GetOp(cursor, op) && op == OP_RETURN && script.GetOp(cursor, op, tag) &&
           tag == Bytes{GoldCountsTag.begin(), GoldCountsTag.end()};
}

inline std::optional<CScript> BuildGoldCountsEnvelope(uint64_t range_number,
                                                      const GoldPartitionCounts& counts)
{
    const auto section = EncodeGoldCountsHeader(range_number, counts);
    if (!section) return {};
    Bytes payload{GoldCountsVersion};
    payload.insert(payload.end(), section->begin(), section->end());
    CScript script = CScript{} << OP_RETURN << Bytes{GoldCountsTag.begin(), GoldCountsTag.end()} << payload;
    if (script.size() > GoldCountsMaximumScriptBytes) return {};
    return script;
}

inline std::optional<GoldCountsRecord> DecodeGoldCountsEnvelope(const CScript& script,
                                                               uint64_t maximum_script_bytes)
{
    if (script.size() > maximum_script_bytes || !IsGoldCountsScript(script)) return {};
    auto cursor = script.begin();
    opcodetype op;
    Bytes tag, payload;
    if (!script.GetOp(cursor, op) || !script.GetOp(cursor, op, tag) ||
        !script.GetOp(cursor, op, payload) || cursor != script.end()) return {};
    // Re-pushing must reproduce the script byte for byte, which rejects every
    // non-minimal push encoding.
    if (script != (CScript{} << OP_RETURN << tag << payload)) return {};
    if (payload.size() != GoldCountsPayloadBytes || payload.front() != GoldCountsVersion) return {};
    const auto section = DecodeGoldCountsHeader(
        std::span<const uint8_t>{payload}.subspan(1));
    if (!section) return {};
    const GoldCountsRecord record{section->first, section->second};
    // Canonicality again over the whole envelope, so no two encodings decode to
    // the same record.
    const auto rebuilt = BuildGoldCountsEnvelope(record.range_number, record.counts);
    if (!rebuilt || *rebuilt != script) return {};
    return record;
}

// Counts above a partition's capacity cannot come from a real scan, so an
// envelope claiming them is invalid rather than merely implausible.
inline std::optional<std::pair<Integer, Integer>> GoldCountsRange(
    const GoldCountsRecord& record, const Integer& origin, const Integer& width)
{
    const auto start = GoldRangeStart(origin, width, record.range_number);
    if (!start) return {};
    return std::pair{*start, width};
}

inline bool GoldCountsRecordBounded(const GoldCountsRecord& record, const Integer& origin,
                                    const Integer& width)
{
    const auto range = GoldCountsRange(record, origin, width);
    return range && GoldCountsBounded(record.counts, range->first, range->second);
}

enum class GoldCountsBlockVerdict {
    Absent,       // no gold envelope: a coordination, bootstrap or report block
    Present,      // exactly one canonical envelope
    Duplicate,    // more than one gold envelope in one block
    Invalid,      // a gold envelope that does not decode
};

struct GoldCountsBlockRead {
    GoldCountsBlockVerdict verdict;
    std::optional<GoldCountsRecord> record;
};

// The declaration lives in the coinbase, and the coinbase is the only output set
// a miner authors without spending anything. Any other location would need a
// spend, which is what the redesign removes. History replay only has the
// coinbase transaction, so that is the primitive.
inline GoldCountsBlockRead ReadGoldCountsFromCoinbase(const CTransaction& coinbase,
                                                      uint64_t maximum_script_bytes)
{
    unsigned envelopes{};
    GoldCountsBlockRead read{GoldCountsBlockVerdict::Absent, {}};
    for (const auto& output : coinbase.vout) {
        if (!IsGoldCountsScript(output.scriptPubKey)) continue;
        // A gold declaration is a commitment, never a payment.
        if (output.nValue != 0) return {GoldCountsBlockVerdict::Invalid, {}};
        ++envelopes;
        const auto record = DecodeGoldCountsEnvelope(output.scriptPubKey, maximum_script_bytes);
        if (!record) return {GoldCountsBlockVerdict::Invalid, {}};
        if (envelopes > 1) return {GoldCountsBlockVerdict::Duplicate, {}};
        read = {GoldCountsBlockVerdict::Present, *record};
    }
    return read;
}

inline GoldCountsBlockRead ReadGoldCountsFromBlock(const CBlock& block, uint64_t maximum_script_bytes)
{
    if (block.vtx.empty() || !block.vtx.front() || !block.vtx.front()->IsCoinBase()) {
        return {GoldCountsBlockVerdict::Absent, {}};
    }
    return ReadGoldCountsFromCoinbase(*block.vtx.front(), maximum_script_bytes);
}

enum class GoldCountsVerdict {
    Accepted,             // a new range number, or an exact replay of an accepted one
    DuplicateRangeNumber, // this number is already paid to another block
    OutOfOrder,           // the ledger only grows forward
};

struct GoldCountsAdmission {
    GoldCountsVerdict verdict;
    int32_t height{};
    uint256 block;
};

// Pay-once, in one line: a range number may appear in at most one accepted
// block, ever. The ledger therefore never forgets. An orphaned claim keeps its
// number spent: reassignment is exactly the protocol this design deletes, so a
// reorg cannot hand the same range to a second miner.
//
// Re-adding the identical (height, block, record) is Accepted, so replaying the
// same authenticated chain is idempotent and a restart is not a new claim.
class GoldCountsLedger
{
public:
    GoldCountsAdmission Add(int32_t height, const uint256& block, const GoldCountsRecord& record)
    {
        if (height <= m_height) {
            const auto known = m_owner.find(record.range_number);
            if (known != m_owner.end() && known->second.first == height && known->second.second == block) {
                return {GoldCountsVerdict::Accepted, height, block};
            }
            return {GoldCountsVerdict::OutOfOrder, height, block};
        }
        const auto known = m_owner.find(record.range_number);
        if (known != m_owner.end()) {
            return {GoldCountsVerdict::DuplicateRangeNumber, height, block};
        }
        m_owner.emplace(record.range_number, std::pair{height, block});
        m_height = height;
        return {GoldCountsVerdict::Accepted, height, block};
    }

    bool Contains(uint64_t range_number) const { return m_owner.contains(range_number); }

    std::optional<std::pair<int32_t, uint256>> Owner(uint64_t range_number) const
    {
        const auto known = m_owner.find(range_number);
        if (known == m_owner.end()) return {};
        return known->second;
    }

    size_t size() const { return m_owner.size(); }
    int32_t Height() const { return m_height; }

private:
    std::map<uint64_t, std::pair<int32_t, uint256>> m_owner;
    int32_t m_height{-1};
};

} // namespace pgn::core
