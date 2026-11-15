// Copyright (c) 2026 The PGN developers. Distributed under the MIT license.
// Private integration candidate. No selected network or consensus activation.
#pragma once
#include <pgn/candidate_block_science.h>
#include <pgn/native_report_audit.h>
#include <pgn/certified_gap_encoding.h>
#include <kernel/cs_main.h>
#include <set>

namespace pgn::core {
enum class TicketGoldGroupVerdict {
    VerifiedGroup, Empty, BudgetExceeded, DifferentTicketOrReport,
    HistoryUnavailable, AuditsIncomplete, InvalidEvidence, NoncanonicalOrder,
    DuplicateGold
};
struct TicketGoldGroupCheck;
class VerifiedNativeTicketGoldGroup {
    uint256 m_parent, m_request;
    Digest m_report;
    ChainRangePolicy m_ranges;
    ChainSciencePolicy m_policy;
    std::vector<VerifiedNativeProfiledScience> m_golds;
    std::vector<Integer> m_lower_primes;
    VerifiedNativeTicketGoldGroup(uint256 parent, uint256 request, Digest report,
        ChainRangePolicy ranges, ChainSciencePolicy policy,
        std::vector<VerifiedNativeProfiledScience> golds, std::vector<Integer> primes)
        :m_parent(parent),m_request(request),m_report(report),m_ranges(ranges),
         m_policy(policy),m_golds(std::move(golds)),m_lower_primes(std::move(primes)) {}
    friend TicketGoldGroupCheck VerifyNativeTicketGoldGroup(const CCoinsView&,
        const CBlockIndex&,const NativeRequestHistoryArchive&,
        std::span<const CandidateBlockScienceProof>,const NativeBlockScienceBudget&);
    friend FactHistoryStep PrepareNativeTicketGoldGroupForward(const CCoinsView&,
        const CBlockIndex&,const CBlockIndex&,const VerifiedNativeTicketGoldGroup&);
public:
    VerifiedNativeTicketGoldGroup(const VerifiedNativeTicketGoldGroup&)=default;
    const uint256& Request()const{return m_request;}
    const Digest& ReportRoot()const{return m_report;}
    size_t GoldCount()const{return m_golds.size();}
    // Closure i belongs to Gold i. Rosettes belong to closure zero only.
    // This is ordering metadata, never a payment authorization.
    const std::vector<Integer>& LowerPrimes()const{return m_lower_primes;}
    const std::vector<VerifiedNativeProfiledScience>& Golds()const{return m_golds;}
};
struct TicketGoldGroupCheck {
    TicketGoldGroupVerdict verdict;
    std::optional<VerifiedNativeTicketGoldGroup> group;
};

// Verifies all supplied Golds from ONE ticket/report at ONE authentic parent.
// Shared range/policy/audit reads occur once; exact mathematics and owner
// authorization remain mandatory for every supplied Gold. Does not establish
// that the supplied list contains every Gold in the complete report, fresh PoW,
// block-body/transaction validity, monetary settlement or public consensus.
inline std::optional<TicketGoldGroupVerdict> CheckTicketGoldGroupEncoding(
    std::span<const CandidateBlockScienceProof> proofs,const NativeBlockScienceBudget& budget)
{
    if(proofs.empty())return TicketGoldGroupVerdict::Empty;
    if(!budget.maximum_coinbase_outputs||proofs.size()>budget.maximum_coinbase_outputs||
       !budget.maximum_payload_bytes||!budget.signature.signature_bytes)
        return TicketGoldGroupVerdict::BudgetExceeded;
    const auto& first=proofs.front();
    size_t remaining=budget.maximum_payload_bytes;
    for(const auto& proof:proofs){
        if(proof.request!=first.request||proof.report_root!=first.report_root)
            return TicketGoldGroupVerdict::DifferentTicketOrReport;
        if(proof.signature.empty()||proof.signature.size()>budget.signature.signature_bytes||
           proof.evidence.inclusion.siblings.size()>32)
            return TicketGoldGroupVerdict::BudgetExceeded;
        for(const auto size:{proof.signature.size(),proof.evidence.segment_bytes.size(),
                            proof.evidence.scientific_bytes.size(),
                            proof.evidence.inclusion.siblings.size()*size_t{32}}){
            if(size>remaining)return TicketGoldGroupVerdict::BudgetExceeded;
            remaining-=size;
        }
    }
    return {};
}
inline TicketGoldGroupCheck VerifyNativeTicketGoldGroup(const CCoinsView& view,
    const CBlockIndex& parent,const NativeRequestHistoryArchive& history,
    std::span<const CandidateBlockScienceProof> proofs,const NativeBlockScienceBudget& budget)
{
    AssertLockHeld(cs_main);
    if(const auto error=CheckTicketGoldGroupEncoding(proofs,budget))return {*error,{}};
    const auto& first=proofs.front();
    const auto ranges=ReadChainRangePolicy(view,parent,history,budget.schedule.archive,budget.policy);
    const auto policy=ReadChainSciencePolicy(view,parent,history,budget.schedule.archive,budget.policy,&budget.schedule,&budget.signature);
    if(!ranges.policy||!policy.policy)return {TicketGoldGroupVerdict::HistoryUnavailable,{}};
    const auto reserved=DeriveNativeCandidateReservation(view,parent,*ranges.policy,history,budget.schedule,first.request);
    if(!reserved.reservation)return {TicketGoldGroupVerdict::HistoryUnavailable,{}};
    const auto audits=ReadNativeAuditLedger(view,parent,history,budget.policy,budget.schedule,budget.signature);
    if(!audits)return {TicketGoldGroupVerdict::HistoryUnavailable,{}};
    const auto found=audits->find(first.request);
    if(found==audits->end()||!NativeAuditReportQualified(*audits,found->second)||
       found->second.details_root!=std::optional{first.report_root})
        return {TicketGoldGroupVerdict::AuditsIncomplete,{}};
    const auto context=NativeProfiledWorkContext(view,parent,*ranges.policy,*policy.policy,*reserved.reservation,first.report_root);
    if(!context)return {TicketGoldGroupVerdict::HistoryUnavailable,{}};
    std::vector<VerifiedNativeProfiledScience> golds;
    std::vector<Integer> lower_primes;
    std::set<uint256> seen;
    golds.reserve(proofs.size());lower_primes.reserve(proofs.size());
    for(const auto& proof:proofs){
        auto checked=VerifyNativeProfiledScience(view,parent,*ranges.policy,*policy.policy,
            *reserved.reservation,first.report_root,proof.signature,proof.evidence,budget.signature);
        if(!checked.candidate)return {TicketGoldGroupVerdict::InvalidEvidence,{}};
        const auto& key=checked.candidate->Science().Science().Key();
        if(!seen.insert(key).second)return {TicketGoldGroupVerdict::DuplicateGold,{}};
        try{
            const auto decoded=decodeCertifiedGap(proof.evidence.scientific_bytes,
                {context->proof_encoding,context->policy.scientific});
            if(!lower_primes.empty()&&decoded.lower<=lower_primes.back())
                return {TicketGoldGroupVerdict::NoncanonicalOrder,{}};
            lower_primes.push_back(decoded.lower);
        }catch(const std::invalid_argument&){return {TicketGoldGroupVerdict::InvalidEvidence,{}};}
        golds.push_back(*checked.candidate);
    }
    return {TicketGoldGroupVerdict::VerifiedGroup,VerifiedNativeTicketGoldGroup{
        parent.GetBlockHash(),first.request,first.report_root,*ranges.policy,*policy.policy,
        std::move(golds),std::move(lower_primes)}};
}

// Prepare ONE atomic native fact commit for the whole ticket's Gold group.
// ConnectBlock must first validate the actual proposed body and exact payment
// plan. This function cannot accept a block, mutate a view or mint currency.
inline FactHistoryStep PrepareNativeTicketGoldGroupForward(const CCoinsView& view,
    const CBlockIndex& parent,const CBlockIndex& proposed,const VerifiedNativeTicketGoldGroup& group)
{
    AssertLockHeld(cs_main);
    if(group.m_parent!=parent.GetBlockHash()||group.m_golds.empty())
        throw std::logic_error("Ticket Gold group is empty or belongs to another parent");
    return PrepareNativeProfiledScienceForward(view,parent,proposed,group.m_ranges,group.m_policy,group.m_golds);
}
} // namespace pgn::core
