// Copyright (c) 2026 The PGN developers. MIT.
// Pay-once as a rule over authenticated history, not only as a ledger API.
//
// A proposed block that declares a gold range number is checked against every
// declaration already on the current branch. The single line the redesign rests
// on is "a range number may appear in at most one accepted block, ever", and it
// is worthless if it is only advisory: this is where a duplicate is refused.
//
// The claimed set is rebuilt from the same authenticated archive the report and
// audit replay use, so nothing here invents history. It is a prefix cache, not a
// new store: extension walks forward from the cached tip, and any change in the
// branch or the chain discards it.
#pragma once

#include <pgn/chain_range_policy.h>
#include <pgn/gold_counts_activation.h>
#include <pgn/gold_counts_envelope.h>
#include <pgn/candidate_range_schedule.h>
#include <chain.h>
#include <coins.h>
#include <hash.h>
#include <kernel/cs_main.h>

#include <memory>
#include <optional>

namespace pgn::core {

// Activation read from the chain's own genesis coinbase. Absence is
// MissingConfiguration, which means the rule does not apply to this chain; it
// never falls back to a compiled-in default.
struct ChainGoldCountsPolicy {
    uint256 chain, genesis, id;
};

struct ChainGoldCountsPolicyCheck {
    ChainRangePolicyVerdict verdict;
    std::optional<ChainGoldCountsPolicy> policy;
};

inline ChainGoldCountsPolicyCheck ReadChainGoldCountsPolicy(
    const CCoinsView& view, const CBlockIndex& parent, const NativeRequestHistoryArchive& archive,
    RequestHistoryArchiveBudget archive_budget, RangePolicyReadBudget budget)
{
    const auto payload = ReadCandidateGenesisPolicyPayload(view, parent, archive, archive_budget, budget,
                                                          GoldCountsPolicyTag);
    if (!payload.genesis) return {payload.verdict, {}};
    const auto decoded = DecodeGoldCountsPolicyPayload(payload.payload, budget);
    if (!decoded) return {ChainRangePolicyVerdict::InvalidConfiguration, {}};
    HashWriter id{TaggedHash("PGN/GOLD-COUNTS-POLICY/1")};
    id << payload.genesis->Chain() << payload.genesis->Block() << payload.payload;
    return {ChainRangePolicyVerdict::Confirmed,
            ChainGoldCountsPolicy{payload.genesis->Chain(), payload.genesis->Block(), id.GetHash()}};
}

// Every range number claimed on the current branch below `parent`. Null when the
// branch cannot be authenticated inside the budget: a caller must then refuse
// the block, never assume the set is empty.
//
// The declaration lives in the header, and the block index keeps the header, so
// the replay reads the index instead of the block bodies. That is the whole
// reason the section is stored there: no archive load, no body read, and the
// claim is exactly the one the block's own id commits to.
inline std::shared_ptr<const GoldCountsLedger> ReadGoldCountsLedger(
    const CCoinsView& view, const CBlockIndex& parent, const RangeScheduleBudget& budget)
{
    AssertLockHeld(cs_main);
    const auto head = view.GetPgnFactHead();
    if (!head || !head->Valid() || !parent.phashBlock || parent.nHeight < 0 ||
        !parent.IsValid(BLOCK_VALID_SCRIPTS) || head->tip != parent.GetBlockHash() ||
        view.GetBestBlock() != parent.GetBlockHash() || !budget.maximum_blocks) return {};
    struct Snapshot {
        uint256 chain, tip;
        int32_t height{-1};
        std::shared_ptr<GoldCountsLedger> ledger;
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
    const auto reject = [&]() -> std::shared_ptr<const GoldCountsLedger> {
        cached = Snapshot{};
        return {};
    };
    try {
        if (!cached.ledger) cached.ledger = std::make_shared<GoldCountsLedger>();
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
                if (block->HasGoldCounts()) {
                    if (block->gold_counts.size() != GoldCountsHeaderBytes) return reject();
                    const auto section = DecodeGoldCountsHeader(block->gold_counts);
                    if (!section) return reject();
                    // Publish only after the whole prefix is authenticated.
                    if (cached.ledger.use_count() != 1) {
                        cached.ledger = std::make_shared<GoldCountsLedger>(*cached.ledger);
                    }
                    const GoldCountsRecord record{section->first, section->second};
                    if (cached.ledger->Add(block->nHeight, block->GetBlockHash(), record).verdict !=
                        GoldCountsVerdict::Accepted) return reject();
                }
                cached.height = block->nHeight;
                cached.tip = block->GetBlockHash();
            }
        }
        return cached.ledger;
    } catch (...) {
        cached = Snapshot{};
        throw;
    }
}

