// Copyright (c) 2026 The PGN developers. MIT. RPC projection only.
#pragma once
#include <chain.h>
#include <pgn/leaderboard_utc_period.h>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
namespace pgn::core {
inline int64_t LeaderboardEventTime(const CBlockIndex& tip,int32_t height){
    if(height<0||height>tip.nHeight)throw std::runtime_error("Native event date unavailable");
    const auto* block=tip.GetAncestor(height);
    if(!block||!block->phashBlock||!block->IsValid(BLOCK_VALID_SCRIPTS))
        throw std::runtime_error("Native event date provenance unavailable");
    return block->GetBlockTime(); // canonical header clock, never physical runtime
}
} // namespace pgn::core
