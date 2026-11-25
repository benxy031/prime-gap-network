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
#include <pgn/gold_reward.h>
#include <pgn/gold_partition_counts.h>
#include <pgn/candidate_range_schedule.h>
#include <chain.h>
#include <coins.h>
#include <kernel/cs_main.h>
#include <pgn/request_history_archive.h>
#include <memory>
#include <vector>
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

// An audit that may be answered forever is an audit nobody has to answer: a miner
// could declare counts, never be checked, and keep the range. The window makes the
// outcome decisive, which is what the redesign's "late result invalid by expiry"
// means once the counts are in the header. Twelve blocks is the published audit
// lease length (NativeAuditLeaseBlocks), reused rather than invented; phase 4's
// batching cadence replaces it with the batch block.
inline constexpr int32_t GoldAuditWindowBlocks = 12;

// The last height at which an answer is still worth anything. The seed sits at
// gold_height + 1 and the window is counted from it, so the deadline is inclusive.
inline int32_t GoldAuditDeadline(const GoldAuditTarget& target)
{
    return target.gold_height + 1 + GoldAuditWindowBlocks;
}

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
    // Who the auditor wants to be paid. The chain pays scripts, not claims, so the
    // envelope has to carry one; it plays no part in whether the answer is correct.
    CScript reward;
    bool operator==(const GoldAuditAnswer&) const = default;
};

enum class GoldAuditVerdict {
    Verified,
    InvalidSlot,       // slot is not 0 or 1
    WrongPartitions,   // the answer recites partitions that were not assigned
    WrongCount,        // a count differs from the header
};

// The assignment alone: a slot and five partitions that could have been handed
// out. Whether the answer is CORRECT depends only on this, never on who is paid.
inline bool GoldAuditAssignmentWellFormed(const GoldAuditAnswer& answer)
{
    return answer.slot < GoldAuditSlots &&
           std::is_sorted(answer.partitions.begin(), answer.partitions.end()) &&
           std::all_of(answer.partitions.begin(), answer.partitions.end(),
                       [](uint32_t index) { return index < GoldPartitionCount; });
}

// The whole thing as it travels: an assignment plus a payee. Only the codec needs
// this, because only the codec decides what may be written into a block.
inline bool GoldAuditAnswerWellFormed(const GoldAuditAnswer& answer)
{
    return GoldAuditAssignmentWellFormed(answer) && !answer.reward.empty() &&
           answer.reward.size() <= GoldRewardMaximumScriptBytes;
}

