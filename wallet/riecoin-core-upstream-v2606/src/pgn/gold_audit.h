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

} // namespace pgn::core
