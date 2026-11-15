// MIT. Private whole-body integration candidate. No consensus activation.
#pragma once
#include "native_ticket_gold_group.h"
#include <pgn/canonical_encoding.h>
#include <pgn/proposed_ticket_requests.h>
#include <pgn/native_work_duty_admission.h>
#include <pgn/native_science_target.h>
#include <consensus/merkle.h>
#include <primitives/block.h>
#include <hash.h>

namespace pgn::core {
enum class TicketGoldBlockVerdict {Verified,InvalidBlock,InvalidEncoding,
    MissingProof,BudgetExceeded,HistoryUnavailable,GroupRejected};
struct TicketGoldBlockCheck;
class VerifiedNativeTicketGoldBlock {
    uint256 m_block,m_parent,m_source;size_t m_transactions;
    VerifiedNativeTicketGoldGroup m_group;
    VerifiedNativeTicketGoldBlock(const CBlock& block,uint256 source,VerifiedNativeTicketGoldGroup group)
        :m_block(block.GetHash()),m_parent(block.hashPrevBlock),m_source(source),
         m_transactions(block.vtx.size()),m_group(std::move(group)){}
    friend TicketGoldBlockCheck VerifyNativeTicketGoldBlock(const CCoinsView&,
        const CBlockIndex&,const NativeRequestHistoryArchive&,const CBlock&,const NativeBlockScienceBudget&);
    friend FactHistoryStep PrepareNativeTicketGoldBlockForward(const CCoinsView&,
        const CBlockIndex&,const CBlockIndex&,const VerifiedNativeTicketGoldBlock&);
public:
    const uint256& Block()const{return m_block;}
    const uint256& SourceMaterial()const{return m_source;}
    const VerifiedNativeTicketGoldGroup& Group()const{return m_group;}
};
struct TicketGoldBlockCheck {
    TicketGoldBlockVerdict verdict;
    std::optional<VerifiedNativeTicketGoldBlock> block;
    std::optional<TicketGoldGroupVerdict> group_verdict;
};

// Uses the existing proof envelope verbatim. Each parser allocation is charged
// against the native payload budget and the script is rebuilt canonically.
inline CandidateBlockScienceProof DecodeTicketGoldProof(const Bytes& bytes,
    const ChainSciencePolicy& policy,const NativeBlockScienceBudget& budget)
{
    const EncodingLimits limits{0,budget.maximum_payload_bytes,0,0};
    detail::Reader in{bytes,limits};in.domain("PGN/SCIENCE-BLOCK/1");
    CandidateBlockScienceProof proof;const auto request=in.digest();
    std::copy(request.begin(),request.end(),proof.request.begin());proof.report_root=in.digest();
    const auto get=[&](uint64_t maximum){
        const auto count=in.count();detail::encodingRequire(count<=maximum&&count<=bytes.size()-in.position,"Oversized Gold field");
        Bytes value(bytes.begin()+in.position,bytes.begin()+in.position+static_cast<size_t>(count));in.position+=count;return value;
    };
    proof.signature=get(budget.signature.signature_bytes);
    proof.evidence.segment_bytes=get(policy.Definition().evidence.encoding.max_encoded_bytes);
    proof.evidence.inclusion.index=in.little(4);const auto siblings=in.count();
    detail::encodingRequire(siblings<=32&&siblings<=(bytes.size()-in.position)/32,"Truncated Gold inclusion");
    for(uint64_t i=0;i<siblings;++i)proof.evidence.inclusion.siblings.push_back(in.digest());
    proof.evidence.record_index=in.little(8);proof.evidence.scientific_bytes=get(policy.Definition().proof.max_encoded_bytes);
    detail::encodingRequire(in.position==bytes.size(),"Trailing Gold evidence");return proof;
}

// Actual candidate body, exact native parent and existing proof/signature/audit
// contracts. This is NOT native UTXO/script/fee validation, complete-report
// enumeration, a closed-ticket uniqueness index, money or fresh-work proof.
inline TicketGoldBlockCheck VerifyNativeTicketGoldBlock(const CCoinsView& view,
    const CBlockIndex& parent,const NativeRequestHistoryArchive& history,
    const CBlock& block,const NativeBlockScienceBudget& budget)
{
    AssertLockHeld(cs_main);
    if(!budget.maximum_coinbase_outputs||!budget.maximum_payload_bytes||!budget.maximum_script_bytes)
        return {TicketGoldBlockVerdict::BudgetExceeded,{},{}};
    if(!parent.phashBlock||block.hashPrevBlock!=parent.GetBlockHash()||block.vtx.empty()||
       !block.vtx.front()||!block.vtx.front()->IsCoinBase())return {TicketGoldBlockVerdict::InvalidBlock,{},{}};
    for(size_t i=1;i<block.vtx.size();++i)if(!block.vtx[i]||block.vtx[i]->IsCoinBase())return {TicketGoldBlockVerdict::InvalidBlock,{},{}};
    bool mutated=false;
    if(BlockMerkleRoot(block,&mutated)!=block.hashMerkleRoot||mutated)return {TicketGoldBlockVerdict::InvalidBlock,{},{}};
    if(block.vtx.front()->vout.size()>budget.maximum_coinbase_outputs)return {TicketGoldBlockVerdict::BudgetExceeded,{},{}};
    const auto policy=ReadChainSciencePolicy(view,parent,history,budget.schedule.archive,budget.policy,&budget.schedule,&budget.signature);
    if(!policy.policy)return {TicketGoldBlockVerdict::HistoryUnavailable,{},{}};
    if(!policy.policy->EffectiveParent().IsNull()){
        const auto encoded=NativeScientificHeaderTarget(*policy.policy);
        if(!encoded||block.nNonce!=0||block.nBits!=*encoded)return {TicketGoldBlockVerdict::InvalidBlock,{},{}};
    }
    std::vector<CandidateBlockScienceProof> proofs;uint64_t remaining=budget.maximum_payload_bytes;
    for(const auto& output:block.vtx.front()->vout){
        const auto& script=output.scriptPubKey;
        if(script.size()>budget.maximum_script_bytes)return {TicketGoldBlockVerdict::BudgetExceeded,{},{}};
        auto cursor=script.begin();opcodetype op;Bytes tag,bytes;
        if(!script.GetOp(cursor,op)||op!=OP_RETURN||!script.GetOp(cursor,op,tag)||
           tag!=Bytes{BlockScienceEnvelopeTag.begin(),BlockScienceEnvelopeTag.end()})continue;
        if(output.nValue!=0||!script.GetOp(cursor,op,bytes)||cursor!=script.end()||bytes.size()>remaining)
            return {TicketGoldBlockVerdict::InvalidEncoding,{},{}};
        remaining-=bytes.size();
        try{
            auto proof=DecodeTicketGoldProof(bytes,*policy.policy,budget);
            const auto canonical=BuildCandidateBlockScienceEnvelope(proof,budget);
            if(!canonical||*canonical!=script)return {TicketGoldBlockVerdict::InvalidEncoding,{},{}};
            proofs.push_back(std::move(proof));
        }catch(const std::invalid_argument&){return {TicketGoldBlockVerdict::InvalidEncoding,{},{}};}
    }
    if(proofs.empty())return {TicketGoldBlockVerdict::MissingProof,{},{}};
    if(!VerifyProposedNativeReportCommitments(view,parent,history,block,budget.policy,budget.schedule,budget.signature)||
       !VerifyProposedNativeAuditEvents(view,parent,history,block,budget.policy,budget.schedule,budget.signature))
        return {TicketGoldBlockVerdict::InvalidBlock,{},{}};
    std::optional<VerifiedRequestRegistry> registry;
    const RequestReplayBudget replay_budget{budget.schedule.maximum_blocks,budget.schedule.maximum_requests,budget.schedule.archive,budget.schedule.scan};
    const auto replay=ReplayConfirmedRequestHistoryToParent(view,parent,history,registry,replay_budget);
    if(replay.verdict!=RequestReplayVerdict::AtParent||!registry)return {TicketGoldBlockVerdict::HistoryUnavailable,{},{}};
    const auto proposed=VerifyProposedTicketRequests(view,parent,*registry,block,budget.schedule.scan,budget.schedule.maximum_requests);
    if(!proposed.batch||!VerifyProposedNativeWorkDuties(view,parent,history,*proposed.batch,budget.policy,budget.schedule,budget.signature))
        return {TicketGoldBlockVerdict::InvalidBlock,{},{}};
    auto group=VerifyNativeTicketGoldGroup(view,parent,history,proofs,budget);
    if(!group.group)return {TicketGoldBlockVerdict::GroupRejected,{},group.verdict};
    HashWriter source{TaggedHash("PGN/UNAPPROVED/TICKET-SCIENCE-SOURCE/1")};
    source<<policy.policy->Id();
    for(const auto& gold:group.group->Golds())source<<gold.Science().Science().Key();
    return {TicketGoldBlockVerdict::Verified,VerifiedNativeTicketGoldBlock{block,source.GetHash(),std::move(*group.group)},group.verdict};
}
inline FactHistoryStep PrepareNativeTicketGoldBlockForward(const CCoinsView& view,
    const CBlockIndex& parent,const CBlockIndex& proposed,const VerifiedNativeTicketGoldBlock& checked)
{
    AssertLockHeld(cs_main);
    if(!proposed.phashBlock||proposed.GetBlockHash()!=checked.m_block||
       parent.GetBlockHash()!=checked.m_parent||proposed.nTx!=checked.m_transactions)
        throw std::logic_error("Whole-ticket block identity changed after verification");
    return PrepareNativeTicketGoldGroupForward(view,parent,proposed,checked.m_group);
}
}
