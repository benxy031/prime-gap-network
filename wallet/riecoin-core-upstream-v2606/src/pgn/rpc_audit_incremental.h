// RPC-only direct-child projection. Consensus uses ReadNativeAuditLedger.
#pragma once
#include <pgn/native_report_audit.h>
#include <memory>

namespace pgn::core {
inline std::shared_ptr<const NativeAuditLedger> AdvanceRpcAuditLedger(
    const std::shared_ptr<const NativeAuditLedger>& previous,const CCoinsView& view,const CBlockIndex& tip,
    const NativeRequestHistoryArchive& history,const RangePolicyReadBudget& policy_budget,
    const RangeScheduleBudget& schedule_budget,ScriptWorkBudget signature_budget,const uint256& previous_policy){
    AssertLockHeld(cs_main);
    try{
        const auto head=view.GetPgnFactHead();
        if(!previous||previous->authority.empty()||!tip.pprev||!tip.phashBlock||!tip.IsValid(BLOCK_VALID_SCRIPTS)||
           !head||!head->Valid()||head->tip!=tip.GetBlockHash()||view.GetBestBlock()!=tip.GetBlockHash())return {};
        auto reports=ReadConfirmedNativeReports(view,tip,history,schedule_budget);
        if(!reports||reports->size()<previous->size())return {};
        const auto policy=ReadChainRangePolicy(view,tip,history,schedule_budget.archive,policy_budget);
        if(!policy.policy||previous_policy!=policy.policy->Id()||
           !ChainRangePolicyOnBranch(view,tip,*policy.policy)||policy.policy->Chain()!=head->chain)return {};
        // Advance the authenticated schedule even for a child without report
        // updates. This uses its index without materializing every active
        // lease. A confirmed lease is pinned by its report before expiry is
        // applied. With the same policy and parent, those old bindings stay
        // fixed. All new bindings still use an exact lookup on this child.
        const auto current=ReconstructCandidateRangeLease(view,tip,*policy.policy,history,schedule_budget,uint256{});
        if(current.verdict!=RangeScheduleVerdict::ReconstructedCandidate||current.lease)return {};
        const auto block=history.Load(view,tip,tip.nHeight,schedule_budget.archive);
        if(!block.block||!block.block->Coinbase())return {};
        std::optional<NativeAuditLedger> next;
        const auto write=[&]() -> NativeAuditLedger& {
            if(!next)next.emplace(*previous);
            return *next;
        };
        if(reports->size()>previous->size()){
            for(const auto& output:block.block->Coinbase()->vout){
                if(!IsNativeReportScript(output.scriptPubKey))continue;
                const auto event=DecodeNativeReportEnvelope(output.scriptPubKey);
                if(!event||output.nValue!=0||event->parent!=tip.pprev->GetBlockHash())return {};
                if(previous->contains(event->request)||(next&&next->contains(event->request)))continue;
                const auto confirmed=reports->find(event->request);
                if(confirmed==reports->end())return {};
                const auto& report=confirmed->second;
                if(report.height!=tip.nHeight||report.block!=tip.GetBlockHash()||
                   report.commitment.root!=event->root||report.commitment.ticket!=event->ticket)return {};
                const auto found=ReconstructCandidateRangeLease(view,tip,*policy.policy,history,schedule_budget,event->request);
                if(found.verdict!=RangeScheduleVerdict::ReconstructedCandidate||!found.lease||
                   found.lease->report_root!=event->root)return {};
                const auto& owned=*found.lease;
                NativeReportAudit entry;entry.report=report;entry.owner=owned.owner.Request().miner;
                std::copy(report.commitment.ticket.begin(),report.commitment.ticket.end(),entry.range.ticket_id.begin());
                entry.range.start=owned.assignment.start;entry.range.length=owned.assignment.end-owned.assignment.start;entry.range.segments=100;
                write().emplace(event->request,std::move(entry));
            }
        }
        if((next?next->size():previous->size())!=reports->size())return {};
        for(const auto& output:block.block->Coinbase()->vout){
            if(!IsNativeAuditScript(output.scriptPubKey))continue;
            const auto event=DecodeNativeAuditEnvelope(output.scriptPubKey);if(!event||output.nValue!=0)return {};
            ApplyNativeAuditEvent(write(),*event,*tip.pprev,policy.policy->Chain(),signature_budget);
        }
        // All policy, ownership and branch checks above still run for a
        // quiet block. All readers can then keep the same immutable ledger.
        if(!next)return previous;
        return std::make_shared<const NativeAuditLedger>(std::move(*next));
    }catch(const std::exception&){return {};}
}
}
