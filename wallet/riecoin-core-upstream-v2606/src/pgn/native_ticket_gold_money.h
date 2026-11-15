// MIT. Private planning adapter, never currency/block admission by itself.
#pragma once
#include "native_ticket_gold_block.h"
#include <pgn/native_science_monetary_plan.h>
namespace pgn::core {
class NativeTicketGoldMonetaryPlan;
std::optional<NativeTicketGoldMonetaryPlan> PrepareNativeTicketGoldMonetaryPlan(
    const CCoinsView&,const CBlockIndex&,const CBlock&,const VerifiedNativeTicketGoldBlock&,
    const ChainMonetaryPolicy&,const NativeAuditSettlementContext*);
class NativeTicketGoldMonetaryPlan {
    uint256 m_block;IssuanceHistoryStep m_forward;ChainMonetaryPolicy m_policy;
    CScript m_recipient;CAmount m_gross,m_winner;AuditSettlementPage m_audit;size_t m_golds;
    NativeTicketGoldMonetaryPlan(uint256 block,IssuanceHistoryStep forward,ChainMonetaryPolicy policy,
        CScript recipient,CAmount gross,CAmount winner,AuditSettlementPage audit,size_t golds)
        :m_block(block),m_forward(std::move(forward)),m_policy(policy),m_recipient(std::move(recipient)),
         m_gross(gross),m_winner(winner),m_audit(std::move(audit)),m_golds(golds){}
    friend std::optional<NativeTicketGoldMonetaryPlan> PrepareNativeTicketGoldMonetaryPlan(
        const CCoinsView&,const CBlockIndex&,const CBlock&,const VerifiedNativeTicketGoldBlock&,
        const ChainMonetaryPolicy&,const NativeAuditSettlementContext*);
public:
    const uint256& Block()const{return m_block;}
    const IssuanceHistoryStep& Forward()const{return m_forward;}
    const ChainMonetaryPolicy& Policy()const{return m_policy;}
    const CScript& Recipient()const{return m_recipient;}
    const AuditSettlementPage& Audit()const{return m_audit;}
    CAmount Gross()const{return m_gross;}CAmount WinnerSubsidy()const{return m_winner;}
    size_t GoldCount()const{return m_golds;}
};
// Uses the inherited grant/split/reservation arithmetic once per Gold, ordered
// by the already verified p1 list. Fees and audit-page output planning happen
// ONCE for the actual block. Full native transaction validation remains owed.
inline std::optional<NativeTicketGoldMonetaryPlan> PrepareNativeTicketGoldMonetaryPlan(
    const CCoinsView& view,const CBlockIndex& parent,const CBlock& block,const VerifiedNativeTicketGoldBlock& science,
    const ChainMonetaryPolicy& policy,const NativeAuditSettlementContext* context=nullptr)
{
    AssertLockHeld(cs_main);
    if(!NativeMonetaryPolicyOnBranch(view,parent,policy)||block.hashPrevBlock!=parent.GetBlockHash()||
       science.Block()!=block.GetHash()||BlockMerkleRoot(block)!=block.hashMerkleRoot)return {};
    const auto& golds=science.Group().Golds();if(golds.empty())return {};
    const auto& reservation=golds.front().Science().Reservation();const auto& request=reservation.Lease().owner.Request();
    if(reservation.Parent()!=parent.GetBlockHash()||request.chain!=policy.Chain()||request.reward.empty()||request.reward.IsUnspendable())return {};
    const auto before=view.GetPgnIssuanceState();
    if(!before||!before->Valid()||before->tip!=parent.GetBlockHash()||!before->partition||!before->pools||
       before->partition->policy!=policy.Id()||before->partition->ordinary_cap!=policy.Definition().ordinary_cap)return {};
    auto after=*before;CAmount gross_total{},winner_total{};
    if(policy.Epochs()&&!after.epochs){
        if(before->minted||before->promised||before->pools->near_miss||before->pools->audit)return {};
        for(auto* index=&parent;index&&index->nHeight>0;index=index->pprev){
            const auto step=view.GetPgnFactForward(index->GetBlockHash());
            if(!step||!step->Valid()||!index->pprev||step->before!=std::optional{FactHead{policy.Chain(),index->pprev->GetBlockHash()}}||
               step->after!=FactHead{policy.Chain(),index->GetBlockHash()}||!step->changes.empty())return {};
        }
        after.epochs=GoldEpochBalances{};
    }
    for(const auto& gold:golds){
        if(gold.Science().Reservation().Lease().owner.Id()!=reservation.Lease().owner.Id())return {};
        const auto total=IssuanceBudgetState::Create(after.cap,after.minted,after.promised);
        const auto ordinary=IssuanceBudgetState::Create(after.partition->ordinary_cap,after.partition->ordinary_minted,after.partition->ordinary_promised);
        if(!total||!ordinary)return {};
        CAmount gross{};std::optional<SubsidyDistributionQuote> split;
        if(policy.Epochs()){
            if(after.epochs->golds==std::numeric_limits<uint64_t>::max())return {};
            const auto grant=QuoteGoldEpoch(*policy.Epochs(),after.epochs->golds);if(!grant)return {};
            gross=grant->gold+grant->silver+grant->bronze+grant->red+grant->audit;
            split=SubsidyDistributionQuote{grant->gold,grant->silver+grant->bronze+grant->red,grant->audit};
            after.epochs->silver+=grant->silver;after.epochs->bronze+=grant->bronze;after.epochs->red+=grant->red;
            ++after.epochs->golds;after.epochs->last_gold_height=parent.nHeight+1;
        }else{
            const auto quote=QuoteIssuedSupplyLinearSubsidy(policy.Definition().curve,*total,*ordinary);if(!quote)return {};
            gross=quote->maximum_unpromised_grant;split=QuoteSubsidyDistribution(gross,policy.Definition());
        }
        if(!split||gross<0||gross>PGN_MAX_SUPPLY_ATOMIC-gross_total||split->winner>PGN_MAX_SUPPLY_ATOMIC-winner_total)return {};
        const auto rt=total->Reserve(gross),ro=ordinary->Reserve(gross);if(!rt||!ro)return {};
        const auto mt=rt->MintReserved(split->winner),mo=ro->MintReserved(split->winner);if(!mt||!mo)return {};
        after.minted=mt->CumulativeMinted();after.promised=mt->Promised();
        after.partition->ordinary_minted=mo->CumulativeMinted();after.partition->ordinary_promised=mo->Promised();
        after.pools->near_miss+=split->near_miss;after.pools->audit+=split->audit;
        gross_total+=gross;winner_total+=split->winner;
        if(!after.Valid())return {};
    }
    after.tip=block.GetHash();const auto audit=PrepareNativeAuditSettlement(view,parent,policy,context,after);if(!audit)return {};
    IssuanceHistoryStep forward{IssuanceDirection::Forward,{before,after}};
    if(!forward.Valid()||after.minted-before->minted+after.promised-before->promised!=gross_total)return {};
    return NativeTicketGoldMonetaryPlan{block.GetHash(),forward,policy,request.reward,gross_total,winner_total,*audit,golds.size()};
}
inline bool VerifyNativeTicketGoldCoinbase(const NativeTicketGoldMonetaryPlan& plan,const CBlock& block,CAmount actual_fees)
{
    if(block.GetHash()!=plan.Block()||BlockMerkleRoot(block)!=block.hashMerkleRoot||block.vtx.empty()||!block.vtx.front()||!block.vtx.front()->IsCoinBase())return false;
    const auto fees=QuoteNativeWinnerFees(actual_fees,plan.Policy().Definition());
    return fees&&*fees<=PGN_MAX_SUPPLY_ATOMIC-plan.WinnerSubsidy()&&
        VerifyAuditSettlementOutputs(plan.Audit(),*block.vtx.front(),plan.Recipient(),plan.WinnerSubsidy()+*fees);
}
}
