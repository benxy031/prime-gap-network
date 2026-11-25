#undef NDEBUG
// Phase 1 semantics, without the header move: a gold block declares its range
// number and 100 counts in one canonical coinbase envelope, a range number is
// paid to exactly one block ever, and only a chain whose genesis commits the
// rule reads counts as coverage. Each of those is a way to lose or duplicate a
// payout, so each is pinned here.
#include <pgn/gold_counts_activation.h>
#include <pgn/gold_counts_envelope.h>
#include <pgn/testnet_definition.h>
#include <util/translation.h>

#include <iostream>
#include <stdexcept>
#include <string>

extern const TranslateFn G_TRANSLATION_FUN = [](const char* text) { return std::string(text); };

using namespace pgn::core;

static void need(bool yes, const char* reason)
{
    if (!yes) throw std::runtime_error(reason);
}

static constexpr RangePolicyReadBudget kBudget{32, 512, 65536, 32768};
static const Integer kOrigin{"150000000000000000000"};
static const Integer kWidth{"200000000000000"};

static GoldPartitionCounts Counts(uint8_t base)
{
    GoldPartitionCounts counts{};
    for (size_t i = 0; i < GoldPartitionCount; ++i) counts[i] = static_cast<uint8_t>((base + i) % 11);
    return counts;
}

static CBlock BlockWithOutputs(const std::vector<CTxOut>& outputs)
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript{} << OP_0 << int64_t{1};
    coinbase.vout = outputs;
    CBlock block;
    block.nVersion = 4;
    block.nTime = 1'700'000'000;
    block.nBits = 0x207fffff;
    block.nNonce = NativeBootstrapNonce(0);
    block.vtx = {MakeTransactionRef(coinbase)};
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

static void CheckEnvelope()
{
    const GoldCountsRecord record{12345, Counts(0)};
    const auto script = BuildGoldCountsEnvelope(record.range_number, record.counts);
    need(script.has_value(), "Canonical envelope was rejected");
    need(IsGoldCountsScript(*script), "Envelope is not recognised as gold counts");
    // 3 opcode/tag bytes + push header + 1 version + 6 number + 100 counts.
    need(script->size() == 1 + 1 + GoldCountsTag.size() + 2 + GoldCountsPayloadBytes,
         "Envelope size is not the canonical one");
    const auto decoded = DecodeGoldCountsEnvelope(*script, GoldCountsMaximumScriptBytes);
    need(decoded.has_value() && *decoded == record, "Envelope does not round-trip");

    // Every range number in the domain must survive the six-byte field.
    for (const uint64_t number : {uint64_t{0}, uint64_t{1}, (uint64_t{1} << 40) + 7,
                                  (uint64_t{1} << GoldRangeNumberBits) - 1}) {
        const auto edge = BuildGoldCountsEnvelope(number, GoldPartitionCounts{});
        need(edge.has_value(), "In-domain range number was rejected");
        const auto back = DecodeGoldCountsEnvelope(*edge, GoldCountsMaximumScriptBytes);
        need(back && back->range_number == number, "Range number did not round-trip");
    }
    need(!BuildGoldCountsEnvelope(uint64_t{1} << GoldRangeNumberBits, GoldPartitionCounts{})
              .has_value(),
         "Out-of-domain range number was encoded");

    // Counts are part of the commitment, so a single changed byte must change
    // the script and the round trip must notice.
    auto other = record;
    other.counts[57] ^= 0x40;
    const auto other_script = BuildGoldCountsEnvelope(other.range_number, other.counts);
    need(other_script && *other_script != *script, "Counts byte does not affect the envelope");

    // Foreign and malformed scripts are rejected, never partially decoded.
    need(!DecodeGoldCountsEnvelope(CScript{}, GoldCountsMaximumScriptBytes).has_value(),
         "Empty script decoded as gold counts");
    need(!IsGoldCountsScript(CScript{} << OP_RETURN << Bytes{'n', 'o'}), "Foreign tag recognised");
    need(!DecodeGoldCountsEnvelope(*script, script->size() - 1).has_value(),
         "Envelope over the caller's budget decoded");

    // Trailing junk after a valid payload.
    CScript trailing = *script;
    trailing << OP_0;
    need(!DecodeGoldCountsEnvelope(trailing, GoldCountsMaximumScriptBytes).has_value(),
         "Trailing bytes decoded as gold counts");
    // A wrong version byte.
    Bytes payload{GoldCountsVersion + 1};
    const auto section = EncodeGoldCountsHeader(7, GoldPartitionCounts{});
    need(section.has_value(), "Canonical section rejected");
    payload.insert(payload.end(), section->begin(), section->end());
    const CScript wrong_version = CScript{} << OP_RETURN
                                            << Bytes{GoldCountsTag.begin(), GoldCountsTag.end()}
                                            << payload;
    need(!DecodeGoldCountsEnvelope(wrong_version, GoldCountsMaximumScriptBytes).has_value(),
         "Unknown envelope version decoded");
    // A short payload.
    const CScript truncated = CScript{} << OP_RETURN
                                        << Bytes{GoldCountsTag.begin(), GoldCountsTag.end()}
                                        << Bytes{GoldCountsVersion, 0, 0};
    need(!DecodeGoldCountsEnvelope(truncated, GoldCountsMaximumScriptBytes).has_value(),
         "Short payload decoded");
}

