#undef NDEBUG
// The audit check under counts. Today audits compare an auditor's five counts
// against a revealed report; with the counts in the gold header the same check
// needs no report, no reveal and no stored expectation, so what is pinned here is
// the whole obligation: the right partitions, the right counts, and a selection
// the miner could not have known when it chose the header.
#include <pgn/gold_audit.h>
#include <pgn/gold_payout.h>
#include <pgn/gold_settlement.h>

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
    const CScript auditor = CScript{} << OP_1;
    const GoldAuditAnswer answer{1, {12, 40, 41, 77, 99}, {0, 1, 2, 3, 4}, auditor};
    const auto script = BuildGoldAuditEnvelope(answer, 4242);
    need(script.has_value(), "Canonical audit envelope was rejected");
    need(IsGoldAuditScript(*script), "The envelope is not recognised as a counts audit");
    need(GoldAuditPayloadBytes == 18, "The payload size changed");
    // The version two payload adds a length byte and the payee script.
    need(script->size() == 1 + 1 + GoldAuditTag.size() + 1 + GoldAuditPayloadBytes +
                              GoldRewardLengthBytes + auditor.size(),
         "The envelope is not the canonical size");

    const auto decoded = DecodeGoldAuditEnvelope(*script);
    need(decoded.has_value() && decoded->range_number == 4242 && decoded->answer == answer,
         "The envelope does not round-trip");

    // An unsorted or out-of-domain assignment is not an answer that could have
    // been handed out, so it is refused at the codec rather than at the ledger.
    GoldAuditAnswer unsorted{0, {40, 12, 41, 77, 99}, {0, 0, 0, 0, 0}, auditor};
    need(!BuildGoldAuditEnvelope(unsorted, 1).has_value(), "An unsorted assignment was encoded");
    GoldAuditAnswer out_of_range{0, {0, 1, 2, 3, 100}, {0, 0, 0, 0, 0}, auditor};
    need(!BuildGoldAuditEnvelope(out_of_range, 1).has_value(), "Partition 100 was encoded");
    GoldAuditAnswer bad_slot{2, {0, 1, 2, 3, 99}, {0, 0, 0, 0, 0}, auditor};
    need(!BuildGoldAuditEnvelope(bad_slot, 1).has_value(), "Slot 2 was encoded");
    // An answer with no payee could never be settled, so it is not an answer.
    GoldAuditAnswer unpaid{0, {0, 1, 2, 3, 99}, {0, 0, 0, 0, 0}, CScript{}};
    need(!BuildGoldAuditEnvelope(unpaid, 1).has_value(), "An answer with no payee was encoded");
    GoldAuditAnswer overdrawn{0, {0, 1, 2, 3, 99}, {0, 0, 0, 0, 0}, CScript{}};
    for (size_t i = 0; i < GoldRewardMaximumScriptBytes + 1; ++i) overdrawn.reward << OP_0;
    need(!BuildGoldAuditEnvelope(overdrawn, 1).has_value(), "An oversized payee was encoded");
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

    const auto first = chain.hashes[6], second = chain.hashes[7];
    GoldAuditLedger audits;
    need(!audits.Audited(31), "A fresh range is reported as audited");

    // One verified answer per slot, one answer per slot ever.
    const auto zero = GoldAuditExpectedAnswer(*target, 0);
    need(audits.Add(5, first, *target, zero) == GoldAuditAdmission::Accepted,
         "A verified answer was refused");
    need(!audits.Audited(31), "One slot is reported as a completed audit");
    need(audits.Add(5, first, *target, zero) == GoldAuditAdmission::Replay,
         "The identical answer was treated as a new claim");
    need(audits.Add(6, second, *target, zero) == GoldAuditAdmission::SlotTaken,
         "A second actor overwrote an answered slot");

    // A wrong answer never consumes a slot: otherwise anyone could burn the range
    // by answering badly. Slot 1 is still free here.
    const auto one = GoldAuditExpectedAnswer(*target, 1);
    auto wrong = one;
    wrong.counts[0] = static_cast<uint8_t>(wrong.counts[0] + 1);
    need(audits.Add(7, second, *target, wrong) == GoldAuditAdmission::NotVerified,
         "A wrong answer was accepted");
    need(audits.Add(7, second, *target, one) == GoldAuditAdmission::Accepted,
         "A wrong answer consumed the slot");
    need(audits.Audited(31), "Two verified slots did not complete the audit");
    need(audits.AuditedRanges() == 1, "Audited range count is wrong");

    // Slot 0 cannot be answered with slot 1's assignment, and the ledger only
    // grows forward.
    GoldAuditLedger other_rules;
    auto crossed_answer = GoldAuditExpectedAnswer(*target, 1);
    crossed_answer.slot = 0;
    need(other_rules.Add(5, first, *target, crossed_answer) == GoldAuditAdmission::NotVerified,
         "A crossed assignment was recorded");
    // A refused answer must not leave a trace, or every reader would see the range
    // as answered by a block that answered nothing.
    need(other_rules.Answers(31) == nullptr, "A refused answer created a ledger entry");
    need(other_rules.Add(5, first, *target, zero) == GoldAuditAdmission::Accepted,
         "The correct slot 0 answer was refused after a crossed one");
    need(other_rules.Add(4, second, *target, GoldAuditExpectedAnswer(*target, 1)) ==
             GoldAuditAdmission::OutOfOrder,
         "An older answer was accepted");

    // The answers are kept as submitted, so a reader can see which block answered.
    const auto* answers = audits.Answers(31);
    need(answers && (*answers)[0] && (*answers)[0]->block == first && (*answers)[0]->height == 5,
         "The recorded slot lost its block or height");
    need(answers && (*answers)[1] && (*answers)[1]->block == second && (*answers)[1]->height == 7,
         "The second slot recorded the wrong block");
    need(audits.Answers(32) == nullptr, "An unanswered range has answers");
}

