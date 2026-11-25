// Copyright (c) 2026 The PGN developers. MIT.
// What one paid range owes, and to whom.
//
// Everything here is decided already: the miner's script is in the gold block, the
// two auditors' scripts are in their answers, the miner's share is the epoch grant
// committed in the genesis rewards blob, and the auditor's share is exactly a
// quarter of it. What is left is arithmetic - so the arithmetic lives in one
// place, is pure, and a settlement page can only ever quote these numbers.
//
// Nothing here mints or authorises a payment. It answers "who would be paid how
// much" from committed data, which is what a page builder needs and what a test
// can prove without a live issuance state.
#pragma once

#include <pgn/gold_audit.h>
#include <pgn/gold_epoch_policy.h>

#include <array>
#include <optional>

namespace pgn::core {

struct GoldPayoutShare {
    CScript script;
    CAmount amount{};
    bool operator==(const GoldPayoutShare&) const = default;
};

struct GoldRangePayout {
    // The whole grant for this gold: miner + near-miss + audit. Reported so a
    // reader can see the split is inside one budget rather than on top of it.
    CAmount gross{};
    CAmount miner{};
    CAmount audit_pot{};
    std::array<std::optional<GoldPayoutShare>, GoldAuditSlots> auditors;
    // Every slot answered inside the window. An incomplete audit pays nobody yet,
    // and one that ran out of window never will.
    bool payable{};
};

// `gold_ordinal` is how many golds this chain paid before this one - the epoch
// policy is indexed by it, so it has to come from the authenticated ledger and not
// from the range number.
inline std::optional<GoldRangePayout> QuoteGoldRangePayout(
    const GoldEpochDefinition& epochs, uint64_t gold_ordinal, const CScript& miner_script,
    const GoldAuditLedger& audits, uint64_t range_number)
{
    if (miner_script.empty() || miner_script.size() > GoldRewardMaximumScriptBytes) return {};
    if (!range_number) return {};
    const auto grant = QuoteGoldEpoch(epochs, gold_ordinal);
    if (!grant) return {};

    GoldRangePayout payout;
    payout.miner = grant->gold;
    payout.audit_pot = grant->audit;
    payout.gross = grant->gold + grant->silver + grant->bronze + grant->red + grant->audit;
    // The redesign's rule is that the auditor earns a quarter of the miner, and the
    // rewards blob is what expresses it. A chain whose committed split says otherwise
    // is refused here rather than quietly paid something else.
    if (payout.miner <= 0 || payout.audit_pot * 4 != payout.miner) return {};

    // The published task splitter, so the two slots divide the pot exactly and no
    // atomic unit disappears through rounding.
    const auto shares = SplitAuditPot(payout.audit_pot, GoldAuditSlots);
    if (!shares || shares->size() != GoldAuditSlots) return {};

    const auto* answers = audits.Answers(range_number);
    if (!answers) return payout;
    for (size_t slot = 0; slot < GoldAuditSlots; ++slot) {
        if (!(*answers)[slot]) continue;
        payout.auditors[slot] = GoldPayoutShare{(*answers)[slot]->answer.reward, (*shares)[slot]};
    }
    payout.payable = payout.auditors[0].has_value() && payout.auditors[1].has_value();
    return payout;
}

} // namespace pgn::core
