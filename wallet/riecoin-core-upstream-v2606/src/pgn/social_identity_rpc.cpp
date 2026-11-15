// Copyright (c) 2026 The PGN developers. MIT.
// Read-only display association. No wallet calls, writes to Native state,
// permissions, peer presence or profile names are accepted from an RPC caller.
#include <pgn/social_identity_rpc.h>
#include <pgn/social_identity_batch.h>
#include <pgn/social_identity_snapshot.h>
#include <pgn/native_node_options.h>
#include <pgn/request_history_archive.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <common/args.h>
#include <validation.h>
#include <chainparams.h>
#include <semaphore>

namespace {
using namespace pgn::social::identity::committed;
using namespace pgn::core;
class NativeIdentityService {
    CandidateIndex index_;
    TargetResolver targets_;
    std::mutex indexing_;
    // A Social-only nonblocking crypto gate; mining never acquires this gate.
    std::counting_semaphore<2> verification_{2};
public:
    NativeIdentityService(const fs::path& path,const uint256& genesis):index_(path,genesis),targets_(index_,128){}
    UniValue Read(ChainstateManager& manager,const NativeLeaderboardSnapshot& snapshot,const UniValue& scripts) {
        const auto canonical=[&](const uint256& hash,int32_t height){
            LOCK(cs_main);const auto* block=manager.ActiveChain()[height];
            return snapshot.chainstate==&manager.ActiveChainstate()&&
                snapshot.genesis==manager.GetConsensus().hashGenesisBlock&&block&&block->GetBlockHash()==hash;
        };
        const auto read=[&](int32_t height,const std::optional<uint256>& expected)->std::optional<VerifiedNativeRequestBlock>{
            // Archive authentication uses the current canonical view even if
            // this immutable snapshot has become an ancestor after extension.
            // Copies leave cs_main before certificate BIP322 verification.
            LOCK(cs_main);
            if(snapshot.chainstate!=&manager.ActiveChainstate()||snapshot.genesis!=manager.GetConsensus().hashGenesisBlock)
                throw AssociationContextUnavailable("Native chainstate changed");
            const auto* anchored=manager.ActiveChain()[snapshot.height];
            const auto* block=manager.ActiveChain()[height];const auto* tip=manager.ActiveChain().Tip();
            const auto* archive=manager.ActiveChainstate().PgnRequestHistory();
            if(!anchored||anchored->GetBlockHash()!=snapshot.tip||!block||!tip||height>snapshot.height||
               (expected&&*expected!=block->GetBlockHash())||!archive||!manager.m_options.pgn_candidate_science_budget)
                throw AssociationContextUnavailable("Canonical Native archive unavailable");
            // Genesis is an immutable compiled block, not necessarily archived.
            // Authenticate it against the actual current Native ancestry too.
            if(height==0)return VerifyNativeRequestBlock(manager.ActiveChainstate().CoinsTip(),*tip,0,manager.GetParams().GenesisBlock()).block;
            const auto result=archive->Load(manager.ActiveChainstate().CoinsTip(),*tip,height,
                manager.m_options.pgn_candidate_science_budget->schedule.archive);
            return result.block;
        };
        std::string sync_state="ready",sync_reason;
        {
            std::unique_lock sync(indexing_,std::try_to_lock);
            if(!sync.owns_lock()){sync_state="pending";sync_reason="Native identity index is advancing";}
            else {
                // Work is bounded per call, independent of a world user count.
                // No caller can make a read scan the chain once per target.
                for(unsigned n=0;n<16;++n){const auto tip=index_.Tip();
                    if(tip.height<0||canonical(tip.hash,tip.height))break;
                    const auto result=index_.RollbackOne();
                    if(result.state=="uncertain"){index_.Recover();sync_state="unavailable";sync_reason="Identity index recovery; retry";break;}
                }
                auto tip=index_.Tip();
                if(sync_state=="ready"&&tip.height>=0&&!canonical(tip.hash,tip.height)){
                    sync_state="pending";sync_reason="Identity index is rolling back the old Native branch";
                }
                if(sync_state=="ready")for(unsigned n=0;n<16&&index_.Tip().height<snapshot.height;++n){
                    const auto block=read(index_.Tip().height+1,std::nullopt);
                    if(!block)throw AssociationContextUnavailable("Authenticated Native identity history missing");
                    const auto result=index_.Append(*block,canonical);
                    if(result.state=="uncertain"){index_.Recover();sync_state="unavailable";sync_reason="Identity index recovery; retry";break;}
                    if(result.state=="reorg"){sync_state="pending";sync_reason="Native anchor changed while indexing";break;}
                }
                if(sync_state=="ready"&&index_.Tip().height<snapshot.height){sync_state="pending";sync_reason="Native identity history is being indexed";}
            }
        }
        UniValue out(UniValue::VOBJ);out.pushKV("schema","PGN_SOCIAL_ASSOCIATIONS_2");out.pushKV("genesis",snapshot.genesis.GetHex());
        out.pushKV("bestblockhash",snapshot.tip.GetHex());out.pushKV("height",snapshot.height);
        out.pushKV("index_height",index_.Tip().height);out.pushKV("generation",std::to_string(index_.Generation()));
        out.pushKV("state",sync_state);out.pushKV("reason",sync_reason);
        out.pushKV("results",ResolveBatch(scripts,[&](const std::string& s){
            TargetResolution reply;reply.social_script=s;reply.anchor={snapshot.height,snapshot.tip};
            if(sync_state!="ready"){reply.state=sync_state;reply.reason=sync_reason;return reply;}
            if(!verification_.try_acquire()){reply.state="pending";reply.reason="Social proof verification is busy";return reply;}
            struct Release {std::counting_semaphore<2>& gate;~Release(){gate.release();}} release{verification_};
            return targets_.Advance(s,snapshot,[&](const CandidateRef& ref){return read(ref.height,ref.block);},canonical,4);
        }));
        if(!canonical(snapshot.tip,snapshot.height))throw AssociationContextUnavailable("Native association snapshot reset: anchor reorganized");
        return out;
    }
};
std::shared_ptr<NativeIdentityService> Service(const fs::path& path,const uint256& genesis) {
    static std::mutex mutex;static fs::path saved_path;static uint256 saved_genesis;
    static std::shared_ptr<NativeIdentityService> saved;
    std::lock_guard lock(mutex);
    if(saved){if(saved_path!=path||saved_genesis!=genesis)throw AssociationContextUnavailable("Native identity service context changed");return saved;}
    auto result=std::make_shared<NativeIdentityService>(path,genesis);saved_path=path;saved_genesis=genesis;saved=result;return result;
}
RPCMethod getsocialassociations(){
    return RPCMethod{"getsocialassociations","Resolve bounded Social scripts through explicit dual consent and authenticated Native operator labels. Display association confers no permission or online status.",
        {{"social_scripts",RPCArg::Type::ARR,RPCArg::Optional::NO,"One through100 canonical Social scripts; errors are isolated per element",{{"script",RPCArg::Type::STR,RPCArg::Optional::NO,"Lower-case P2WPKH script"}}}},
        RPCResult{RPCResult::Type::OBJ_DYN,"","Canonical anchor, index progress and per-target state",{{RPCResult::Type::ELISION,"","Display association provenance, never role authority"}}},RPCExamples{""},
        [](const RPCMethod&,const JSONRPCRequest& request)->UniValue{
            const auto& scripts=request.params[0];
            if(!scripts.isArray()||scripts.size()==0||scripts.size()>100)throw JSONRPCError(RPC_INVALID_PARAMETER,"One through100 Social scripts required");
            auto& manager=EnsureAnyChainman(request.context);
            try{
                const auto snapshot=ReadNativeSocialIdentitySnapshot(manager);
                return Service(EnsureAnyArgsman(request.context).GetDataDirNet()/"social"/"native-identity-v2",snapshot->genesis)->Read(manager,*snapshot,scripts);
            }catch(const std::exception& error){throw JSONRPCError(RPC_MISC_ERROR,error.what());}
        }};
}
}
void RegisterPgnSocialIdentityRPCCommands(CRPCTable& table){
    static const CRPCCommand command{"social",&getsocialassociations};table.appendCommand(command.name,&command);
}
