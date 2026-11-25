#undef NDEBUG
// Pay-once over authenticated history. The ledger alone only records claims; this
// fixture proves the rule actually refuses a proposed block that reuses a range
// number already on the branch, and that it stays out of the way on a chain whose
// genesis does not commit it.
//
// Closed fixture: synthetic blocks, an in-memory coins view and a temporary
// archive. No process, wallet, transport or GPU.
#include <pgn/candidate_block_science.h>
#include <pgn/gold_audit.h>
#include <pgn/gold_counts_history.h>
#include <pgn/native_bootstrap_hash.h>
#include <pgn/native_test_bip322.h>
#include <consensus/merkle.h>
#include <streams.h>
#include <txdb.h>
#include <util/strencodings.h>
#include <util/translation.h>

#include <array>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <vector>

extern const TranslateFn G_TRANSLATION_FUN = [](const char* text) { return std::string(text); };

using namespace pgn::core;

static void need(bool yes, const char* reason)
{
    if (!yes) throw std::runtime_error(reason);
}

namespace {

constexpr RangePolicyReadBudget kPolicyBudget{16, 4096, 16384, 8192};
constexpr uint64_t kMaximumScriptBytes = GoldCountsMaximumScriptBytes;

GoldPartitionCounts Counts(uint8_t base)
{
    GoldPartitionCounts counts{};
    for (size_t i = 0; i < GoldPartitionCount; ++i) counts[i] = static_cast<uint8_t>((base + i) % 5);
    return counts;
}

// A chain fixture: genesis policies, then one block per supplied declaration.
// The declaration lives in the header, so the index carries it and the replay
// needs no block bodies.
struct Fixture {
    uint256 chain;
    std::array<CBlock, 5> blocks;
    std::array<uint256, 5> hashes;
    std::vector<std::unique_ptr<CBlockIndex>> indices;
    std::unique_ptr<CCoinsViewDB> coins;
    std::unique_ptr<CCoinsViewCache> view;
    std::unique_ptr<NativeRequestHistoryArchive> history;

    Fixture(const fs::path& path, bool activate, const std::vector<std::optional<GoldCountsRecord>>& claims,
            const std::vector<std::vector<CScript>>& audits = {}, const std::string& name = {})
    {
        chain.begin()[0] = activate ? 21 : 22;
        // A deliberately narrow ticket width: each of the 100 partitions then
        // holds 50 odd positions, so a count of 255 is impossible and the
        // capacity bound is exercised rather than merely satisfied.
        const Integer lower{"150000000000000000000"};
        const RangePoolDefinition pool{lower, Integer{"10000"}, 2, 1, 10};
        const auto range_script = BuildCandidateRangePolicyEnvelope(pool, kPolicyBudget);
        need(range_script.has_value(), "Range policy envelope rejected");

        CMutableTransaction genesis;
        genesis.vin.resize(1);
        genesis.vin[0].prevout.SetNull();
        genesis.vin[0].scriptSig = CScript{} << 1 << OP_0;
        genesis.vout = {CTxOut{0, *range_script}};
        if (activate) {
            const auto policy = BuildGoldCountsPolicyEnvelope(kPolicyBudget);
            need(policy.has_value(), "Gold counts policy envelope rejected");
            genesis.vout.emplace_back(0, *policy);
        }
        blocks[0].nTime = 100;
        blocks[0].vtx = {MakeTransactionRef(genesis)};
        blocks[0].hashMerkleRoot = BlockMerkleRoot(blocks[0]);
        hashes[0] = blocks[0].GetHash();

        need(claims.size() <= blocks.size() - 1, "Fixture claim list too long");
        for (size_t height = 1; height <= claims.size(); ++height) {
            CMutableTransaction coinbase;
            coinbase.vin.resize(1);
            coinbase.vin[0].prevout.SetNull();
            coinbase.vin[0].scriptSig = CScript{} << int64_t(height) << OP_0;
            coinbase.vout.emplace_back(0, CScript{} << OP_RETURN << Bytes{'P', 'G', 'N'});
            if (height - 1 < audits.size()) {
                for (const auto& script : audits[height - 1]) coinbase.vout.emplace_back(0, script);
            }
            blocks[height].nTime = 200 + int64_t(height);
            blocks[height].hashPrevBlock = hashes[height - 1];
            blocks[height].nVersion = 4;
            if (claims[height - 1]) {
                blocks[height].nVersion |= PGN_GOLD_COUNTS_VERSION_BIT;
                const auto section =
                    EncodeGoldCountsHeader(claims[height - 1]->range_number, claims[height - 1]->counts);
                need(section.has_value(), "Fixture section rejected");
                blocks[height].gold_counts = *section;
            }
            blocks[height].vtx = {MakeTransactionRef(coinbase)};
            blocks[height].hashMerkleRoot = BlockMerkleRoot(blocks[height]);
            hashes[height] = blocks[height].GetHash();
        }
        for (size_t height = 0; height <= claims.size(); ++height) {
            auto index = std::make_unique<CBlockIndex>(blocks[height]);
            index->phashBlock = &hashes[height];
            index->nHeight = int32_t(height);
            index->nTx = 1;
            index->nStatus = BLOCK_VALID_SCRIPTS;
            index->pprev = height ? indices.back().get() : nullptr;
            index->BuildSkip();
            indices.push_back(std::move(index));
        }
        const auto stem = fs::PathFromString((activate ? "coins-on" : "coins-off") + name);
        coins = std::make_unique<CCoinsViewDB>(
            DBParams{.path = path / stem, .cache_bytes = 1 << 20,
                     .memory_only = true, .wipe_data = false}, CoinsViewOptions{});
        view = std::make_unique<CCoinsViewCache>(coins.get());
        view->SetBestBlock(hashes[claims.size()]);
        view->StagePgnFactCommit({{}, FactHead{chain, hashes[claims.size()]}, {}});
        history = std::make_unique<NativeRequestHistoryArchive>(
            path / fs::PathFromString((activate ? "history-on" : "history-off") + name), 1 << 20);
        const RequestHistoryArchiveBudget archive_budget{16384, 16384};
        for (size_t height = 0; height <= claims.size(); ++height) {
            need(history->StoreBlock(*view, *indices.back(), int32_t(height), blocks[height], archive_budget) ==
                     RequestArchiveVerdict::Stored,
                 "Whole native history was not stored");
        }
    }

