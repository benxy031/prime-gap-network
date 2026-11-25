// Copyright (c) 2026 The PGN developers. MIT.
// What an audit checks once the counts are in the header.
//
// Today an audit needs three things that exist only to serve it: a report root to
// commit the counts, a reveal to disclose them, and a stored expected vector to
// compare against. With the counts in the gold header all three disappear, and
// what is left is a direct check:
//
//     the five partitions are derived from sha256(counts) and a seed block
//     the auditor's five counts must equal the header's counts at those indices
//
// The seed is the block AFTER the gold block. That ordering is the whole reason
// the check is sound: the miner chooses the gold block's contents and could grind
// the counts against a seed it already knew, but it cannot know the next block's
// hash when it mines. Nothing here is wired into consensus yet - the report path
// is still what the chain runs - so this is the verifier phase 2 will call.
#pragma once

#include <pgn/gold_counts_envelope.h>
#include <pgn/gold_partition_counts.h>
#include <chain.h>
#include <map>
#include <optional>
#include <string_view>

#include <algorithm>
#include <array>
#include <optional>

namespace pgn::core {

inline constexpr size_t GoldAuditPartitionsPerSlot = 5;
inline constexpr size_t GoldAuditSlots = 2;

// The block immediately after the gold block, which fixes the selection without
// letting the miner influence it. Null until that block exists.
inline std::optional<uint256> GoldAuditSeed(const CBlockIndex& tip, int32_t gold_height)
{
    if (gold_height < 0 || gold_height >= tip.nHeight) return {};
    const auto* seed = tip.GetAncestor(gold_height + 1);
    if (!seed || !seed->phashBlock) return {};
    return seed->GetBlockHash();
}

// A range number that is already paid, with everything needed to audit it.
struct GoldAuditTarget {
    GoldCountsRecord record;
    uint256 gold_block;
    int32_t gold_height{};
    uint256 seed;
    std::array<std::array<uint32_t, GoldAuditPartitionsPerSlot>, GoldAuditSlots> partitions;
};

// The seed must exist on this chain, so a range whose gold block is the tip is
// not auditable yet - the miner would otherwise know the selection in advance.
inline std::optional<GoldAuditTarget> ReadGoldAuditTarget(const CBlockIndex& tip,
                                                         const GoldCountsLedger& ledger,
                                                         uint64_t range_number)
{
    const auto owner = ledger.Owner(range_number);
    if (!owner) return {};
    const auto* index = tip.GetAncestor(owner->first);
    if (!index || !index->phashBlock || index->GetBlockHash() != owner->second) return {};
    if (index->gold_counts.size() != GoldCountsHeaderBytes) return {};
    const auto section = DecodeGoldCountsHeader(index->gold_counts);
    if (!section || section->first != range_number) return {};
    const auto seed = GoldAuditSeed(tip, owner->first);
    if (!seed) return {};
    const GoldCountsRecord record{section->first, section->second};
    return GoldAuditTarget{record, owner->second, owner->first, *seed,
                           GoldAuditPartitions(record.counts, *seed)};
}

// One auditor's answer: the partitions it was assigned and the counts it found.
// Sorted index sets are compared exactly, so a slot cannot be swapped for the
// other slot's partitions.
struct GoldAuditAnswer {
    uint8_t slot{};
    std::array<uint32_t, GoldAuditPartitionsPerSlot> partitions{};
    std::array<uint8_t, GoldAuditPartitionsPerSlot> counts{};
    bool operator==(const GoldAuditAnswer&) const = default;
};

enum class GoldAuditVerdict {
    Verified,
    InvalidSlot,       // slot is not 0 or 1
    WrongPartitions,   // the answer recites partitions that were not assigned
    WrongCount,        // a count differs from the header
};

inline bool GoldAuditAnswerWellFormed(const GoldAuditAnswer& answer)
{
    return answer.slot < GoldAuditSlots &&
           std::is_sorted(answer.partitions.begin(), answer.partitions.end()) &&
           std::all_of(answer.partitions.begin(), answer.partitions.end(),
                       [](uint32_t index) { return index < GoldPartitionCount; });
}

inline GoldAuditVerdict VerifyGoldAuditAnswer(const GoldAuditTarget& target,
                                             const GoldAuditAnswer& answer)
{
    if (!GoldAuditAnswerWellFormed(answer)) {
        return answer.slot < GoldAuditSlots ? GoldAuditVerdict::WrongPartitions
                                           : GoldAuditVerdict::InvalidSlot;
    }
    const auto& assigned = target.partitions[answer.slot];
    if (!std::equal(assigned.begin(), assigned.end(), answer.partitions.begin())) {
        return GoldAuditVerdict::WrongPartitions;
    }
    for (size_t i = 0; i < GoldAuditPartitionsPerSlot; ++i) {
        if (answer.counts[i] != target.record.counts[assigned[i]]) return GoldAuditVerdict::WrongCount;
    }
    return GoldAuditVerdict::Verified;
}

// Both slots cover ten distinct partitions, so a single auditor cannot answer
// twice as two different slots and a pair of auditors cannot collide.
inline bool GoldAuditSlotsDisjoint(const GoldAuditTarget& target)
{
    std::array<uint32_t, 2 * GoldAuditPartitionsPerSlot> all{};
    std::copy(target.partitions[0].begin(), target.partitions[0].end(), all.begin());
    std::copy(target.partitions[1].begin(), target.partitions[1].end(),
              all.begin() + GoldAuditPartitionsPerSlot);
    std::sort(all.begin(), all.end());
    return std::adjacent_find(all.begin(), all.end()) == all.end();
}

// The correct answer for a target, which is what an auditor running the scan
// produces. Kept beside the verifier so a test can prove the two agree without
// duplicating the selection.
inline GoldAuditAnswer GoldAuditExpectedAnswer(const GoldAuditTarget& target, uint8_t slot)
{
    GoldAuditAnswer answer;
    answer.slot = slot;
    const auto& assigned = target.partitions[slot];
    for (size_t i = 0; i < GoldAuditPartitionsPerSlot; ++i) {
        answer.partitions[i] = assigned[i];
        answer.counts[i] = target.record.counts[assigned[i]];
    }
    return answer;
}

// The answer as it travels on chain: one canonical zero-value OP_RETURN. The
// actor is whoever the containing transaction spends from, so it is not encoded
// here - an envelope cannot claim an identity by writing one down.
inline constexpr std::string_view GoldAuditTag{"PGNGOLDAUDIT-1"};
inline constexpr uint8_t GoldAuditVersion = 1;
inline constexpr size_t GoldAuditPayloadBytes =
    1 + GoldRangeNumberBytes + 1 + GoldAuditPartitionsPerSlot + GoldAuditPartitionsPerSlot;

inline std::optional<CScript> BuildGoldAuditEnvelope(const GoldAuditAnswer& answer, uint64_t range_number)
{
    if (range_number >= (uint64_t{1} << GoldRangeNumberBits)) return {};
    if (!GoldAuditAnswerWellFormed(answer)) return {};
    Bytes payload{GoldAuditVersion};
    for (size_t i = 0; i < GoldRangeNumberBytes; ++i) payload.push_back(uint8_t(range_number >> (8 * i)));
    payload.push_back(answer.slot);
    for (const auto partition : answer.partitions) payload.push_back(uint8_t(partition));
    for (const auto count : answer.counts) payload.push_back(count);
    CScript script = CScript{} << OP_RETURN << Bytes{GoldAuditTag.begin(), GoldAuditTag.end()} << payload;
    if (script.size() > GoldCountsMaximumScriptBytes) return {};
    return script;
}

struct GoldAuditEnvelope {
    uint64_t range_number{};
    GoldAuditAnswer answer;
    bool operator==(const GoldAuditEnvelope&) const = default;
};

inline bool IsGoldAuditScript(const CScript& script)
{
    auto cursor = script.begin();
    opcodetype op;
    Bytes tag;
    return script.GetOp(cursor, op) && op == OP_RETURN && script.GetOp(cursor, op, tag) &&
           tag == Bytes{GoldAuditTag.begin(), GoldAuditTag.end()};
}

inline std::optional<GoldAuditEnvelope> DecodeGoldAuditEnvelope(const CScript& script)
{
    if (script.size() > GoldCountsMaximumScriptBytes || !IsGoldAuditScript(script)) return {};
    auto cursor = script.begin();
    opcodetype op;
    Bytes tag, payload;
    if (!script.GetOp(cursor, op) || !script.GetOp(cursor, op, tag) ||
        !script.GetOp(cursor, op, payload) || cursor != script.end()) return {};
    if (payload.size() != GoldAuditPayloadBytes || payload.front() != GoldAuditVersion) return {};
    GoldAuditEnvelope envelope;
    size_t at = 1;
    for (size_t i = 0; i < GoldRangeNumberBytes; ++i) envelope.range_number |= uint64_t{payload[at++]} << (8 * i);
    envelope.answer.slot = payload[at++];
    for (auto& partition : envelope.answer.partitions) partition = payload[at++];
    for (auto& count : envelope.answer.counts) count = payload[at++];
    // Both directions must agree, which rejects every non-canonical encoding and
    // every answer that could not have been assigned.
    if (!GoldAuditAnswerWellFormed(envelope.answer)) return {};
    const auto rebuilt = BuildGoldAuditEnvelope(envelope.answer, envelope.range_number);
    if (!rebuilt || *rebuilt != script) return {};
    return envelope;
}

enum class GoldAuditAdmission {
    Accepted,      // a verified answer took a free slot
    Replay,        // the identical answer at the same height: no new claim
    SlotTaken,     // that slot is already answered; one answer per slot, ever
    NotVerified,   // the answer does not match the header, and does NOT take the slot
    OutOfOrder,    // the ledger only grows forward
};

// Two answers per paid range, one per slot. A wrong answer is refused without
// consuming the slot: otherwise any actor could burn a range by answering badly,
// and the range would become unauditable for everyone.
class GoldAuditLedger
{
public:
    struct Slot {
        CScript actor;
        int32_t height{};
        GoldAuditAnswer answer;
        bool operator==(const Slot&) const = default;
    };