static void CheckBounds()
{
    const GoldCountsRecord record{0, GoldPartitionCounts{}};
    need(GoldCountsRecordBounded(record, kOrigin, kWidth), "Zero counts are not bounded");
    need(!GoldCountsRecordBounded(record, kOrigin, Integer{0}), "Zero width accepted");

    // Partition 0 of a ticket-width range holds far more odd positions than one
    // byte can express, so a full partition is legal but 256 is not encodable.
    auto wide = record;
    wide.counts[0] = 255;
    need(GoldCountsRecordBounded(wide, kOrigin, kWidth), "A full byte of counts was rejected");

    // A number outside the published domain has no interval at all.
    const GoldCountsRecord outside{(uint64_t{1} << GoldRangeNumberBits) - 1, GoldPartitionCounts{}};
    const auto range = GoldCountsRange(outside, kOrigin, kWidth);
    need(range.has_value(), "Top range number has no interval");
    need(range->first == kOrigin + Integer{outside.range_number} * kWidth, "Range start is wrong");
}

static void CheckBlockRead()
{
    const auto script = BuildGoldCountsEnvelope(99, Counts(3));
    need(script.has_value(), "Fixture envelope was rejected");
    const GoldCountsRecord record{99, Counts(3)};
    const auto section = EncodeGoldCountsHeader(record.range_number, record.counts);
    need(section.has_value(), "Fixture section rejected");

    // The header is the declaration; the coinbase alone is not.
    const auto plain = BlockWithOutputs({{0, CScript{} << OP_RETURN << Bytes{'P', 'G', 'N', 'B', 'O', 'O', 'T'}}});
    need(ReadGoldCountsFromBlock(plain, GoldCountsMaximumScriptBytes).verdict ==
             GoldCountsBlockVerdict::Absent,
         "A coordination block reported a gold declaration");
    const auto envelope_only = BlockWithOutputs({{0, *script}});
    need(ReadGoldCountsFromBlock(envelope_only, GoldCountsMaximumScriptBytes).verdict ==
             GoldCountsBlockVerdict::Absent,
         "A coinbase envelope without a header was read as the declaration");

    auto with_header = BlockWithOutputs({{0, CScript{} << OP_RETURN << Bytes{'P', 'G', 'N'}}});
    with_header.nVersion |= PGN_GOLD_COUNTS_VERSION_BIT;
    with_header.gold_counts = *section;
    const auto present = ReadGoldCountsFromBlock(with_header, GoldCountsMaximumScriptBytes);
    need(present.verdict == GoldCountsBlockVerdict::Present && present.record.has_value(),
         "A header declaration was not found");
    need(*present.record == record, "Wrong record read from the header");

    // An agreeing coinbase copy is tolerated, so an older runtime's block is not
    // reinterpreted; a disagreeing one is a lie in one of the two.
    auto agreeing = with_header;
    CMutableTransaction agreeing_tx;
    agreeing_tx.vin.resize(1);
    agreeing_tx.vin[0].prevout.SetNull();
    agreeing_tx.vin[0].scriptSig = CScript{} << OP_0 << int64_t{1};
    agreeing_tx.vout = {CTxOut{0, *BuildGoldCountsEnvelope(99, Counts(3))}};
    agreeing.vtx = {MakeTransactionRef(agreeing_tx)};
    need(ReadGoldCountsFromBlock(agreeing, GoldCountsMaximumScriptBytes).verdict ==
             GoldCountsBlockVerdict::Present,
         "An agreeing envelope was refused");

    auto disagreeing = agreeing;
    auto other = CMutableTransaction{*agreeing.vtx.front()};
    other.vout[0] = CTxOut{0, *BuildGoldCountsEnvelope(100, Counts(3))};
    disagreeing.vtx = {MakeTransactionRef(other)};
    need(ReadGoldCountsFromBlock(disagreeing, GoldCountsMaximumScriptBytes).verdict ==
             GoldCountsBlockVerdict::Invalid,
         "A header that disagrees with the envelope was accepted");

    // A paid declaration output is not a declaration, and it cannot ride along
    // with a header either.
    auto paid = agreeing;
    auto paid_tx = CMutableTransaction{*agreeing.vtx.front()};
    paid_tx.vout[0] = CTxOut{1, *BuildGoldCountsEnvelope(99, Counts(3))};
    paid.vtx = {MakeTransactionRef(paid_tx)};
    need(ReadGoldCountsFromBlock(paid, GoldCountsMaximumScriptBytes).verdict ==
             GoldCountsBlockVerdict::Invalid,
         "A paid gold output was accepted");

    // Two envelopes in one coinbase remain ambiguous.
    auto twice = with_header;
    auto twice_tx = CMutableTransaction{*agreeing.vtx.front()};
    twice_tx.vout.emplace_back(0, *BuildGoldCountsEnvelope(99, Counts(3)));
    twice.vtx = {MakeTransactionRef(twice_tx)};
    need(ReadGoldCountsFromBlock(twice, GoldCountsMaximumScriptBytes).verdict ==
             GoldCountsBlockVerdict::Invalid,
         "Two gold envelopes were accepted");

    // A block with no coinbase cannot carry a declaration.
    CBlock empty;
    empty.nVersion |= PGN_GOLD_COUNTS_VERSION_BIT;
    empty.gold_counts = *section;
    need(ReadGoldCountsFromBlock(empty, GoldCountsMaximumScriptBytes).verdict ==
             GoldCountsBlockVerdict::Invalid,
         "A coinbase-free counts block reported a declaration");
}