    const CBlockIndex& tip() const { return *indices.back(); }
    const CBlockIndex& at(size_t height) const { return *indices[height]; }
};

RangeScheduleBudget Schedule()
{
    const TicketRequestBudget request_budget{128, 128, 4096};
    const ConfirmedRequestScanBudget scan{16, 4, 16384, 1, request_budget};
    return RangeScheduleBudget{8, 16, 4, RequestHistoryArchiveBudget{16384, 16384}, scan,
                               RangeDrawBudget{4, 1 << 16, 1 << 16}};
}

NativeBlockScienceBudget Science()
{
    return NativeBlockScienceBudget{kPolicyBudget, Schedule(), ScriptWorkBudget{4096, 8192}, 32, 16384, 32768};
}

CScript Envelope(uint64_t number, const GoldPartitionCounts& counts)
{
    const auto script = BuildGoldCountsEnvelope(number, counts);
    need(script.has_value(), "Fixture envelope rejected");
    return *script;
}

// A proposed block. `header` selects the counts section; `envelope` adds a
// coinbase copy, which an activated chain does not want unless it agrees.
CBlock Proposed(const CBlockIndex& parent, std::optional<GoldCountsRecord> header = {},
                std::vector<CScript> extra_outputs = {})
{
    CMutableTransaction coinbase;
    coinbase.vin.resize(1);
    coinbase.vin[0].prevout.SetNull();
    coinbase.vin[0].scriptSig = CScript{} << int64_t(parent.nHeight + 1) << OP_0;
    coinbase.vout.emplace_back(0, CScript{} << OP_RETURN << Bytes{'P', 'G', 'N'});
    for (const auto& script : extra_outputs) coinbase.vout.emplace_back(0, script);
    CBlock block;
    block.nTime = parent.GetBlockTime() + 60;
    block.hashPrevBlock = parent.GetBlockHash();
    block.nVersion = 4;
    if (header) {
        block.nVersion |= PGN_GOLD_COUNTS_VERSION_BIT;
        const auto section = EncodeGoldCountsHeader(header->range_number, header->counts);
        need(section.has_value(), "Proposed section rejected");
        block.gold_counts = *section;
    }
    block.nBits = 0x207fffff;
    block.nNonce = NativeBootstrapNonce(0);
    block.vtx = {MakeTransactionRef(coinbase)};
    block.hashMerkleRoot = BlockMerkleRoot(block);
    return block;
}

} // namespace

