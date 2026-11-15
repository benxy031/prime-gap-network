// Copyright (c) 2026 The PGN developers. MIT.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace pgn::core {

// Integer must preserve exact nonnegative arithmetic at the scientific range
// size (the native caller uses the existing arbitrary-precision Integer type).
// Inputs are ranges already qualified by the authenticated active-chain view.
// A random ticket/request ID is deliberately absent from this calculation.
template <class Integer> struct ValidatedRangeCoverage {
    Integer origin;
    Integer width;
    Integer contiguous_end; // Exclusive; [origin, contiguous_end) has no holes.
    Integer first_missing_index; // Zero-based scientific range index.
    std::optional<Integer> last_validated_end;
    uint64_t validated_ranges{}; // Deduplicated; reissued ranges count once.
};

template <class Integer>
std::optional<ValidatedRangeCoverage<Integer>> BuildValidatedRangeCoverage(
    const Integer& origin, const Integer& width,
    const std::vector<std::pair<Integer, Integer>>& qualified_ranges)
{
    if (origin < 0 || width <= 0) return std::nullopt;
    std::vector<Integer> starts;
    starts.reserve(qualified_ranges.size());
    for (const auto& [start, end] : qualified_ranges) {
        if (start < origin || end != start + width || (start - origin) % width != 0)
            return std::nullopt; // Missing/invalid provenance is not zero coverage.
        starts.push_back(start);
    }
    std::sort(starts.begin(), starts.end());
    starts.erase(std::unique(starts.begin(), starts.end()), starts.end());
    ValidatedRangeCoverage<Integer> result{origin, width, origin, Integer{0}, std::nullopt, 0};
    result.validated_ranges = static_cast<uint64_t>(starts.size());
    if (!starts.empty()) result.last_validated_end = starts.back() + width;
    for (const auto& start : starts) {
        if (start != result.contiguous_end) break;
        result.contiguous_end += width;
    }
    result.first_missing_index = (result.contiguous_end - origin) / width;
    return result;
}
} // namespace pgn::core
