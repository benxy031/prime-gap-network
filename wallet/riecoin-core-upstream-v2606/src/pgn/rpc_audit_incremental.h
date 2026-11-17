// RPC-only direct-child projection. Consensus uses ReadNativeAuditLedger.
#pragma once
#include <pgn/native_report_audit.h>
#include <memory>

namespace pgn::core {
inline std::shared_ptr<const NativeAuditLedger> AdvanceRpcAuditLedger(
    const std::shared_ptr<const NativeAuditLedger>& previous,const CCoinsView& view,const CBlockIndex& tip,
    const NativeRequestHistoryArchive& history,const RangePolicyReadBudget& policy_budget,
    const RangeScheduleBudget& schedule_budget,ScriptWorkBudget signature_budget){
    AssertLockHeld(cs_main);
    try{
        const auto head=view.GetPgnFactHead();
        if(!previous||previous->authority.empty()||!tip.pprev||!tip.phashBlock||!tip.IsValid(BLOCK_VALID_SCRIPTS)||
           !head||!head->Valid()||head->tip!=tip.GetBlockHash()||view.GetBestBlock()!=tip.GetBlockHash())return {};
        auto reports=ReadConfirmedNativeReports(view,tip,history,schedule_budget);
        if(!reports||reports->size()<previous->size())return {};
        const auto policy=ReadChainRangePolicy(view,tip,history,schedule_budget.archive,policy_budget);
        if(!policy.policy||!ChainRangePolicyOnBranch(view,tip,*policy.policy)||policy.policy->Chain()!=head->chain)return {};
        // Re-derive the authenticated current schedule rather than assuming
        // that a cached assignment survives a policy or retention transition.
        std::optional<NativeAuditLedger> next;
        const auto write=[&]() -> NativeAuditLedger& {
            if(!next)next.emplace(*previous);
            return *next;
        };
        // Borrow each authenticated lease under cs_main. The verified
        // request registry permits each signed request ID only once.
        size_t matched{};
        const auto bind=[&](const CandidateRangeLease& owned) {
            const auto report_entry=reports->find(owned.owner.Id());
            if(report_entry==reports->end())return true;
            const auto& [id,report]=*report_entry;
            const auto old=previous->find(id);
            if(old!=previous->end()){
                const auto& retained=old->second;const auto& a=retained.report.commitment;const auto& b=report.commitment;
                if(a.request!=b.request||a.ticket!=b.ticket||a.root!=b.root||a.parent!=b.parent||a.signature!=b.signature||
                   retained.report.block!=report.block||retained.report.height!=report.height||
                   retained.owner!=owned.owner.Request().miner||retained.range.start!=owned.assignment.start||
                   retained.range.length!=owned.assignment.end-owned.assignment.start||retained.range.segments!=100||
                   !std::equal(retained.range.ticket_id.begin(),retained.range.ticket_id.end(),report.commitment.ticket.begin()))return false;
            }else{
                // Only the direct child may add a previously unseen report.
                // A missing earlier report requires full reconstruction.
                if(report.height!=tip.nHeight||report.block!=tip.GetBlockHash())return false;
                NativeReportAudit entry;entry.report=report;entry.owner=owned.owner.Request().miner;
                std::copy(report.commitment.ticket.begin(),report.commitment.ticket.end(),entry.range.ticket_id.begin());
                entry.range.start=owned.assignment.start;entry.range.length=owned.assignment.end-owned.assignment.start;entry.range.segments=100;
                write().emplace(id,std::move(entry));
            }
            ++matched;
            return true;
        };
        bool consistent=true;
        const auto schedule=VisitCandidateRangeSchedule(view,tip,*policy.policy,history,schedule_budget,
            [&](const CandidateRangeLease& lease){if(consistent)consistent=bind(lease);});
        if(!schedule.schedule||!consistent||matched!=reports->size())return {};
        // No old entry may silently disappear from the confirmed registry.
        if((next?next->size():previous->size())!=reports->size())return {};
        const auto block=history.Load(view,tip,tip.nHeight,schedule_budget.archive);
        if(!block.block||!block.block->Coinbase())return {};
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
