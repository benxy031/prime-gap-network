// Copyright (c) 2026 The PGN developers
// Distributed under the MIT software license; see COPYING.
#include <pgn/social_access.h>
#include <pgn/social_policy.h>
#include <pgn/social_storage.h>
#include <addresstype.h>
#include <chainparams.h>
#include <common/signmessage.h>
#include <crypto/sha256.h>
#include <dbwrapper.h>
#include <key_io.h>
#include <util/fs_helpers.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <cstdio>
#include <map>
#include <mutex>
#include <vector>

namespace pgn::social {
namespace {
namespace m=moderation;
// This is a bounded PRIVATE journal, not a claim of scalable public governance.
// Exhaustion disables only new moderation controls, never chat/mining. A signed
// canonical checkpoint/compaction policy is required before public activation.
constexpr size_t MAX_JOURNAL_BYTES=4*1024*1024,MAX_JOURNAL_RECORDS=4096;
std::string Genesis(){return Params().GetConsensus().hashGenesisBlock.GetHex();}
int64_t Now(){return GetTime<std::chrono::milliseconds>().count();}
std::string Hash(const std::string& s){unsigned char out[32];CSHA256().Write(reinterpret_cast<const unsigned char*>(s.data()),s.size()).Finalize(out);return HexStr(out);}
bool Proof(const std::string& script,const std::string& address,const std::string& message,const std::string& signature){
    const auto raw=DecodeBase64(signature);const auto bytes=TryParseHex<unsigned char>(script);
    if(!raw||raw->size()<100||raw->size()>140||(*raw)[0]!=2||EncodeBase64(*raw)!=signature||!bytes)return false;
    CScript native(bytes->begin(),bytes->end());CTxDestination destination;
    if(!ExtractDestination(native,destination)||EncodeDestination(destination)!=address)return false;
    auto proof=*raw;
    return MessageVerifyBIP322(destination,proof,message,MessageVerificationResult::ERR_ADDRESS_NO_KEY)==MessageVerificationResult::OK;
}
struct Item{std::string message,signature,id,nonce;};
std::string Key(uint64_t n){static constexpr char digits[]="0123456789abcdef";std::string out="j/";for(int shift=60;shift>=0;shift-=4)out+=digits[(n>>shift)&15];return out;}
UniValue Encode(const Item& item){UniValue out(UniValue::VOBJ);out.pushKV("canonical_message",item.message);out.pushKV("signature",item.signature);return out;}
}
bool IsAccessProof(const std::string& message){return message.starts_with("PGN/SOCIAL/MODERATION/1\n")||message.starts_with("PGN/SOCIAL/POLICY-ATTEST/1\n");}
class AccessStore::Impl {
public:
    fs::path directory;Lookup lookup;std::mutex mutex;std::unique_ptr<CDBWrapper> db;
    m::Verifier verifier{Genesis(),Hash,Proof};m::PolicyVerifier policy{Genesis(),Hash,Proof};
    std::optional<m::PolicyTracker> tracker;std::string root_document,root_id,error;
    std::vector<Item> journal;std::map<std::string,size_t> ids;std::map<std::string,std::string> nonces;
    std::map<std::string,m::VerifiedAction> actions;std::set<std::pair<std::string,std::string>> masks;size_t bytes{};
    Impl(fs::path path,Lookup target):directory(std::move(path)),lookup(std::move(target)){}
    void RebuildMasks(){
        masks.clear();if(!tracker)return;const auto state=tracker->Snapshot();if(state.conflicted)return;
        for(const auto& [id,mask]:state.ratified)masks.emplace(mask.channel,mask.target_event);
        for(const auto& [id,action]:actions){const m::TargetContext target{Genesis(),action.Channel(),action.TargetEvent()};const auto result=m::Authorize(state,action,&target);if(result==m::Decision::Allowed||result==m::Decision::AllowedRatified)masks.emplace(action.Channel(),action.TargetEvent());}
    }
    Item ParseApply(const std::string& message,const std::string& signature,m::PolicyTracker& staged,std::map<std::string,m::VerifiedAction>& projected,bool loading){
        if(message.size()+signature.size()>23500)throw std::runtime_error("Bounded moderation transport proof required");
        const auto use=loading?m::ProofUse::StoredHistory:m::ProofUse::Admission;
        if(message.starts_with("PGN/SOCIAL/POLICY-ATTEST/1\n")){
            const auto vote=policy.Verify(message,signature,Now(),use);
            const auto result=staged.Observe(vote);
            if(result==m::PolicyResult::Rejected)throw std::runtime_error("Peer rejected unauthorized or non-contiguous policy transition");
            // Conflicted is durably retained as EVIDENCE, not a newly elected head.
            return {message,signature,vote.EventId(),vote.SenderScript()+":"+vote.Nonce()};
        }
        const auto action=verifier.Verify(message,signature,Now(),use);
        auto target=lookup(action.TargetEvent());
        // At startup an original target may already be pruned. Its signed
        // action is still verified; only a currently present exact target can
        // be projected into the visible chat. No target is invented by RPC.
        const m::TargetContext retained{Genesis(),action.Channel(),action.TargetEvent()};
        const auto decision=m::Authorize(staged.Snapshot(),action,target?&*target:loading?&retained:nullptr);
        if(decision!=m::Decision::Allowed&&decision!=m::Decision::AllowedRatified)throw std::runtime_error(m::Reason(decision));
        projected.insert_or_assign(action.EventId(),action);
        return {message,signature,action.EventId(),action.SenderScript()+":"+action.Nonce()};
    }
    void LoadLocked(){
        tracker.reset();db.reset();journal.clear();ids.clear();nonces.clear();actions.clear();masks.clear();bytes=0;root_document.clear();root_id.clear();error.clear();
        // A local JSON file cannot become public-network moderator authority.
        if(Params().GetChainType()!=ChainType::REGTEST){error="Moderation unavailable: no public authority/order bootstrap has been approved";return;}
        const auto file=directory/"private-moderation-bootstrap.json";
        if(!fs::exists(file)){error="Moderation unavailable: explicit private regtest anchor absent";return;}
        if(fs::file_size(file)>m::MAX_POLICY_BYTES+256)throw std::runtime_error("Private bootstrap oversized");
        FILE* handle=fsbridge::fopen(file,"rb");if(!handle)throw std::runtime_error("Private bootstrap unreadable");
        std::string raw(fs::file_size(file),'\0');const auto n=std::fread(raw.data(),1,raw.size(),handle);std::fclose(handle);
        UniValue saved;if(n!=raw.size()||!saved.read(raw)||!saved.isObject()||saved.getKeys().size()!=2||saved["synthetic_only"].get_str()!="PGN_PRIVATE_REGTEST_MODERATION_1")throw std::runtime_error("Explicit synthetic regtest bootstrap required");
        auto anchor=policy.ParseDocument(saved["policy"]);root_document=anchor.canonical;root_id=anchor.id;tracker.emplace(anchor);
        fs::create_directories(directory);db=std::make_unique<CDBWrapper>(DBParams{.path=directory/"access-private-v1",.cache_bytes=1024*1024});
        std::string stored_root;if(db->Read(std::string{"root"},stored_root)){if(stored_root!=root_document)throw std::runtime_error("Private authority root changed: refusing silent local rebind");}
        else{std::unique_ptr<CDBIterator> first(db->NewIterator());first->SeekToFirst();if(first->Valid())throw std::runtime_error("Moderation journal missing trust anchor");CDBBatch batch(*db);batch.Write(std::string{"root"},root_document);CommitSocialBatch(*db,batch);}
        uint64_t expected=1;std::unique_ptr<CDBIterator> it(db->NewIterator());
        for(it->Seek(Key(0));it->Valid();it->Next()){
            std::string key,value;if(!it->GetKey(key))throw std::runtime_error("Invalid moderation journal key");if(!key.starts_with("j/"))break;
            if(key!=Key(expected)||journal.size()>=MAX_JOURNAL_RECORDS||!it->GetValue(value)||value.size()>25000||bytes+value.size()>MAX_JOURNAL_BYTES)throw std::runtime_error("Moderation journal bound/sequence invalid");
            UniValue entry;if(!entry.read(value))throw std::runtime_error("Invalid moderation journal document");
            const auto item=ParseApply(entry["canonical_message"].get_str(),entry["signature"].get_str(),*tracker,actions,true);
            if(ids.contains(item.id)||nonces.contains(item.nonce))throw std::runtime_error("Moderation journal replay invalid");
            ids[item.id]=journal.size();nonces[item.nonce]=item.id;journal.push_back(item);bytes+=value.size();++expected;
        }
        RebuildMasks();
    }
};
AccessStore::AccessStore(const fs::path& directory,Lookup lookup):impl(std::make_unique<Impl>(directory,std::move(lookup))){}
AccessStore::~AccessStore()=default;
void AccessStore::Load(){std::lock_guard guard(impl->mutex);try{impl->LoadLocked();}catch(const std::exception& e){impl->error=e.what();impl->tracker.reset();}}
std::string AccessStore::Root(){std::lock_guard guard(impl->mutex);return impl->root_id;}
void AccessStore::Commit(const std::string& message,const std::string& signature){
    std::lock_guard guard(impl->mutex);
    if(!impl->error.empty()||!impl->tracker)throw std::runtime_error(impl->error.empty()?"Verified moderator authority unavailable":impl->error);
    auto staged=*impl->tracker;std::map<std::string,m::VerifiedAction> projected;
    // Always verify and authorize against the CURRENT policy, even on retry.
    auto item=impl->ParseApply(message,signature,staged,projected,false);
    if(impl->ids.contains(item.id))return;
    if(impl->nonces.contains(item.nonce))throw std::runtime_error("Moderation nonce replay");
    const auto encoded=Encode(item).write();
    if(impl->journal.size()>=MAX_JOURNAL_RECORDS||impl->bytes+encoded.size()>MAX_JOURNAL_BYTES)throw std::runtime_error("Private moderation journal full; chat continues. Public compaction/authority policy not activated");
    CDBBatch batch(*impl->db);batch.Write(Key(impl->journal.size()+1),encoded);
    try{CommitSocialBatch(*impl->db,batch);}catch(const StorageCommitUncertain&){impl->error="Moderation WAL outcome uncertain; restart/reconcile this Social journal before privileged retry";throw;}
    // Publish only after the synchronous native atomic batch succeeded.
    const bool policy_changed=staged.Current().id!=impl->tracker->Current().id||staged.Conflicted()!=impl->tracker->Conflicted();
    impl->ids[item.id]=impl->journal.size();impl->nonces[item.nonce]=item.id;impl->journal.push_back(std::move(item));impl->bytes+=encoded.size();impl->tracker=std::move(staged);
    for(const auto& [id,action]:projected){impl->actions.insert_or_assign(id,action);impl->masks.emplace(action.Channel(),action.TargetEvent());}
    if(policy_changed)impl->RebuildMasks();
}
bool AccessStore::Hidden(const std::string& channel,const std::string& event){
    std::lock_guard guard(impl->mutex);if(!impl->tracker||!impl->error.empty())return false;
    return impl->masks.contains({channel,event});
}
UniValue AccessStore::Read(uint64_t cursor,unsigned limit){
    std::lock_guard guard(impl->mutex);if(limit<1||limit>10)throw std::runtime_error("Moderation page1through10 required");
    UniValue out(UniValue::VOBJ),records(UniValue::VARR),moderators(UniValue::VARR),masks(UniValue::VARR);
    out.pushKV("genesis",Genesis());out.pushKV("private_qualification",true);out.pushKV("root_id",impl->root_id);out.pushKV("root_document",impl->root_document);
    out.pushKV("available",impl->tracker.has_value()&&impl->error.empty());out.pushKV("error",impl->error);out.pushKV("unique_authority_order_proven",false);
    if(impl->tracker){const auto state=impl->tracker->Snapshot();out.pushKV("policy_id",state.id);out.pushKV("epoch",impl->tracker->Current().epoch);out.pushKV("conflicted",state.conflicted);for(const auto& script:state.moderators)moderators.push_back(script);
        if(!state.conflicted)for(const auto& [id,mask]:state.ratified){UniValue row(UniValue::VOBJ);row.pushKV("action_id",id);row.pushKV("channel",mask.channel);row.pushKV("target_event",mask.target_event);masks.push_back(row);}
    }
    out.pushKV("moderators",moderators);out.pushKV("masks",masks);
    const bool reset=cursor>impl->journal.size();if(reset)cursor=0;size_t budget=0;uint64_t next=cursor;
    while(next<impl->journal.size()&&records.size()<limit){const auto row=Encode(impl->journal[next]);const auto size=row.write().size();if(budget+size>23500)break;records.push_back(row);budget+=size;++next;}
    out.pushKV("records",records);out.pushKV("next_cursor",next);out.pushKV("more",next<impl->journal.size());out.pushKV("reset",reset);out.pushKV("journal_bytes",uint64_t(impl->bytes));out.pushKV("journal_limit",uint64_t(MAX_JOURNAL_BYTES));
    out.pushKV("limitation","Private authority lineage only; hide preserves proof and cannot erase hostile/legacy copies");return out;
}
}