    GoldAuditAdmission Add(int32_t height, const GoldAuditTarget& target, const CScript& actor,
                           const GoldAuditAnswer& answer)
    {
        if (height <= m_height) {
            const auto& entry = m_answers[target.record.range_number];
            const auto& slot = entry[answer.slot];
            if (slot && slot->height == height && slot->actor == actor && slot->answer == answer) {
                return GoldAuditAdmission::Replay;
            }
            return GoldAuditAdmission::OutOfOrder;
        }
        if (answer.slot >= GoldAuditSlots) return GoldAuditAdmission::NotVerified;
        auto& entry = m_answers[target.record.range_number];
        if (entry[answer.slot]) return GoldAuditAdmission::SlotTaken;
        if (VerifyGoldAuditAnswer(target, answer) != GoldAuditVerdict::Verified) {
            return GoldAuditAdmission::NotVerified;
        }
        entry[answer.slot] = Slot{actor, height, answer};
        m_height = height;
        return GoldAuditAdmission::Accepted;
    }

    bool Audited(uint64_t range_number) const
    {
        const auto known = m_answers.find(range_number);
        return known != m_answers.end() && known->second[0] && known->second[1];
    }

    const std::array<std::optional<Slot>, GoldAuditSlots>* Answers(uint64_t range_number) const
    {
        const auto known = m_answers.find(range_number);
        if (known == m_answers.end()) return nullptr;
        return &known->second;
    }

    size_t AuditedRanges() const
    {
        size_t count{};
        for (const auto& [range, slots] : m_answers) {
            if (slots[0] && slots[1]) ++count;
        }
        return count;
    }

    int32_t Height() const { return m_height; }

private:
    std::map<uint64_t, std::array<std::optional<Slot>, GoldAuditSlots>> m_answers;
    int32_t m_height{-1};
};

} // namespace pgn::core