inline GoldAuditVerdict VerifyGoldAuditAnswer(const GoldAuditTarget& target,
                                             const GoldAuditAnswer& answer)
{
    if (!GoldAuditAssignmentWellFormed(answer)) {
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
inline GoldAuditAnswer GoldAuditExpectedAnswer(const GoldAuditTarget& target, uint8_t slot,
                                               CScript reward = CScript{} << OP_1)
{
    GoldAuditAnswer answer;
    answer.slot = slot;
    answer.reward = std::move(reward);
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
// Version two appends the auditor's payee. Version one had no payee at all, so an
// answer in that form could never be settled and is not accepted on a chain that
// commits the counts rule.
inline constexpr uint8_t GoldAuditVersion = 2;
inline constexpr size_t GoldAuditPayloadBytes =
    1 + GoldRangeNumberBytes + 1 + GoldAuditPartitionsPerSlot + GoldAuditPartitionsPerSlot;
inline constexpr size_t GoldRewardLengthBytes = 1;

inline std::optional<CScript> BuildGoldAuditEnvelope(const GoldAuditAnswer& answer, uint64_t range_number)
{
    if (range_number >= (uint64_t{1} << GoldRangeNumberBits)) return {};
    if (!GoldAuditAnswerWellFormed(answer)) return {};
    Bytes payload{GoldAuditVersion};
    for (size_t i = 0; i < GoldRangeNumberBytes; ++i) payload.push_back(uint8_t(range_number >> (8 * i)));
    payload.push_back(answer.slot);
    for (const auto partition : answer.partitions) payload.push_back(uint8_t(partition));
    for (const auto count : answer.counts) payload.push_back(count);
    payload.push_back(uint8_t(answer.reward.size()));
    payload.insert(payload.end(), answer.reward.begin(), answer.reward.end());
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
    if (payload.size() < GoldAuditPayloadBytes + GoldRewardLengthBytes ||
        payload.front() != GoldAuditVersion) {
        return {};
    }
    GoldAuditEnvelope envelope;
    size_t at = 1;
    for (size_t i = 0; i < GoldRangeNumberBytes; ++i) envelope.range_number |= uint64_t{payload[at++]} << (8 * i);
    envelope.answer.slot = payload[at++];
    for (auto& partition : envelope.answer.partitions) partition = payload[at++];
    for (auto& count : envelope.answer.counts) count = payload[at++];
    const size_t reward_bytes = payload[at++];
    if (payload.size() != at + reward_bytes) return {};
    envelope.answer.reward = CScript{payload.begin() + at, payload.end()};
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
    // What the chain actually proves is that this block carried this answer.
    // Who gets paid for it is the pay layer's problem (the coinbase payee), not
    // something an envelope can assert about itself.
    struct Slot {
        uint256 block;
        int32_t height{};
        GoldAuditAnswer answer;
        bool operator==(const Slot&) const = default;
    };

    // The verdict without the entry, so an admission path can ask a read-only
    // ledger whether a candidate's answer would be accepted.
    GoldAuditAdmission Check(int32_t height, const uint256& block, const GoldAuditTarget& target,
                             const GoldAuditAnswer& answer) const
    {
        if (height <= m_height) {
            // Replay lookup only; a refused answer must never create an entry.
            const auto known = m_answers.find(target.record.range_number);
            if (known != m_answers.end()) {
                const auto& slot = known->second[answer.slot];
                if (slot && slot->height == height && slot->block == block && slot->answer == answer) {
                    return GoldAuditAdmission::Replay;
                }
            }
            return GoldAuditAdmission::OutOfOrder;
        }
        if (answer.slot >= GoldAuditSlots) return GoldAuditAdmission::NotVerified;
        if (m_answers.contains(target.record.range_number) &&
            m_answers.at(target.record.range_number)[answer.slot]) {
            return GoldAuditAdmission::SlotTaken;
        }
        if (VerifyGoldAuditAnswer(target, answer) != GoldAuditVerdict::Verified) {
            return GoldAuditAdmission::NotVerified;
        }
        return GoldAuditAdmission::Accepted;
    }

    GoldAuditAdmission Add(int32_t height, const uint256& block, const GoldAuditTarget& target,
                           const GoldAuditAnswer& answer)
    {
        const auto verdict = Check(height, block, target, answer);
        // Only a verified answer creates or extends the entry.
        if (verdict != GoldAuditAdmission::Accepted) return verdict;
        m_answers[target.record.range_number][answer.slot] = Slot{block, height, answer};
        m_height = height;
        return verdict;
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

// What a paid range's audit looks like right now. Expired is not a failure - it is
// the final answer for a range nobody audited in time, and a later payment rule has
// to be able to read it as such.
enum class GoldAuditStatus { Open, Audited, Expired };

struct GoldAuditState {
    GoldAuditStatus status{GoldAuditStatus::Open};
    int32_t deadline{};
    uint8_t answered{};
};

inline GoldAuditState GoldAuditRangeState(const GoldAuditLedger& audits, const GoldAuditTarget& target,
                                          int32_t tip_height)
{
    GoldAuditState state;
    state.deadline = GoldAuditDeadline(target);
    if (const auto* answers = audits.Answers(target.record.range_number)) {
        for (const auto& slot : *answers) {
            if (slot) ++state.answered;
        }
    }
    if (state.answered == GoldAuditSlots) {
        state.status = GoldAuditStatus::Audited;
    } else if (tip_height >= state.deadline) {
        // Not ">": an answer needs a block slot at or before the deadline, and once
        // the tip IS the deadline the next block is past it. The range is decided.
        state.status = GoldAuditStatus::Expired;
    }
    return state;
}

// Replay of every counts audit on the current branch, read from the same
// authenticated archive the report and audit replay use. Audits live in coinbase
// outputs, so unlike the pay-once ledger this one needs the block bodies: the
// index carries a header, not a body.
//
// A bad answer does not invalidate the block that carried it - the block already
// paid for its work - it simply does not take a slot, so the range stays
// auditable for everyone else. That is the same rule GoldAuditLedger applies to
// a live submission.
inline std::shared_ptr<const GoldAuditLedger> ReadGoldAuditLedger(
    const CCoinsView& view, const CBlockIndex& parent, const NativeRequestHistoryArchive& history,
    const RangeScheduleBudget& budget, const GoldCountsLedger& claims, uint64_t maximum_script_bytes)
{
    AssertLockHeld(cs_main);
    const auto head = view.GetPgnFactHead();
    if (!head || !head->Valid() || !parent.phashBlock || parent.nHeight < 0 ||
        !parent.IsValid(BLOCK_VALID_SCRIPTS) || head->tip != parent.GetBlockHash() ||
        view.GetBestBlock() != parent.GetBlockHash() || !budget.maximum_blocks) return {};
    struct Snapshot {
        uint256 chain, tip;
        int32_t height{-1};
        std::shared_ptr<GoldAuditLedger> audits;
    };
    static Snapshot cached;
    const auto* anchor = cached.height >= 0 && cached.height <= parent.nHeight
                             ? parent.GetAncestor(cached.height) : nullptr;
    if (cached.chain != head->chain ||
        (cached.height >= 0 && (!anchor || !anchor->phashBlock ||
                                !anchor->IsValid(BLOCK_VALID_SCRIPTS) ||
                                anchor->GetBlockHash() != cached.tip))) {
        cached = Snapshot{};
    }
    cached.chain = head->chain;
    const auto reject = [&]() -> std::shared_ptr<const GoldAuditLedger> {
        cached = Snapshot{};
        return {};
    };
    try {
        if (!cached.audits) cached.audits = std::make_shared<GoldAuditLedger>();
        while (cached.height < parent.nHeight) {
            const int64_t first = int64_t(cached.height) + 1;
            const int64_t last = std::min<int64_t>(parent.nHeight, first + budget.maximum_blocks - 1);
            std::vector<const CBlockIndex*> ancestry(size_t(last - first + 1));
            auto* index = parent.GetAncestor(int32_t(last));
            for (int64_t height = last; height >= first; --height) {
                if (!index || !index->phashBlock || index->nHeight != height ||
                    !index->IsValid(BLOCK_VALID_SCRIPTS)) return reject();
                ancestry[size_t(height - first)] = index;
                index = index->pprev;
            }
            if (first && (!index || !index->phashBlock || index->GetBlockHash() != cached.tip)) return reject();
            for (const auto* block : ancestry) {
                const auto loaded = history.Load(view, parent, block->nHeight, budget.archive);
                if (!loaded.block || !loaded.block->Coinbase()) return reject();
                unsigned envelopes{};
                bool changed{};
                for (const auto& output : loaded.block->Coinbase()->vout) {
                    if (!IsGoldAuditScript(output.scriptPubKey)) continue;
                    // A declaration is never a payment, and one block answers at
                    // most one slot: two answers in one coinbase would let a single
                    // block fill both halves of the audit.
                    if (output.nValue != 0 || ++envelopes > 1) return reject();
                    const auto envelope = DecodeGoldAuditEnvelope(output.scriptPubKey);
                    if (!envelope) return reject();
                    const auto owner = claims.Owner(envelope->range_number);
                    // An answer to a range that was never paid is not an audit.
                    if (!owner) return reject();
                    // The seed is the block after the gold block; answering in that
                    // same block would let the miner answer against a seed it chose.
                    if (block->nHeight <= owner->first + 1) continue;
                    const auto target = ReadGoldAuditTarget(parent, claims, envelope->range_number);
                    if (!target) return reject();
                    // Admission refuses a late answer, so one cannot be on a chain this
                    // replay accepts. Skipping rather than rejecting keeps an already
                    // mined chain readable instead of making it unauthenticatable.
                    if (block->nHeight > GoldAuditDeadline(*target)) continue;
                    const auto admission = cached.audits->Add(block->nHeight, block->GetBlockHash(),
                                                              *target, envelope->answer);
                    if (admission == GoldAuditAdmission::Accepted) changed = true;
                    if (admission == GoldAuditAdmission::OutOfOrder) return reject();
                }
                // Publish only after the whole prefix is authenticated.
                if (changed && cached.audits.use_count() != 1) {
                    cached.audits = std::make_shared<GoldAuditLedger>(*cached.audits);
                }
                cached.height = block->nHeight;
                cached.tip = block->GetBlockHash();
            }
        }
        return cached.audits;
    } catch (...) {
        cached = Snapshot{};
        throw;
    }
}

} // namespace pgn::core
