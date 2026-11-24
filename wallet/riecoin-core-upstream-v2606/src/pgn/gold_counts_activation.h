// Copyright (c) 2026 The PGN developers. MIT.
// Genesis-committed activation for the counts-based gold block.
//
// The published testnet must keep behaving exactly as it does today, so the new
// rule cannot be a compile-time default and must not depend on an environment
// variable at validation time. It is committed in the genesis coinbase instead,
// like every other PGN policy: a chain whose genesis carries
// PGNGOLDCOUNTS-POLICY-1 reads counts as gold coverage, and a chain without it
// does not. The local harness chain carries it; an empty suffix cannot, because
// that genesis is pinned byte for byte.
#pragma once

#include <pgn/chain_range_policy.h>
#include <consensus/merkle.h>
#include <primitives/block.h>
#include <script/script.h>

#include <optional>
#include <string_view>

namespace pgn::core {

inline constexpr std::string_view GoldCountsPolicyTag{"PGNGOLDCOUNTS-POLICY-1"};
inline constexpr uint8_t GoldCountsPolicyVersion = 1;
inline constexpr size_t GoldCountsPolicyPayloadBytes = 1;

struct GoldCountsPolicy {
    // The envelope carries no tunables yet: counts, range numbers and the audit
    // selector are all fixed by the format itself. A version byte keeps room for
    // one without a second tag.
    uint8_t version{GoldCountsPolicyVersion};
};

inline std::optional<CScript> BuildGoldCountsPolicyEnvelope(RangePolicyReadBudget budget)
{
    if (!budget.maximum_outputs || !budget.maximum_script_bytes ||
        budget.maximum_payload_bytes < GoldCountsPolicyPayloadBytes) return {};
    const Bytes payload{GoldCountsPolicyVersion};
    CScript script = CScript{} << OP_RETURN
                              << Bytes{GoldCountsPolicyTag.begin(), GoldCountsPolicyTag.end()}
                              << payload;
    if (script.size() > budget.maximum_script_bytes) return {};
    return script;
}

inline std::optional<GoldCountsPolicy> DecodeGoldCountsPolicyPayload(const Bytes& payload,
                                                                    RangePolicyReadBudget budget)
{
    if (payload.size() > budget.maximum_payload_bytes) return {};
    if (payload.size() != GoldCountsPolicyPayloadBytes) return {};
    if (payload.front() != GoldCountsPolicyVersion) return {};
    return GoldCountsPolicy{payload.front()};
}

// Reads the policy from the chain's own genesis coinbase. `std::nullopt` means
// the genesis is malformed or declares the policy more than once; `false`
// (an engaged optional holding no policy) means it is absent, which is the
// published testnet. The genesis hash is not trusted here; callers must
// authenticate it against their pinned chain parameters before acting on this.
inline std::optional<std::optional<GoldCountsPolicy>> ReadGoldCountsPolicyFromGenesis(
    const CBlock& genesis, RangePolicyReadBudget budget)
{
    if (!budget.maximum_outputs || !budget.maximum_script_bytes ||
        !budget.maximum_payload_bytes) return {};
    if (genesis.vtx.size() != 1 || !genesis.vtx.front() || !genesis.vtx.front()->IsCoinBase()) return {};
    const auto& outputs = genesis.vtx.front()->vout;
    if (outputs.size() > budget.maximum_outputs) return {};
    const Bytes wanted{GoldCountsPolicyTag.begin(), GoldCountsPolicyTag.end()};
    std::optional<Bytes> found;
    uint64_t remaining = budget.maximum_script_bytes;
    for (const auto& output : outputs) {
        const auto& script = output.scriptPubKey;
        if (script.size() > remaining) return {};
        remaining -= script.size();
        auto cursor = script.begin();
        opcodetype op;
        Bytes tag, payload;
        if (!script.GetOp(cursor, op) || op != OP_RETURN) continue;
        if (!script.GetOp(cursor, op, tag) || tag != wanted) continue;
        // A policy output is never a payment, and must be the whole script.
        if (found || output.nValue != 0) return {};
        if (!script.GetOp(cursor, op, payload) || cursor != script.end()) return {};
        if (script != (CScript{} << OP_RETURN << tag << payload)) return {};
        const auto policy = DecodeGoldCountsPolicyPayload(payload, budget);
        if (!policy) return {};
        found = payload;
    }
    if (!found) return std::optional<GoldCountsPolicy>{};
    return std::optional<GoldCountsPolicy>{*DecodeGoldCountsPolicyPayload(*found, budget)};
}

} // namespace pgn::core