static void CheckWindow()
{
    const GoldCountsRecord record{17, Counts(3)};
    Chain chain{8, {{3, record}}};
    const auto ledger_claims = LedgerWith(chain, {{3, record}});
    const auto target = [&] {
        const auto found = ReadGoldAuditTarget(chain.tip(), ledger_claims, 17);
        need(found.has_value(), "Window target missing");
        return *found;
    }();

    // The deadline is counted from the seed, so it does not move with the tip.
    const auto deadline = GoldAuditDeadline(target);
    need(deadline == target.gold_height + 1 + GoldAuditWindowBlocks, "The deadline arithmetic moved");

    // Nothing answered: open while a block can still land inside the window, expired
    // from the moment the tip reaches the deadline, because the next block is past it.
    const GoldAuditLedger empty;
    need(GoldAuditRangeState(empty, target, deadline - 1).status == GoldAuditStatus::Open,
         "An unanswered range expired before its deadline");
    need(GoldAuditRangeState(empty, target, deadline).status == GoldAuditStatus::Expired,
         "An unanswered range did not expire at its deadline");

    // One answer is still incomplete, so the range must not read as audited.
    GoldAuditLedger half;
    need(half.Add(5, chain.hashes[5], target, GoldAuditExpectedAnswer(target, 0)) ==
             GoldAuditAdmission::Accepted,
         "The window fixture lost its first answer");
    const auto partial = GoldAuditRangeState(half, target, deadline + 1);
    need(partial.status == GoldAuditStatus::Expired && partial.answered == 1,
         "A half-answered range did not report expired with one answer");

    // Both answers complete the audit, and completion is not undone by the clock.
    need(half.Add(6, chain.hashes[6], target, GoldAuditExpectedAnswer(target, 1)) ==
             GoldAuditAdmission::Accepted,
         "The window fixture lost its second answer");
    const auto complete = GoldAuditRangeState(half, target, deadline + 1);
    need(complete.status == GoldAuditStatus::Audited && complete.answered == 2,
         "A completed audit was reported expired");
    need(complete.deadline == deadline, "The reported deadline is wrong");
}

