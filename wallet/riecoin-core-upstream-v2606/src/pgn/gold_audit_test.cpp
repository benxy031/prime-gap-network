#undef NDEBUG
// The audit check under counts. Today audits compare an auditor's five counts
// against a revealed report; with the counts in the gold header the same check
// needs no report, no reveal and no stored expectation, so what is pinned here is
// the whole obligation: the right partitions, the right counts, and a selection
// the miner could not have known when it chose the header.
#include <pgn/gold_audit.h>

#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace pgn::core;

static void need(bool yes, const char* reason)
{
    if (!yes) throw std::runtime_error(reason);
}

namespace {

GoldPartitionCounts Counts(uint8_t base)
{
    GoldPartitionCounts counts{};
    for (size_t i = 0; i < GoldPartitionCount; ++i) counts[i] = static_cast<uint8_t>((base + i) % 11);
    return counts;
}

// A chain of `length` blocks whose index carries the counts section where asked,
// so the target reader sees exactly what a live node would.
struct Chain {
    std::vector<CBlock> blocks;
    std::vector<uint256> hashes;
    std::vector<std::unique_ptr<CBlockIndex>> indices;

    explicit Chain(size_t length, std::vector<std::pair<size_t, GoldCountsRecord>> claims)
    {
        blocks.resize(length);
        hashes.resize(length);
        for (size_t height = 0; height < length; ++height) {
            blocks[height].nTime = 1'700'000'000 + int64_t(height);
            blocks[height].nVersion = 4;
            for (const auto& [at, record] : claims) {
                if (at != height) continue;
                blocks[height].nVersion |= PGN_GOLD_COUNTS_VERSION_BIT;
                const auto section = EncodeGoldCountsHeader(record.range_number, record.counts);
                need(section.has_value(), "Fixture section rejected");
                blocks[height].gold_counts = *section;
            }
            if (height) blocks[height].hashPrevBlock = hashes[height - 1];
            hashes[height] = blocks[height].GetHash();
        }
        for (size_t height = 0; height < length; ++height) {
            auto index = std::make_unique<CBlockIndex>(blocks[height]);
            index->phashBlock = &hashes[height];
            index->nHeight = int32_t(height);
            index->nStatus = BLOCK_VALID_SCRIPTS;
            index->nTx = 1;
            index->pprev = height ? indices.back().get() : nullptr;
            index->BuildSkip();
            indices.push_back(std::move(index));
        }
    }

    const CBlockIndex& tip() const { return *indices.back(); }
};

GoldCountsLedger LedgerWith(const Chain& chain, std::vector<std::pair<size_t, GoldCountsRecord>> claims)
{
    GoldCountsLedger ledger;
    for (const auto& [height, record] : claims) {
        need(ledger.Add(int32_t(height), chain.hashes[height], record).verdict ==
                 GoldCountsVerdict::Accepted,
             "Fixture claim rejected");
    }
    return ledger;
}

} // namespace

static void CheckSeedOrdering()
{
    const GoldCountsRecord record{5, Counts(2)};
    Chain chain{6, {{2, record}}};
    const auto& tip = chain.tip();

    // The seed is the block after the gold block, not the gold block itself.
    const auto seed = GoldAuditSeed(tip, 2);
    need(seed.has_value(), "A confirmed gold block has no seed");
    need(*seed == chain.hashes[3], "The seed is not the block after the gold block");
    need(*seed != chain.hashes[2], "The seed is the gold block itself");
    need(*seed != chain.hashes[4], "The seed is not the immediate next block");

    // A gold block at the tip has no seed yet, so it is not auditable - the miner
    // would otherwise know the selection in advance.
    need(!GoldAuditSeed(*chain.indices[2], 2).has_value(),
         "A tip gold block was reported as auditable");
    need(!GoldAuditSeed(tip, 99).has_value(), "A future gold height produced a seed");
    need(!GoldAuditSeed(tip, -1).has_value(), "A negative gold height produced a seed");
}

static void CheckTarget()
{
    const GoldCountsRecord record{17, Counts(3)};
    Chain chain{8, {{3, record}}};
    const auto ledger = LedgerWith(chain, {{3, record}});

    const auto target = ReadGoldAuditTarget(chain.tip(), ledger, 17);
    need(target.has_value(), "A claimed range has no audit target");
    need(target->record == record, "The target lost the counts");
    need(target->gold_height == 3 && target->gold_block == chain.hashes[3],
         "The target points at the wrong block");
    need(target->seed == chain.hashes[4], "The target seed is wrong");
    need(GoldAuditSlotsDisjoint(*target), "The two slots share a partition");

    // Everything the miner could not know: a different seed selects different
    // partitions, so an answer prepared before the next block cannot be reused.
    const auto partitions = GoldAuditPartitions(record.counts, chain.hashes[4]);
    need(target->partitions == partitions, "The target did not use the counts selector");
    // A different seed selects different partitions, which is what stops a miner
    // from preparing the answer before the next block exists.
    need(GoldAuditPartitions(record.counts, chain.hashes[3]) != partitions,
         "The partition selector ignores the seed");

    need(!ReadGoldAuditTarget(chain.tip(), ledger, 18).has_value(),
         "An unclaimed range produced a target");

    // A claimed number whose block is not the ledger's block is not that claim.
    GoldCountsLedger foreign;
    need(foreign.Add(3, chain.hashes[5], record).verdict == GoldCountsVerdict::Accepted,
         "Fixture foreign claim rejected");
    need(!ReadGoldAuditTarget(chain.tip(), foreign, 17).has_value(),
         "A target was built from a ledger entry the chain does not confirm");
}

