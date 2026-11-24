#undef NDEBUG
// Phase 1 coverage proof: 100 counts in the gold header replace the recorded
// gap list. Three things must hold for that to be safe, and each is pinned
// here: the range number identifies exactly one interval, the counts are
// collected with the same partition split the audits use, and the counts root
// selects the same partitions through the shared selector as the report root
// always did.
#include <pgn/gold_partition_counts.h>
#include <pgn/native_report_audit.h>
#include <util/strencodings.h>

#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>

using namespace pgn::core;

static void need(bool yes, const char* reason)
{
    if (!yes) throw std::runtime_error(reason);
}

// The published report-root selector, kept verbatim as the reference the shared
// implementation must not drift from.
static std::array<std::array<uint32_t, 5>, 2> LegacyAuditPartitions(const uint256& root, const uint256& seed)
{
    const std::string prefix = "PGN/audit-partitions/1";
    Bytes input{prefix.begin(), prefix.end()};
    input.push_back(0);
    const auto root_bytes = ParseHex(root.GetHex()), seed_bytes = ParseHex(seed.GetHex());
    input.insert(input.end(), root_bytes.begin(), root_bytes.end());
    input.insert(input.end(), seed_bytes.begin(), seed_bytes.end());
    std::vector<uint32_t> remaining(100);
    for (uint32_t i = 0; i < 100; ++i) remaining[i] = i;
    std::array<std::array<uint32_t, 5>, 2> result;
    uint64_t counter{};
    unsigned selected{};
    const Integer space = Integer{1} << 256;
    while (selected < 10) {
        auto bytes = input;
        for (int i = 7; i >= 0; --i) bytes.push_back(uint8_t(counter >> (8 * i)));
        ++counter;
        Digest out;
        CSHA256{}.Write(bytes.data(), bytes.size()).Finalize(out.data());
        Integer value;
        import_bits(value, out.begin(), out.end(), 8, true);
        const Integer bound = space - space % remaining.size();
        if (value >= bound) continue;
        const auto position = (value % remaining.size()).convert_to<size_t>();
        result[selected / 5][selected % 5] = remaining[position];
        remaining.erase(remaining.begin() + position);
        ++selected;
    }
    for (auto& group : result) std::sort(group.begin(), group.end());
    return result;
}

static void CheckRangeNumbers()
{
    const Integer origin{"150000000000000000000"}, width{"200000000000000"};
    const Integer limit = Integer{1} << 95;
    const Integer ranges = (limit - origin + width - 1) / width;
    need(ranges < (Integer{1} << GoldRangeNumberBits), "Published range domain does not fit 48 bits");

    need(GoldRangeNumber(origin, width, origin) == std::optional<uint64_t>{0}, "Origin is not range 0");
    need(GoldRangeNumber(origin, width, origin + width) == std::optional<uint64_t>{1}, "Width is not range 1");
    need(GoldRangeNumber(origin, width, origin + 12345 * width) == std::optional<uint64_t>{12345},
         "Range number is not offset/width");
    need(!GoldRangeNumber(origin, width, origin + width - 1).has_value(), "Off-boundary start got a number");
    need(!GoldRangeNumber(origin, width, origin - width).has_value(), "Start below origin got a number");
    need(GoldRangeNumber(origin, width, origin + (Integer{1} << GoldRangeNumberBits) * width) == std::nullopt,
         "Range number outside 48 bits was accepted");
    need(GoldRangeStart(origin, width, 7777) == std::optional{origin + 7777 * width}, "Start does not invert number");
    need(!GoldRangeStart(origin, width, uint64_t{1} << GoldRangeNumberBits).has_value(),
         "Start accepted a number outside 48 bits");
    need(!GoldRangeStart(origin, Integer{0}, 1).has_value(), "Start accepted a zero width");
}