int main(int argc, char** argv)
{
    try {
        need(argc == 2, "Explicit private test folder required");
        const fs::path path = fs::PathFromString(argv[1]);
        LOCK(cs_main);
        const auto schedule = Schedule();

        // 0: genesis with the rule, 1: range 7, 2: plain, 3: range 8.
        const GoldCountsRecord seven{7, Counts(0)}, eight{8, Counts(1)};
        Fixture fixture{path, true, {seven, std::nullopt, eight}};
        need(fixture.tip().nHeight == 3, "Fixture height is wrong");
        need(fixture.at(1).HasGoldCounts() && fixture.at(1).gold_counts.size() == GoldCountsHeaderBytes,
             "The fixture block did not keep its section in the index");

        const auto policy = ReadChainGoldCountsPolicy(*fixture.view, fixture.tip(), *fixture.history,
                                                     schedule.archive, kPolicyBudget);
        need(policy.verdict == ChainRangePolicyVerdict::Confirmed && policy.policy.has_value(),
             "Activated chain did not confirm the gold counts policy");

        // The replay reads the block index, not block bodies: no archive, no
        // coinbase, and the claim is exactly the one the block id commits to.
        const auto ledger = ReadGoldCountsLedger(*fixture.view, fixture.tip(), schedule);
        need(ledger != nullptr, "Ledger replay was held");
        need(ledger->size() == 2 && ledger->Contains(7) && ledger->Contains(8) && !ledger->Contains(9),
             "Ledger replay found the wrong claims");
        need(ledger->Owner(7) == std::optional{std::pair{1, fixture.hashes[1]}}, "Range 7 has the wrong owner");

        // A claimed range number is refused, and the refusal names the record.
        const auto duplicate = VerifyProposedGoldCounts(*fixture.view, fixture.tip(), *fixture.history,
                                                        Proposed(fixture.tip(), seven), schedule, kPolicyBudget);
        need(duplicate.verdict == GoldCountsProposedVerdict::DuplicateRangeNumber,
             "A reused range number was not refused");
        need(duplicate.record && duplicate.record->range_number == 7, "The refusal lost its record");
        need(duplicate.record->counts == Counts(0), "The refusal lost the counts");

        // An envelope without a header has no committed home on an activated
        // chain, so it is not a lesser form of the same claim.
        const auto envelope_only = VerifyProposedGoldCounts(
            *fixture.view, fixture.tip(), *fixture.history,
            Proposed(fixture.tip(), std::nullopt, {Envelope(9, Counts(2))}), schedule, kPolicyBudget);
        need(envelope_only.verdict == GoldCountsProposedVerdict::InvalidDeclaration,
             "An envelope without a header was accepted on an activated chain");

        // A header that disagrees with a coinbase copy is a lie in one of them.
        const auto disagreeing = VerifyProposedGoldCounts(
            *fixture.view, fixture.tip(), *fixture.history,
            Proposed(fixture.tip(), GoldCountsRecord{9, Counts(2)}, {Envelope(10, Counts(3))}),
            schedule, kPolicyBudget);
        need(disagreeing.verdict == GoldCountsProposedVerdict::InvalidDeclaration,
             "A header that disagrees with the envelope was accepted");
        // Agreement is tolerated, so an older runtime's block is not reinterpreted.
        const auto agreeing = VerifyProposedGoldCounts(
            *fixture.view, fixture.tip(), *fixture.history,
            Proposed(fixture.tip(), GoldCountsRecord{9, Counts(2)}, {Envelope(9, Counts(2))}),
            schedule, kPolicyBudget);
        need(agreeing.verdict == GoldCountsProposedVerdict::Verified,
             "A header with a matching envelope was refused");

        // An unclaimed number is exactly what the rule is for.
        const auto fresh = VerifyProposedGoldCounts(*fixture.view, fixture.tip(), *fixture.history,
                                                    Proposed(fixture.tip(), GoldCountsRecord{9, Counts(2)}),
                                                    schedule, kPolicyBudget);
        need(fresh.verdict == GoldCountsProposedVerdict::Verified, "An unclaimed range number was refused");
        need(fresh.record && fresh.record->range_number == 9, "The verified record is wrong");

        // No declaration at all is unaffected.
        const auto plain = VerifyProposedGoldCounts(*fixture.view, fixture.tip(), *fixture.history,
                                                    Proposed(fixture.tip()), schedule, kPolicyBudget);
        need(plain.verdict == GoldCountsProposedVerdict::Absent, "A plain block was treated as a declaration");

        // Counts above a partition's capacity cannot come from a real scan.
        GoldPartitionCounts impossible{};
        impossible[0] = 255; // partition 0 owns 50 odd positions in this fixture
        const auto huge = VerifyProposedGoldCounts(*fixture.view, fixture.tip(), *fixture.history,
                                                   Proposed(fixture.tip(), GoldCountsRecord{11, impossible}),
                                                   schedule, kPolicyBudget);
        need(huge.verdict == GoldCountsProposedVerdict::InvalidDeclaration,
             "Counts above partition capacity were accepted");

        // The same declaration on a chain whose genesis does not commit the rule
        // is an ordinary zero-value OP_RETURN, which is today's testnet. A counts
        // header there is refused earlier, by header validation.
        Fixture dormant{path, false, {std::nullopt}};
        const auto dormant_policy = ReadChainGoldCountsPolicy(*dormant.view, dormant.tip(),
                                                              *dormant.history, schedule.archive, kPolicyBudget);
        need(dormant_policy.verdict == ChainRangePolicyVerdict::MissingConfiguration &&
                 !dormant_policy.policy.has_value(),
             "An inactive chain reported the rule");
        const auto inactive = VerifyProposedGoldCounts(
            *dormant.view, dormant.tip(), *dormant.history,
            Proposed(dormant.tip(), std::nullopt, {Envelope(7, Counts(0))}), schedule, kPolicyBudget);
        need(inactive.verdict == GoldCountsProposedVerdict::Inactive,
             "An inactive chain rejected a harmless envelope");
        // And it does not build a ledger it would never use.
        const auto dormant_ledger = ReadGoldCountsLedger(*dormant.view, dormant.tip(), schedule);
        need(dormant_ledger != nullptr, "Inactive chain ledger replay was held");

        // The audit ledger replays from coinbase outputs against the same
        // authenticated archive. A wrong answer is ignored rather than fatal: the
        // block already paid for its work, and the range must stay auditable.
        {
            const GoldCountsRecord audited{41, Counts(5)};
            // gold at 1, seed at 2, both answers after the seed.
            Fixture audited_chain{path, true, {audited, std::nullopt, std::nullopt}, {}, "-audit"};
            const auto audited_claims = ReadGoldCountsLedger(*audited_chain.view, audited_chain.at(3), schedule);
            need(audited_claims != nullptr, "Audited chain claims were held");
            const auto audited_target = ReadGoldAuditTarget(audited_chain.at(3), *audited_claims, 41);
            need(audited_target.has_value(), "Audited chain has no target");

            const auto answer = GoldAuditExpectedAnswer(*audited_target, 0);
            const auto envelope = BuildGoldAuditEnvelope(answer, 41);
            need(envelope.has_value(), "Audit envelope rejected");
            // Replaying the chain without the answer shows an unaudited range.
            const auto empty = ReadGoldAuditLedger(*audited_chain.view, audited_chain.at(3),
                                                  *audited_chain.history, schedule, *audited_claims,
                                                  kMaximumScriptBytes);
            need(empty != nullptr && !empty->Audited(41),
                 "A range with no answer is reported as audited");

            // The same chain with the answer in block 3 does count.
            Fixture with_answer{path, true, {audited, std::nullopt, std::nullopt},
                                {{}, {}, {*envelope}}, "-audit-answer"};
            const auto answer_claims = ReadGoldCountsLedger(*with_answer.view, with_answer.at(3), schedule);
            need(answer_claims != nullptr, "Answered chain claims were held");
            const auto audits = ReadGoldAuditLedger(*with_answer.view, with_answer.at(3),
                                                   *with_answer.history, schedule, *answer_claims,
                                                   kMaximumScriptBytes);
            need(audits != nullptr, "Audit replay was held");
            const auto* recorded = audits->Answers(41);
            need(recorded && (*recorded)[0], "The replayed audit lost its answer");
            need((*recorded)[0]->block == with_answer.hashes[3], "The answer is attributed to the wrong block");
            need(!audits->Audited(41), "One replayed answer completed the audit");

            // A wrong answer does not become an audit, and does not invalidate the
            // chain either.
            auto wrong = answer;
            wrong.counts[0] = static_cast<uint8_t>(wrong.counts[0] + 1);
            const auto wrong_envelope = BuildGoldAuditEnvelope(wrong, 41);
            need(wrong_envelope.has_value(), "Wrong audit envelope rejected");
            Fixture with_wrong{path, true, {audited, std::nullopt, std::nullopt},
                              {{}, {}, {*wrong_envelope}}, "-audit-wrong"};
            const auto wrong_claims = ReadGoldCountsLedger(*with_wrong.view, with_wrong.at(3), schedule);
            const auto wrong_audits = ReadGoldAuditLedger(*with_wrong.view, with_wrong.at(3),
                                                         *with_wrong.history, schedule, *wrong_claims,
                                                         kMaximumScriptBytes);
            need(wrong_audits != nullptr, "A wrong answer made the replay fail");
            need(wrong_audits->Answers(41) == nullptr, "A wrong answer was recorded");

            // An answer to a range that was never paid is not an audit at all.
            const auto orphan = BuildGoldAuditEnvelope(answer, 42);
            need(orphan.has_value(), "Orphan audit envelope rejected");
            Fixture with_orphan{path, true, {audited, std::nullopt, std::nullopt},
                               {{}, {}, {*orphan}}, "-audit-orphan"};
            const auto orphan_claims = ReadGoldCountsLedger(*with_orphan.view, with_orphan.at(3), schedule);
            const auto orphan_audits = ReadGoldAuditLedger(*with_orphan.view, with_orphan.at(3),
                                                          *with_orphan.history, schedule, *orphan_claims,
                                                          kMaximumScriptBytes);
            need(orphan_audits == nullptr, "An answer to an unpaid range was accepted");
        }

        // The science path verifies real gold blocks, so it must refuse a paid
        // range before it spends anything on the proof. A block with a science
        // envelope tag and a duplicate counts header never reaches the proof.
        {
            const auto science_tag = Bytes{BlockScienceEnvelopeTag.begin(), BlockScienceEnvelopeTag.end()};
            const CScript envelope = CScript{} << OP_RETURN << science_tag << Bytes{1, 2, 3};
            CBlock candidate = Proposed(fixture.tip(), seven);
            auto coinbase = CMutableTransaction{*candidate.vtx.front()};
            coinbase.vout.emplace_back(0, envelope);
            candidate.vtx = {MakeTransactionRef(coinbase)};
            candidate.hashMerkleRoot = BlockMerkleRoot(candidate);
            const auto checked = VerifyNativeCandidateBlockScience(*fixture.view, fixture.tip(),
                                                                  *fixture.history, candidate, Science());
            need(checked.verdict == NativeBlockScienceVerdict::GoldCountsRejected,
                 "The science path accepted a paid range number");
            // A fresh number gets past the gold gate and stops at the proof.
            CBlock fresh_science = Proposed(fixture.tip(), GoldCountsRecord{12, Counts(4)});
            auto fresh_coinbase = CMutableTransaction{*fresh_science.vtx.front()};
            fresh_coinbase.vout.emplace_back(0, envelope);
            fresh_science.vtx = {MakeTransactionRef(fresh_coinbase)};
            fresh_science.hashMerkleRoot = BlockMerkleRoot(fresh_science);
            const auto later = VerifyNativeCandidateBlockScience(*fixture.view, fixture.tip(),
                                                                *fixture.history, fresh_science, Science());
            need(later.verdict != NativeBlockScienceVerdict::GoldCountsRejected,
                 "The science path refused an unclaimed range number at the gold gate");
        }

        std::cout << R"({"PASS":true,"scope":"gold counts pay-once over authenticated history"})"
                  << std::endl;
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "gold counts history failed: " << failure.what() << std::endl;
        return 1;
    }
}
