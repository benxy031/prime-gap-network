// Copyright (c) 2026 The PGN developers. MIT.
// Where a counted range's money goes.
//
// The audit envelope says who answered; it cannot say who is paid, because the
// chain pays a script, not a claim. So both ends of the payout are committed
// explicitly and once:
//
//     the gold block     carries PGNGOLDREWARD-1 with the miner's script
//     each answer        carries the auditor's script (PGNGOLDAUDIT-1 v2)
//
// Both are zero-value OP_RETURN outputs, like every other declaration on this
// chain, so committing a payee never mints anything. Nothing here moves funds;
// the settlement page is what turns a committed script into a payment.
#pragma once

#include <pgn/gold_partition_counts.h>
#include <pgn/range_commitment.h>
#include <primitives/transaction.h>
#include <script/script.h>

#include <optional>
#include <string_view>
#include <vector>

namespace pgn::core {

inline constexpr std::string_view GoldRewardTag{"PGNGOLDREWARD-1"};
inline constexpr uint8_t GoldRewardVersion = 1;
// A payee is a standard output script; the bound keeps the declaration inside the
// same budget as every other coinbase output.
inline constexpr size_t GoldRewardMaximumScriptBytes = 100;

inline std::optional<CScript> BuildGoldRewardEnvelope(const CScript& miner)
{
    if (miner.empty() || miner.size() > GoldRewardMaximumScriptBytes) return {};
    Bytes payload{GoldRewardVersion};
    payload.insert(payload.end(), miner.begin(), miner.end());
    return CScript{} << OP_RETURN << Bytes{GoldRewardTag.begin(), GoldRewardTag.end()} << payload;
}

inline bool IsGoldRewardScript(const CScript& script)
{
    auto cursor = script.begin();
    opcodetype op;
    Bytes tag;
    return script.GetOp(cursor, op) && op == OP_RETURN && script.GetOp(cursor, op, tag) &&
           tag == Bytes{GoldRewardTag.begin(), GoldRewardTag.end()};
}

// Only the exact canonical encoding decodes, which is what stops a script from
// being committed twice in two different byte sequences.
inline std::optional<CScript> DecodeGoldRewardEnvelope(const CScript& script)
{
    if (!IsGoldRewardScript(script)) return {};
    auto cursor = script.begin();
    opcodetype op;
    Bytes tag, payload;
    if (!script.GetOp(cursor, op) || !script.GetOp(cursor, op, tag) ||
        !script.GetOp(cursor, op, payload) || cursor != script.end()) {
        return {};
    }
    if (payload.size() < 2 || payload.front() != GoldRewardVersion ||
        payload.size() - 1 > GoldRewardMaximumScriptBytes) {
        return {};
    }
    const CScript miner{payload.begin() + 1, payload.end()};
    const auto rebuilt = BuildGoldRewardEnvelope(miner);
    if (!rebuilt || *rebuilt != script) return {};
    return miner;
}

enum class GoldRewardBlockVerdict {
    Absent,  // no commitment in this block
    Present, // exactly one canonical, zero-value commitment
    Invalid, // more than one, not zero-value, or not canonical
};

struct GoldRewardBlockRead {
    GoldRewardBlockVerdict verdict{GoldRewardBlockVerdict::Absent};
    std::optional<CScript> miner;
};

// The archive hands out a verified coinbase rather than a whole block, and the
// payee lives in the coinbase, so that is the primitive.
inline GoldRewardBlockRead ReadGoldRewardFromCoinbase(const CTransaction& coinbase,
                                                      uint64_t maximum_script_bytes)
{
    GoldRewardBlockRead read;
    if (!coinbase.IsCoinBase()) return read;
    for (const auto& output : coinbase.vout) {
        if (!IsGoldRewardScript(output.scriptPubKey)) continue;
        if (read.verdict != GoldRewardBlockVerdict::Absent) {
            read.verdict = GoldRewardBlockVerdict::Invalid;
            read.miner.reset();
            return read;
        }
        read.verdict = GoldRewardBlockVerdict::Invalid;
        if (output.nValue != 0 || output.scriptPubKey.size() > maximum_script_bytes) continue;
        read.miner = DecodeGoldRewardEnvelope(output.scriptPubKey);
        if (read.miner) read.verdict = GoldRewardBlockVerdict::Present;
    }
    return read;
}

inline GoldRewardBlockRead ReadGoldRewardFromBlock(const CBlock& block, uint64_t maximum_script_bytes)
{
    if (block.vtx.empty() || !block.vtx.front()) return {};
    return ReadGoldRewardFromCoinbase(*block.vtx.front(), maximum_script_bytes);
}

} // namespace pgn::core