static void CheckPartitionSplit()
{
    // 150 is deliberately not a multiple of 100: the naive (d*100)/length bin
    // and the canonical split disagree there, and only the canonical split may
    // be used by the scan and the audit alike.
    const Integer start{1000}, length{150};
    size_t disagreeing{};
    for (size_t d = 0; d < 150; ++d) {
        const Integer lower = start + Integer{static_cast<unsigned long long>(d)};
        const auto index = GoldPartitionIndex(start, length, lower);
        need(index.has_value() && *index < GoldPartitionCount, "Position has no partition");
        const Integer naive{(lower - start) * GoldPartitionCount / length};
        if (naive.convert_to<size_t>() != *index) ++disagreeing;
        const Integer first = start + length * Integer{*index} / GoldPartitionCount;
        const Integer end = start + length * Integer{*index + 1} / GoldPartitionCount;
        need(lower >= first && lower < end, "Partition does not own the position");
    }
    need(disagreeing > 0, "Non-divisible length no longer distinguishes the two bins");
    need(!GoldPartitionIndex(start, length, start - 1).has_value(), "Position below the range got a partition");
    need(!GoldPartitionIndex(start, length, start + length).has_value(), "Position past the range got a partition");
    need(!GoldPartitionIndex(start, Integer{0}, start).has_value(), "Empty range got a partition");

    // Every partition of a whole range is reachable and capacities sum to the
    // eligible positions.
    const Integer wide_start{99}, wide_length{250};
    Integer capacity{};
    std::set<size_t> seen;
    for (size_t d = 0; d < 250; ++d) {
        const auto index = GoldPartitionIndex(wide_start, wide_length,
                                             wide_start + Integer{static_cast<unsigned long long>(d)});
        need(index.has_value(), "Position has no partition");
        seen.insert(*index);
    }
    for (size_t i = 0; i < GoldPartitionCount; ++i) capacity += GoldPartitionCapacity(wide_start, wide_length, i);
    need(seen.size() == GoldPartitionCount, "Not every partition is reachable");
    need(capacity == 125, "Partition capacities do not cover the odd positions");
}

static void CheckCountsRoot()
{
    // Exactly sha256 of the 100 count bytes: no tag, no length prefix.
    const GoldPartitionCounts zeros{};
    need(HexStr(GoldCountsRoot(zeros)) == "cd00e292c5970d3c5e2f0ffa5171e555bc46bfc4faddfb4a418b6840b86e79a3",
         "Zero counts root changed");
    GoldPartitionCounts ramp{};
    for (size_t i = 0; i < GoldPartitionCount; ++i) ramp[i] = static_cast<uint8_t>(i / 10);
    need(HexStr(GoldCountsRoot(ramp)) == "cd8334fd6d4b523a20427a95cdf5b35e319d42b28e76b21695559b032f936444",
         "Ramp counts root changed");
    need(GoldCountsTotal(ramp) == 450, "Ramp total changed");
    need(GoldCountsRootId(zeros).GetHex() == HexStr(GoldCountsRoot(zeros)),
         "Counts root id is not the digest read big-endian");

    // Moving one gap between partitions changes the root, so a miner cannot
    // rebalance counts after inclusion.
    auto moved = ramp;
    --moved[0];
    ++moved[GoldPartitionCount - 1];
    need(GoldCountsRoot(moved) != GoldCountsRoot(ramp), "Root ignored a rebalance");

    const Integer start{0}, length{1000};
    need(GoldCountsBounded(zeros, start, length), "Zero counts are not bounded");
    auto over = zeros;
    over[0] = 200; // partition 0 owns [0, 10): five odd positions
    need(!GoldCountsBounded(over, start, length), "Count above partition capacity was accepted");
    over[0] = 5;
    need(GoldCountsBounded(over, start, length), "Count at partition capacity was rejected");
    need(!GoldCountsBounded(zeros, start, Integer{0}), "Empty range is bounded");
}

static void CheckAccumulator()
{
    const Integer start{0}, length{100000}; // one partition per 1000 positions
    GoldCountsAccumulator accumulator{start, length};
    std::array<uint64_t, GoldPartitionCount> expected{};
    for (size_t partition = 0; partition < GoldPartitionCount; ++partition) {
        for (size_t k = 0; k < 5; ++k) {
            const Integer lower =
                start + Integer{static_cast<unsigned long long>(partition * 1000 + 2 * k)};
            need(accumulator.Add(lower), "Scan count rejected an owned position");
            ++expected[*GoldPartitionIndex(start, length, lower)];
        }
    }
    for (size_t i = 0; i < GoldPartitionCount; ++i) {
        need(accumulator.Counts()[i] == expected[i], "Accumulated count differs from a recount");
    }
    need(!accumulator.Overflowed(), "Accumulator claims an overflow");
    need(GoldCountsTotal(accumulator.Counts()) == 500, "Accumulator total is wrong");
    need(GoldCountsBounded(accumulator.Counts(), start, length), "Accumulated counts are not bounded");

    // A partition that would exceed one byte stops the range instead of
    // wrapping, and an unowned position stops it too.
    GoldCountsAccumulator saturated{start, length};
    for (unsigned i = 0; i < 255; ++i) need(saturated.Add(Integer{static_cast<unsigned long long>(2 * i)}),
                                           "Saturation rejected a legal count");
    need(!saturated.Add(Integer{510}), "Overflow was accepted");
    need(saturated.Overflowed(), "Overflow was not recorded");

    GoldCountsAccumulator foreign{start, length};
    need(!foreign.Add(start + length), "Out-of-range position was counted");
    need(foreign.Overflowed(), "Out-of-range position was not recorded");
}