static void CheckPayout()
{
    // The local chain's committed rewards: the miner's share and a pot that is
    // exactly a quarter of it, inside the same 247.5 per gold.
    const CAmount coin = PGN_ATOMIC_UNITS_PER_COIN;
    GoldEpochDefinition epochs{200000, 10, coin * 1468 / 10, 32 * coin, 24 * coin, 8 * coin,
                               coin * 367, 1000 * coin, 40U * 65536U};
    need(GoldEpochDefinitionValid(epochs), "The payout fixture policy is invalid");

    const GoldCountsRecord record{17, Counts(3)};
    Chain chain{8, {{3, record}}};
    const auto claims = LedgerWith(chain, {{3, record}});
    const auto target = ReadGoldAuditTarget(chain.tip(), claims, 17);
    need(target.has_value(), "Payout target missing");

    const CScript miner = CScript{} << OP_1, first = CScript{} << OP_2, second = CScript{} << OP_3;
    GoldAuditLedger audits;
    const auto answer = [&](uint8_t slot, const CScript& payee) {
        auto value = GoldAuditExpectedAnswer(*target, slot, payee);
        need(audits.Add(6 + slot, chain.hashes[6 + slot], *target, value) ==
                 GoldAuditAdmission::Accepted,
             "The payout fixture lost its answer");
    };

    // Nothing answered: the pot is known, the miner is known, nobody is paid.
    const auto open = QuoteGoldRangePayout(epochs, 0, miner, audits, 17);
    need(open.has_value(), "An open range has no payout quote");
    need(open->miner == coin * 1468 / 10, "The miner's share is wrong");
    need(open->audit_pot * 4 == open->miner, "The auditor's pot is not a quarter of the miner");
    need(open->gross == coin * 2475 / 10, "The grant total is wrong");
    need(!open->payable && !open->auditors[0] && !open->auditors[1],
         "An unanswered range reported a payout");

    // One slot is still not a completed audit, and the answered slot's payee is
    // already visible so a page can be checked against it.
    answer(0, first);
    const auto half = QuoteGoldRangePayout(epochs, 0, miner, audits, 17);
    need(half.has_value() && !half->payable, "A half-answered range reported payable");
    need(half->auditors[0] && half->auditors[0]->script == first,
         "The first slot lost its payee");
    need(!half->auditors[1], "The unanswered slot reported a share");

    // Both slots: the pot divides exactly, with no atomic unit lost.
    answer(1, second);
    const auto complete = QuoteGoldRangePayout(epochs, 0, miner, audits, 17);
    need(complete.has_value() && complete->payable, "A completed audit is not payable");
    need(complete->auditors[0] && complete->auditors[1], "A completed audit lost a slot");
    need(complete->auditors[0]->script == first && complete->auditors[1]->script == second,
         "The auditors' payees are crossed");
    const auto paid = complete->auditors[0]->amount + complete->auditors[1]->amount;
    need(paid == complete->audit_pot, "The slots did not divide the pot exactly");
    need(paid * 4 == complete->miner, "The auditors together did not get a quarter");

    // The rule holds in every era, because halving applies to both sides.
    const auto later = QuoteGoldRangePayout(epochs, epochs.era_golds, miner, audits, 17);
    need(later.has_value(), "An era one quote failed");
    need(later->audit_pot * 4 == later->miner, "The quarter drifted in a later era");

    // A chain whose committed split is not a quarter is refused rather than paid
    // something else.
    auto wrong = epochs;
    wrong.audit_period_amount = coin * 125;
    need(GoldEpochDefinitionValid(wrong), "The negative fixture policy is invalid");
    need(!QuoteGoldRangePayout(wrong, 0, miner, audits, 17).has_value(),
         "A chain that does not commit the quarter produced a payout");

    // And a range with no payee for its miner cannot be quoted at all.
    need(!QuoteGoldRangePayout(epochs, 0, CScript{}, audits, 17).has_value(),
         "A range with no miner payee produced a payout");

    // Correctness must never depend on the payee: the verifier answers "are these
    // counts right", while only the codec decides what may enter a block. Getting
    // this wrong made every honest answer look forged once the payee was added.
    const auto anonymous = GoldAuditExpectedAnswer(*target, 0, CScript{});
    need(VerifyGoldAuditAnswer(*target, anonymous) == GoldAuditVerdict::Verified,
         "Correctness was made to depend on the payee");
    need(!BuildGoldAuditEnvelope(anonymous, 17).has_value(),
         "An answer with no payee was encoded");
}