static void CheckAnswers()
{
    const GoldCountsRecord record{23, Counts(5)};
    Chain chain{9, {{4, record}}};
    const auto ledger = LedgerWith(chain, {{4, record}});
    const auto target = ReadGoldAuditTarget(chain.tip(), ledger, 23);
    need(target.has_value(), "Fixture target missing");

    // The honest answer verifies for both slots, and the two are different
    // assignments of different counts.
    const auto first = GoldAuditExpectedAnswer(*target, 0);
    const auto second = GoldAuditExpectedAnswer(*target, 1);
    need(VerifyGoldAuditAnswer(*target, first) == GoldAuditVerdict::Verified,
         "The expected answer for slot 0 was refused");
    need(VerifyGoldAuditAnswer(*target, second) == GoldAuditVerdict::Verified,
         "The expected answer for slot 1 was refused");
    need(!(first == second), "Both slots ask for the same answer");

    // A slot cannot be answered with the other slot's partitions.
    auto crossed = second;
    crossed.slot = 0;
    need(VerifyGoldAuditAnswer(*target, crossed) == GoldAuditVerdict::WrongPartitions,
         "Slot 1's partitions were accepted as slot 0");

    // One wrong count fails, and one repeated count does not pass by luck.
    auto wrong_count = first;
    wrong_count.counts[2] = static_cast<uint8_t>(wrong_count.counts[2] + 1);
    need(VerifyGoldAuditAnswer(*target, wrong_count) == GoldAuditVerdict::WrongCount,
         "A wrong count was accepted");
    auto wrong_partition = first;
    wrong_partition.partitions[0] = static_cast<uint32_t>(wrong_partition.partitions[0] + 1);
    need(VerifyGoldAuditAnswer(*target, wrong_partition) == GoldAuditVerdict::WrongPartitions,
         "A wrong partition index was accepted");

    // Structural failures are refused before any comparison.
    auto bad_slot = first;
    bad_slot.slot = 2;
    need(VerifyGoldAuditAnswer(*target, bad_slot) == GoldAuditVerdict::InvalidSlot,
         "Slot 2 was accepted");
    auto unsorted = first;
    std::swap(unsorted.partitions[0], unsorted.partitions[4]);
    need(VerifyGoldAuditAnswer(*target, unsorted) == GoldAuditVerdict::WrongPartitions,
         "An unsorted assignment was accepted");
    auto out_of_range = first;
    out_of_range.partitions[4] = 100;
    need(VerifyGoldAuditAnswer(*target, out_of_range) == GoldAuditVerdict::WrongPartitions,
         "Partition 100 was accepted");

    // A miner that rewrites the counts after the fact would also have to rewrite
    // the block id, and then the seed block's ancestry; the check reads the
    // counts from the index, so a ledger entry disagreeing with it is refused.
    const GoldCountsRecord forged{23, Counts(6)};
    Chain forged_chain{9, {{4, forged}}};
    const auto forged_ledger = LedgerWith(forged_chain, {{4, forged}});
    const auto forged_target = ReadGoldAuditTarget(forged_chain.tip(), forged_ledger, 23);
    need(forged_target.has_value(), "Forged fixture target missing");
    // The honest answer for the real counts cannot verify against a header that
    // declares different ones - the selection itself moves, so it fails before
    // the counts are even compared.
    need(VerifyGoldAuditAnswer(*forged_target, first) != GoldAuditVerdict::Verified,
         "An answer for the real counts verified against a different header");
    // And whatever the header says is the authority: the check has no memory of
    // an earlier declaration and no revealed expectation to compare against.
    need(VerifyGoldAuditAnswer(*forged_target, GoldAuditExpectedAnswer(*forged_target, 0)) ==
             GoldAuditVerdict::Verified,
         "The forged header's own answer did not verify");
}