static void CheckHeaderCodec()
{
    need(GoldCountsHeaderBytes == 106, "Header section is not 6 bytes of number plus 100 counts");
    // The section sits between nBits and nNonce, so the counter family the
    // local block generator patches stays the last ten header bytes.
    need(GoldCountsSectionOffset == 80, "Counts section does not start after nBits");
    need(GoldHeaderBytes == 218 && GoldCountsSectionOffset + GoldCountsHeaderBytes + GoldNonceBytes == 218,
         "Gold header is not 112 native bytes plus the 106-byte section");
    GoldPartitionCounts counts{};
    for (size_t i = 0; i < GoldPartitionCount; ++i) counts[i] = static_cast<uint8_t>(i % 13);
    const uint64_t number = 0x0000'1234'5678'9abc & ((uint64_t{1} << GoldRangeNumberBits) - 1);
    const auto encoded = EncodeGoldCountsHeader(number, counts);
    need(encoded.has_value(), "Canonical header section was rejected");
    need(encoded->size() == 106, "Encoded section has the wrong size");
    need((*encoded)[0] == static_cast<uint8_t>(number) && (*encoded)[5] == static_cast<uint8_t>(number >> 40),
         "Range number is not six little-endian bytes first");
    const auto decoded = DecodeGoldCountsHeader(*encoded);
    need(decoded.has_value() && decoded->first == number && decoded->second == counts,
         "Header section does not round-trip");
    need(!DecodeGoldCountsHeader(std::span<const uint8_t>{encoded->data(), encoded->size() - 1}).has_value(),
         "Short header section was decoded");
    need(!EncodeGoldCountsHeader(uint64_t{1} << GoldRangeNumberBits, counts).has_value(),
         "Out-of-domain range number was encoded");

    const std::array<uint8_t, GoldCountsHeaderBytes> zeros{};
    const auto zero_section = DecodeGoldCountsHeader(zeros);
    need(zero_section.has_value() && zero_section->first == 0 && zero_section->second == GoldPartitionCounts{},
         "Zero header section is not range 0 with zero counts");
}

static void CheckPartitionSeeds()
{
    GoldPartitionCounts counts{};
    for (size_t i = 0; i < GoldPartitionCount; ++i) counts[i] = static_cast<uint8_t>(i % 7);
    const auto seeds = {uint256::FromHex("0102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f20").value(),
                        uint256::FromHex("ff00000000000000000000000000000000000000000000000000000000000000").value(),
                        uint256::FromHex("0000000000000000000000000000000000000000000000000000000000000001").value()};
    const auto reports = {GoldCountsRootId(counts), GoldCountsRootId(GoldPartitionCounts{}),
                          uint256{}, uint256::FromHex("00000000000000000000000000000000000000000000000000000000deadbeef").value()};
    for (const auto& root : reports) {
        for (const auto& seed : seeds) {
            // The report-root path is unchanged bit for bit.
            need(NativeAuditPartitions(root, seed) == LegacyAuditPartitions(root, seed),
                 "Shared selector drifted from the published report-root selector");
        }
    }
    for (const auto& seed : seeds) {
        const uint256 root = GoldCountsRootId(counts);
        // The counts path is the same selection over sha256(counts).
        const auto direct = GoldAuditPartitions(counts, seed);
        need(direct == NativeAuditPartitions(root, seed), "Counts path and report-root path disagree");
        need(direct == GoldAuditPartitions(counts, Uint256Digest(seed)), "Digest and uint256 seeds disagree");
        for (const auto& slot : direct) {
            std::set<uint32_t> distinct(slot.begin(), slot.end());
            need(distinct.size() == 5, "A slot repeats a partition");
            need(distinct.size() == 5 && *distinct.begin() < 100 && *distinct.rbegin() < 100,
                 "A slot left the partition domain");
            need(std::is_sorted(slot.begin(), slot.end()), "A slot is not sorted");
        }
        need(std::set<uint32_t>(direct[0].begin(), direct[0].end()).size() == 5 &&
                 std::set<uint32_t>(direct[1].begin(), direct[1].end()).size() == 5,
             "Slots are not separate selections");
    }

    // Different counts must not select the same partitions by accident of an
    // empty count vector.
    auto other = counts;
    other[42] ^= 1;
    need(GoldAuditPartitions(counts, *seeds.begin()) != GoldAuditPartitions(other, *seeds.begin()),
         "Partition selection ignores the counts");
}

int main()
{
    try {
        CheckRangeNumbers();
        CheckPartitionSplit();
        CheckCountsRoot();
        CheckAccumulator();
        CheckHeaderCodec();
        CheckPartitionSeeds();
        std::cout << "gold partition counts ok" << std::endl;
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "gold partition counts failed: " << failure.what() << std::endl;
        return 1;
    }
}