static void CheckRelease()
{
    const CAmount coin = PGN_ATOMIC_UNITS_PER_COIN;
    const GoldEpochDefinition epochs{200000, 10, coin * 1468 / 10, 32 * coin, 24 * coin, 8 * coin,
                                     coin * 367, 1000 * coin, 40U * 65536U};

    // Two ranges paid at heights 1 and 2, both audited, but the second one completes
    // first. The release order has to follow completion and not mining.
    const GoldCountsRecord early{17, Counts(3)}, late{18, Counts(4)};
    Chain chain{8, {{3, early}}};
    GoldCountsLedger claims;
    need(claims.Add(1, chain.hashes[1], early).verdict == GoldCountsVerdict::Accepted,
         "Release fixture claim one");
    need(claims.Add(2, chain.hashes[2], late).verdict == GoldCountsVerdict::Accepted,
         "Release fixture claim two");
    need(claims.GoldsBefore(2) == 1, "The gold ordinal is wrong");

    GoldAuditLedger audits;
    const auto target_for = [&](const GoldCountsRecord& record) {
        GoldAuditTarget built;
        built.record = record;
        built.gold_height = 1;
        built.seed = chain.hashes[2];
        for (size_t slot = 0; slot < GoldAuditSlots; ++slot) {
            for (size_t i = 0; i < GoldAuditPartitionsPerSlot; ++i) {
                built.partitions[slot][i] = uint32_t(14 * slot + i);
            }
        }
        return built;
    };
    const auto sign = [&](const GoldCountsRecord& record, uint8_t slot, int32_t height,
                          const CScript& payee) {
        // One answer per height: the ledger only grows forward, exactly as the
        // replay enforces.
        const auto target = target_for(record);
        need(audits.Add(height, chain.hashes[size_t(height)], target,
                        GoldAuditExpectedAnswer(target, slot, payee)) ==
                 GoldAuditAdmission::Accepted,
             "Release fixture answer");
    };
    sign(late, 0, 3, CScript{} << OP_4);
    sign(late, 1, 4, CScript{} << OP_5);
    sign(early, 0, 5, CScript{} << OP_2);
    sign(early, 1, 6, CScript{} << OP_3);

    const auto complete = GoldCompletedAudits(claims, audits);
    need(complete.size() == 2, "Both audited ranges were not complete");
    // Range 18 finished at height 4, range 17 at height 5, so 18 is released first.
    need(complete[0].range_number == 18 && complete[0].completion_height == 4,
         "The release order does not follow completion");
    need(complete[1].range_number == 17 && complete[1].completion_height == 6,
         "The release order does not follow completion");

    const auto script_at = [](int32_t) { return std::optional<CScript>{CScript{} << OP_1}; };

    // The cursor starts at zero and nothing is released before the answers are behind
    // the parent.
    need(PlanGoldRangeReleases(epochs, claims, audits, 0, 3, script_at)->empty(),
         "A release happened in the block carrying the answer");
    const auto first = PlanGoldRangeReleases(epochs, claims, audits, 0, 4, script_at);
    need(first && first->size() == 1 && (*first)[0].range_number == 18,
         "The first release is wrong");
    // The whole pot for one gold: 367 coin accumulated over an audit period of ten.
    need((*first)[0].total == coin * 367 / 10, "The released amount is not the pot");
    need((*first)[0].total * 4 == coin * 1468 / 10, "The released pot is not a quarter of the miner");
    need((*first)[0].shares[0] && (*first)[0].shares[1], "A release lost a share");

    // One block may release everything that is ready, and the cursor then holds.
    const auto both = PlanGoldRangeReleases(epochs, claims, audits, 0, 6, script_at);
    need(both && both->size() == 2, "A block did not release both ready ranges");
    need((*both)[0].range_number == 18 && (*both)[1].range_number == 17,
         "The batch is out of order");
    const auto after = PlanGoldRangeReleases(epochs, claims, audits, 2, 6, script_at);
    need(after && after->empty(), "The cursor released a range twice");

    // An unreadable gold body refuses the plan rather than skipping the range and
    // paying the ones behind it.
    const auto missing = PlanGoldRangeReleases(epochs, claims, audits, 0, 6,
                                               [](int32_t) { return std::optional<CScript>{}; });
    need(!missing.has_value(), "An unreadable body skipped a release");

    // A range nobody audited never enters the order at all, so it cannot stall the
    // ranges that were audited.
    GoldCountsLedger one_more = claims;
    need(one_more.Add(3, chain.hashes[3], GoldCountsRecord{19, Counts(1)}).verdict ==
             GoldCountsVerdict::Accepted,
         "The un-audited fixture claim failed");
    const auto unaffected = PlanGoldRangeReleases(epochs, one_more, audits, 0, 6, script_at);
    need(unaffected && unaffected->size() == 2, "An unaudited range entered the release order");
}

int main()
{

    try {
        CheckSeedOrdering();
        CheckTarget();
        CheckAnswers();
        CheckEnvelope();
        CheckLedger();
        CheckWindow();
        CheckPayout();
        CheckRelease();
        std::cout << "gold audit ok" << std::endl;
        return 0;
    } catch (const std::exception& failure) {
        std::cerr << "gold audit failed: " << failure.what() << std::endl;
        return 1;
    }
}