enum class GoldCountsProposedVerdict {
    Absent,               // no declaration: unaffected, whatever the chain activates
    Inactive,             // declared, but this chain does not commit the rule
    Verified,             // declared, bounded, and the range number is unclaimed
    DuplicateRangeNumber, // the number is already paid to another block
    InvalidDeclaration,   // not canonical, paid, duplicated in one block, or over capacity
    OutOfDomain,          // no usable range policy, so the declaration cannot be checked
    HistoryHeld,          // the branch could not be authenticated within the budget
};

struct GoldCountsProposedCheck {
    GoldCountsProposedVerdict verdict;
    std::optional<GoldCountsRecord> record;
};

// `Inactive` is deliberately permissive: on a chain without the rule the
// envelope is an ordinary zero-value OP_RETURN, which is what the published
// testnet accepts today. Every activated chain is strict.
inline GoldCountsProposedCheck VerifyProposedGoldCounts(
    const CCoinsView& view, const CBlockIndex& parent, const NativeRequestHistoryArchive& archive,
    const CBlock& block, const RangeScheduleBudget& schedule_budget, RangePolicyReadBudget policy_budget)
{
    AssertLockHeld(cs_main);
    const bool declares_header = block.HasGoldCounts();
    bool declares_envelope{};
    if (!block.vtx.empty() && block.vtx.front() && block.vtx.front()->IsCoinBase()) {
        for (const auto& output : block.vtx.front()->vout) {
            declares_envelope |= IsGoldCountsScript(output.scriptPubKey);
        }
    }
    if (!declares_header && !declares_envelope) return {GoldCountsProposedVerdict::Absent, {}};

    const auto activation =
        ReadChainGoldCountsPolicy(view, parent, archive, schedule_budget.archive, policy_budget);
    if (!activation.policy) {
        // Only explicit absence is permissive; an unreadable genesis is not. On
        // such a chain the envelope is an ordinary zero-value OP_RETURN, which is
        // what the published testnet accepts today. Header validation has already
        // refused the version bit itself on this chain.
        return {activation.verdict == ChainRangePolicyVerdict::MissingConfiguration
                    ? GoldCountsProposedVerdict::Inactive
                    : GoldCountsProposedVerdict::OutOfDomain,
                {}};
    }
    // On an activated chain the header is the only declaration: a coinbase
    // envelope has no committed home, so it is not a lesser form of the same
    // claim, it is an unbacked one.
    if (!declares_header) return {GoldCountsProposedVerdict::InvalidDeclaration, {}};
    const auto read = ReadGoldCountsFromBlock(block, GoldCountsMaximumScriptBytes);
    if (read.verdict != GoldCountsBlockVerdict::Present || !read.record) {
        return {GoldCountsProposedVerdict::InvalidDeclaration, {}};
    }
    // The declaration is only meaningful against this chain's own address space.
    const auto ranges = ReadChainRangePolicy(view, parent, archive, schedule_budget.archive, policy_budget);
    if (!ranges.policy ||
        !GoldCountsRecordBounded(*read.record, ranges.policy->Definition().origin,
                                 ranges.policy->Definition().width)) {
        return {GoldCountsProposedVerdict::InvalidDeclaration, read.record};
    }
    const auto ledger = ReadGoldCountsLedger(view, parent, schedule_budget);
    if (!ledger) return {GoldCountsProposedVerdict::HistoryHeld, read.record};
    if (ledger->Contains(read.record->range_number)) {
        return {GoldCountsProposedVerdict::DuplicateRangeNumber, read.record};
    }
    return {GoldCountsProposedVerdict::Verified, read.record};
}

} // namespace pgn::core