static void CheckEnvelope()
{
    const GoldAuditAnswer answer{1, {12, 40, 41, 77, 99}, {0, 1, 2, 3, 4}};
    const auto script = BuildGoldAuditEnvelope(answer, 4242);
    need(script.has_value(), "Canonical audit envelope was rejected");
    need(IsGoldAuditScript(*script), "The envelope is not recognised as a counts audit");
    // OP_RETURN, the tag push, then the payload push: a direct push while the
    // payload stays under 76 bytes, one length byte more above it.
    need(GoldAuditPayloadBytes == 18, "The payload size changed");
    need(script->size() == 1 + 1 + GoldAuditTag.size() + 1 + GoldAuditPayloadBytes,
         "The envelope is not the canonical size");

    const auto decoded = DecodeGoldAuditEnvelope(*script);
    need(decoded.has_value() && decoded->range_number == 4242 && decoded->answer == answer,
         "The envelope does not round-trip");

    // An unsorted or out-of-domain assignment is not an answer that could have
    // been handed out, so it is refused at the codec rather than at the ledger.
    GoldAuditAnswer unsorted{0, {40, 12, 41, 77, 99}, {0, 0, 0, 0, 0}};
    need(!BuildGoldAuditEnvelope(unsorted, 1).has_value(), "An unsorted assignment was encoded");
    GoldAuditAnswer out_of_range{0, {0, 1, 2, 3, 100}, {0, 0, 0, 0, 0}};
    need(!BuildGoldAuditEnvelope(out_of_range, 1).has_value(), "Partition 100 was encoded");
    GoldAuditAnswer bad_slot{2, {0, 1, 2, 3, 99}, {0, 0, 0, 0, 0}};
    need(!BuildGoldAuditEnvelope(bad_slot, 1).has_value(), "Slot 2 was encoded");
    need(!BuildGoldAuditEnvelope(answer, uint64_t{1} << 48).has_value(), "An out-of-domain range was encoded");

    // Trailing bytes and foreign scripts.
    CScript trailing = *script;
    trailing << OP_0;
    need(!DecodeGoldAuditEnvelope(trailing).has_value(), "Trailing bytes were decoded");
    need(!DecodeGoldAuditEnvelope(CScript{} << OP_RETURN << Bytes{'n', 'o'}).has_value(),
         "A foreign tag was decoded");
    need(!DecodeGoldAuditEnvelope(CScript{}).has_value(), "An empty script was decoded");
}

static void CheckLedger()
{
    const GoldCountsRecord record{31, Counts(7)};
    Chain chain{9, {{4, record}}};
    const auto ledger_claims = LedgerWith(chain, {{4, record}});
    const auto target = ReadGoldAuditTarget(chain.tip(), ledger_claims, 31);
    need(target.has_value(), "Fixture target missing");

    const CScript first{CScript{} << OP_1}, second{CScript{} << OP_2};
    GoldAuditLedger audits;
    need(!audits.Audited(31), "A fresh range is reported as audited");

    // One verified answer per slot, one answer per slot ever.
    const auto zero = GoldAuditExpectedAnswer(*target, 0);
    need(audits.Add(5, *target, first, zero) == GoldAuditAdmission::Accepted,
         "A verified answer was refused");
    need(!audits.Audited(31), "One slot is reported as a completed audit");
    need(audits.Add(5, *target, first, zero) == GoldAuditAdmission::Replay,
         "The identical answer was treated as a new claim");
    need(audits.Add(6, *target, second, zero) == GoldAuditAdmission::SlotTaken,
         "A second actor overwrote an answered slot");

    // A wrong answer never consumes a slot: otherwise anyone could burn the range
    // by answering badly. Slot 1 is still free here.
    const auto one = GoldAuditExpectedAnswer(*target, 1);
    auto wrong = one;
    wrong.counts[0] = static_cast<uint8_t>(wrong.counts[0] + 1);
    need(audits.Add(7, *target, second, wrong) == GoldAuditAdmission::NotVerified,
         "A wrong answer was accepted");
    need(audits.Add(7, *target, second, one) == GoldAuditAdmission::Accepted,
         "A wrong answer consumed the slot");
    need(audits.Audited(31), "Two verified slots did not complete the audit");
    need(audits.AuditedRanges() == 1, "Audited range count is wrong");

    // Slot 0 cannot be answered with slot 1's assignment, and the ledger only
    // grows forward.
    GoldAuditLedger other_rules;
    auto crossed_answer = GoldAuditExpectedAnswer(*target, 1);
    crossed_answer.slot = 0;
    need(other_rules.Add(5, *target, first, crossed_answer) == GoldAuditAdmission::NotVerified,
         "A crossed assignment was recorded");
    need(other_rules.Add(5, *target, first, zero) == GoldAuditAdmission::Accepted,
         "The correct slot 0 answer was refused after a crossed one");
    need(other_rules.Add(4, *target, second, GoldAuditExpectedAnswer(*target, 1)) ==
             GoldAuditAdmission::OutOfOrder,
         "An older answer was accepted");

    // The answers are kept as submitted, so a reader can see who audited what.
    const auto* answers = audits.Answers(31);
    need(answers && (*answers)[0] && (*answers)[0]->actor == first && (*answers)[0]->height == 5,
         "The recorded slot lost its actor or height");
    need(answers && (*answers)[1] && (*answers)[1]->actor == second && (*answers)[1]->height == 7,
         "The second slot recorded the wrong actor");
    need(audits.Answers(32) == nullptr, "An unanswered range has answers");
}

int main()
{

    try {
        CheckSeedOrdering();
        CheckTarget();
        CheckAnswers();
        CheckEnvelope();
        CheckLedger();
        std::cout << "gold audit ok" << std::endl;
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "gold audit failed: " << failure.what() << std::endl;
        return 1;
    }
}
