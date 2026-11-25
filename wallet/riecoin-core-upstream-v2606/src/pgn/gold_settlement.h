// Copyright (c) 2026 The PGN developers. MIT.
// Releasing a counted range's audit money, exactly once and in a fixed order.
//
// The quote in gold_payout.h says who is owed what. This says when the chain may
// hand it out, and it has to answer two questions the quote cannot:
//
//   * which range is next, given that several can complete their audit in the
//     same block or in a different order than they were mined; and
//   * how the chain remembers that a range has already been paid, without keeping
//     an unbounded list of them.
//
// The answer to both is one monotone cursor over COMPLETED audits, ordered by
// (height at which the second slot was answered, range number). That order is a
// function of the chain, so every node derives the same next range, and the state
// only has to remember how many have been released.
//
// A range whose window closed unaudited never enters that order at all: it is not
// deferred, it is finished, and it must not be able to stall the ranges behind it.
#pragma once

#include <pgn/gold_payout.h>

#include <algorithm>
#include <functional>
#include <optional>
#include <vector>

namespace pgn::core {

// One range whose audit completed, in release order.
struct GoldCompletedAudit {
    uint64_t range_number{};
    int32_t completion_height{};
    int32_t gold_height{};
    size_t ordinal{};
    bool operator==(const GoldCompletedAudit&) const = default;
};

// Every fully audited range on the branch, in release order. Complete means both
// slots carry an answer, which is the only thing the ledger ever records.
inline std::vector<GoldCompletedAudit> GoldCompletedAudits(const GoldCountsLedger& claims,
                                                           const GoldAuditLedger& audits)
{
    std::vector<GoldCompletedAudit> complete;
    for (const auto& [range, owner] : claims.Claims()) {
        const auto* answers = audits.Answers(range);
        if (!answers || !(*answers)[0] || !(*answers)[1]) continue;
        complete.push_back(GoldCompletedAudit{range,
                                              std::max((*answers)[0]->height, (*answers)[1]->height),
                                              owner.first, claims.GoldsBefore(owner.first)});
    }
    std::sort(complete.begin(), complete.end(),
              [](const GoldCompletedAudit& a, const GoldCompletedAudit& b) {
                  if (a.completion_height != b.completion_height) {
                      return a.completion_height < b.completion_height;
                  }
                  return a.range_number < b.range_number;
              });
    return complete;
}

struct GoldRangeRelease {
    uint64_t range_number{};
    int32_t completion_height{};
    CAmount total{};
    std::array<std::optional<GoldPayoutShare>, GoldAuditSlots> shares;
    bool operator==(const GoldRangeRelease&) const = default;
};

// The releases a block at `parent_height` may mint: every completed audit after the
// `released` cursor, in order, whose second answer is already behind the parent.
//
// The caller supplies the miner's script because it lives in the gold block's body
// rather than in the index. A body that cannot be read is a failure and not a
// reason to skip: skipping would pay the ranges behind it and leave this one to be
// retried forever, so the plan refuses instead and the caller holds the block.
inline std::optional<std::vector<GoldRangeRelease>> PlanGoldRangeReleases(
    const GoldEpochDefinition& epochs, const GoldCountsLedger& claims, const GoldAuditLedger& audits,
    uint64_t released, int32_t parent_height,
    const std::function<std::optional<CScript>(int32_t gold_height)>& miner_script_at)
{
    if (!GoldEpochDefinitionValid(epochs)) return {};
    const auto complete = GoldCompletedAudits(claims, audits);
    if (released > complete.size()) return {};

    std::vector<GoldRangeRelease> releases;
    for (size_t index = size_t(released); index < complete.size(); ++index) {
        const auto& entry = complete[index];
        // Nothing is released in the block that carries the second answer itself, so
        // nobody can answer and pay in the same coinbase.
        if (entry.completion_height > parent_height) break;
        const auto script = miner_script_at(entry.gold_height);
        if (!script) return {};
        const auto quote = QuoteGoldRangePayout(epochs, entry.ordinal, *script, audits,
                                                entry.range_number);
        if (!quote) return {};
        GoldRangeRelease release;
        release.range_number = entry.range_number;
        release.completion_height = entry.completion_height;
        release.shares = quote->auditors;
        for (const auto& share : release.shares) {
            if (share) release.total += share->amount;
        }
        releases.push_back(std::move(release));
    }
    return releases;
}

} // namespace pgn::core