static void CheckPayOnce()
{
    GoldCountsLedger ledger;
    const auto a = uint256::FromHex("0000000000000000000000000000000000000000000000000000000000000001").value();
    const auto b = uint256::FromHex("0000000000000000000000000000000000000000000000000000000000000002").value();
    const auto c = uint256::FromHex("0000000000000000000000000000000000000000000000000000000000000003").value();
    const GoldCountsRecord first{7, Counts(0)};
    const GoldCountsRecord second{8, Counts(1)};

    need(ledger.Add(5, a, first).verdict == GoldCountsVerdict::Accepted, "First claim was refused");
    need(ledger.Contains(7) && ledger.size() == 1, "First claim was not recorded");
    need(ledger.Owner(7) == std::optional{std::pair{5, a}}, "First claim has the wrong owner");

    // A different block claiming the same range number is the case the whole
    // rule exists for.
    const auto duplicate = ledger.Add(6, b, first);
    need(duplicate.verdict == GoldCountsVerdict::DuplicateRangeNumber,
         "A second claim on one range number was accepted");
    need(ledger.size() == 1 && ledger.Owner(7) == std::optional{std::pair{5, a}},
         "A refused claim mutated the ledger");

    // Replaying the accepted block is not a new claim, so a restart or a re-read
    // of the same chain is idempotent.
    need(ledger.Add(5, a, first).verdict == GoldCountsVerdict::Accepted, "Replay was refused");
    need(ledger.size() == 1, "Replay duplicated a claim");

    // Forward progress is required: the ledger is an ordered replay, not a set.
    need(ledger.Add(4, c, second).verdict == GoldCountsVerdict::OutOfOrder,
         "A block older than the ledger was accepted");
    need(ledger.Add(7, b, second).verdict == GoldCountsVerdict::Accepted, "A later claim was refused");
    need(ledger.Height() == 7 && ledger.size() == 2, "Ledger progress is wrong");
    need(!ledger.Contains(9), "An unclaimed range number is reported as claimed");

    // An orphaned claim keeps its range number spent: a reorg must not hand the
    // same range to a second miner.
    need(ledger.Add(8, c, first).verdict == GoldCountsVerdict::DuplicateRangeNumber,
         "A reorg re-used a spent range number");
}

