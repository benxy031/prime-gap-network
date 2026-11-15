// MIT. Private candidate derived index; no network or consensus activation.
#pragma once
#include <pgn/candidate_block_science.h>
#include <pgn/native_report_audit.h>
#include <pgn/canonical_encoding.h>
#include <algorithm>
#include <array>
#include <limits>
#include <map>
namespace pgn::core {
struct NativeTicketClosure {
    uint256 request,block,report;
    uint64_t golds{};
    bool Valid()const{return !request.IsNull()&&!block.IsNull()&&!report.IsNull()&&golds;}
    bool operator==(const NativeTicketClosure&)const=default;
    SERIALIZE_METHODS(NativeTicketClosure,obj){READWRITE(obj.request,obj.block,obj.report,obj.golds);}
};
struct NativeTicketClosures {std::map<uint256,NativeTicketClosure> tickets;};
namespace native_audit_replay_detail {
template<>struct ExtraCodec<NativeTicketClosures>{
    // Private index namespace, separate from the existing audit/duty snapshots.
    static constexpr uint8_t id=225;
    static void Write(DataStream& stream,const NativeTicketClosures& value,
                      std::vector<std::pair<uint256,size_t>>*){stream<<value.tickets;}
    static NativeTicketClosures Read(DataStream& stream){
        NativeTicketClosures out;const auto count=ReadCompactSize(stream);
        // A checkpoint entry has four fixed hashes and an eight-byte count.
        // Charge bytes actually retained, never invent a lifetime ticket cap.
        RequireAudit(count<=stream.size()/136,"Truncated ticket-closure checkpoint");
        for(uint64_t i=0;i<count;++i){uint256 ticket;NativeTicketClosure row;stream>>ticket>>row;
            RequireAudit(!ticket.IsNull()&&row.Valid()&&out.tickets.emplace(ticket,row).second,
                         "Invalid or duplicate ticket closure checkpoint");}
        return out;
    }
};
}
using NativeTicketClosureReplay=native_audit_replay_detail::LedgerState<NativeTicketClosures>;
// This index is DERIVED from authenticated, previously validated native blocks.
// It is not a new database, a user-supplied counter, or permission to accept a
// block. The caller must still authenticate all current Gold mathematics,
// complete-list semantics, native transactions, money, header work and undo.
inline std::optional<NativeTicketClosureReplay> ReadNativeTicketClosures(
    const CCoinsView& view,const CBlockIndex& parent,const NativeRequestHistoryArchive& history,
    const NativeBlockScienceBudget& budget)
{
    using namespace native_audit_replay_detail;AssertLockHeld(cs_main);
    static Snapshot<NativeTicketClosures,std::array<uint64_t,3>> cached;
    const auto before_block=[&](const NativeAuditLedger& ledger,const CBlockIndex& prior,
                               const VerifiedNativeRequestBlock& block,NativeTicketClosures& extra){
        const auto* index=parent.GetAncestor(prior.nHeight+1);
        if(!index||!index->phashBlock||!index->IsValid(BLOCK_VALID_SCRIPTS)||index->pprev!=&prior)return false;
        std::optional<uint256> request,report;uint64_t golds{},remaining=budget.maximum_payload_bytes;
        for(const auto& output:block.Coinbase()->vout){
            const auto& script=output.scriptPubKey;auto at=script.begin();opcodetype op;Bytes tag,bytes;
            if(!script.GetOp(at,op)||op!=OP_RETURN||!script.GetOp(at,op,tag)||
               tag!=Bytes{BlockScienceEnvelopeTag.begin(),BlockScienceEnvelopeTag.end()})continue;
            if(output.nValue||script.size()>budget.maximum_script_bytes||!script.GetOp(at,op,bytes)||
               at!=script.end()||bytes.size()>remaining||++golds>budget.maximum_coinbase_outputs)return false;
            remaining-=bytes.size();detail::Reader in{bytes,EncodingLimits{0,budget.maximum_payload_bytes,0,0}};
            in.domain("PGN/SCIENCE-BLOCK/1");uint256 r,root;
            const auto request_bytes=in.digest(),root_bytes=in.digest();
            std::copy(request_bytes.begin(),request_bytes.end(),r.begin());std::copy(root_bytes.begin(),root_bytes.end(),root.begin());
            if((request&&*request!=r)||(report&&*report!=root))return false;
            request=r;report=root;
        }
        if(!golds)return true;
        const auto found=ledger.find(*request);
        // Compare committed bytes; uint256 display hex reverses their order.
        if(found==ledger.end()||!NativeAuditReportQualified(ledger,found->second)||!found->second.details_root||
           !std::equal(found->second.details_root->begin(),found->second.details_root->end(),report->begin()))return false;
        const auto ticket=found->second.report.commitment.ticket;
        return !ticket.IsNull()&&extra.tickets.emplace(ticket,
            NativeTicketClosure{*request,index->GetBlockHash(),*report,golds}).second;
    };
    return ReadIncremental(view,parent,history,budget.policy,budget.schedule,budget.signature,cached,
        std::array<uint64_t,3>{budget.maximum_coinbase_outputs,budget.maximum_payload_bytes,budget.maximum_script_bytes},
        before_block,[](const NativeTicketClosures& extra)->std::optional<size_t>{
            if(extra.tickets.size()>std::numeric_limits<size_t>::max()/(sizeof(NativeTicketClosure)+sizeof(uint256)+256))return {};
            return extra.tickets.size()*(sizeof(NativeTicketClosure)+sizeof(uint256)+256);
        });
}
}