static void CheckActivation()
{
    // The published testnet must not carry the rule, and its genesis must stay
    // the value the rest of the tree pins.
    const auto published = BuildPgnTestnetDefinition();
    need(published.pinned_genesis.GetHex() ==
             "710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314",
         "Published genesis changed");
    const auto published_policy = ReadGoldCountsPolicyFromGenesis(published.genesis, kBudget);
    need(published_policy.has_value() && !published_policy->has_value(),
         "Published genesis declares the gold counts rule");

    // A suffixed chain commits it.
    const auto local = BuildPgnTestnetDefinition("phase1");
    need(local.pinned_genesis != published.pinned_genesis, "Suffix did not change the genesis");
    const auto local_policy = ReadGoldCountsPolicyFromGenesis(local.genesis, kBudget);
    need(local_policy.has_value() && local_policy->has_value(), "Local genesis omits the rule");
    need(local_policy->has_value() &&
             local_policy->value().version == GoldCountsPolicyVersion,
         "Local policy has an unexpected version");

    // Two different suffixes both carry it, and they remain different chains.
    const auto other = BuildPgnTestnetDefinition("phase2");
    need(other.pinned_genesis != local.pinned_genesis, "Two suffixes produced the same chain");
    const auto other_policy = ReadGoldCountsPolicyFromGenesis(other.genesis, kBudget);
    need(other_policy.has_value() && other_policy->has_value(), "Second suffix omits the rule");

    // A duplicated policy output is malformed, not merely redundant.
    auto doubled = local.genesis;
    const auto policy = BuildGoldCountsPolicyEnvelope(kBudget);
    need(policy.has_value(), "Policy envelope was rejected");
    CMutableTransaction coinbase{*local.genesis.vtx.front()};
    coinbase.vout.emplace_back(0, *policy);
    doubled.vtx[0] = MakeTransactionRef(coinbase);
    need(!ReadGoldCountsPolicyFromGenesis(doubled, kBudget).has_value(),
         "A duplicated policy was accepted");
}

int main()
{
    try {
        CheckEnvelope();
        CheckBounds();
        CheckBlockRead();
        CheckPayOnce();
        CheckActivation();
        std::cout << "gold counts envelope ok" << std::endl;
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "gold counts envelope failed: " << failure.what() << std::endl;
        return 1;
    }
}
