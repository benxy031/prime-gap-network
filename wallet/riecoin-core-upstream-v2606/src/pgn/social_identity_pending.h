// Copyright (c) 2026 The PGN developers. MIT.
// Transient signed-display submission queue, NOT Native state or role authority.
// Limits must be supplied explicitly by the adapter/qualification policy.
#pragma once
#include <pgn/social_identity_prepare.h>
#include <pgn/social_storage.h>
#include <dbwrapper.h>
#include <mutex>
#include <list>
#include <map>
#include <memory>
namespace pgn::social::identity::committed {
struct PendingLimits {size_t bytes,per_operator;};
struct PendingReply {std::string state,reason,event_id;bool signature_cached{};};
struct PendingConsent {std::string id,operator_id,social_script,source;};
class PendingAssociations {
    using Key=std::pair<uint8_t,std::string>;
    struct Entry {std::string id,operator_id,social_script,source;
        SERIALIZE_METHODS(Entry,obj){READWRITE(obj.id,obj.operator_id,obj.social_script,obj.source);}};
    const fs::path path_;const std::string genesis_;const PendingLimits limits_;leveldb::Env* environment_{};
    std::unique_ptr<CDBWrapper> db_;std::mutex mutex_;size_t bytes_{};
    std::map<std::string,size_t> operators_;
    // RAM-only proof memo, exact signed bytes, discarded on restart/recovery.
    // It never substitutes for current Native lineage/context validation.
    std::list<std::string> proofs_;std::set<std::string> proof_set_;
    bool uncertain_{};std::string turn_;std::map<std::string,std::string> operator_turn_;
    static Key Record(const std::string& id){return {'p',id};}
    static Key Ordered(const std::string& o,const std::string& id){return {'o',o+id};}
    static Key Subject(const std::string& s){return {'s',s};}
    static size_t Charge(const Entry& e){return e.source.size()+e.id.size()+e.operator_id.size()+e.social_script.size()+256;}
    static void Check(const Entry& e){Require(Hex(e.id,64)&&Hex(e.operator_id,64)&&Hex(e.social_script,44)&&e.social_script.starts_with("0014")&&e.source.size()<=MaxFrame,"Pending candidate metadata corrupt");}
    void Remember(const std::string& digest){
        if(proof_set_.contains(digest)){proofs_.remove(digest);proofs_.push_back(digest);return;}
        if(proofs_.size()>=512){proof_set_.erase(proofs_.front());proofs_.pop_front();}
        proofs_.push_back(digest);proof_set_.insert(digest);
    }
    void Open(){
        db_=std::make_unique<CDBWrapper>(DBParams{.path=path_,.cache_bytes=1024*1024,.memory_only=false,.wipe_data=false,.testing_env=environment_});
        const Key context{'c',"genesis"};std::string saved;
        if(db_->Read(context,saved))Require(saved==genesis_,"Pending queue network mismatch");
        else {Require(!db_->Exists(context),"Pending context corruption");CDBBatch batch(*db_);batch.Write(context,genesis_);CommitSocialBatch(*db_,batch);}
        bytes_=0;operators_.clear();operator_turn_.clear();proofs_.clear();proof_set_.clear();turn_.clear();
        auto iterator=std::unique_ptr<CDBIterator>(db_->NewIterator());iterator->Seek(Record(std::string(64,'0')));
        while(iterator->Valid()){
            Key key;if(!iterator->GetKey(key)||key.first!='p')break;Entry entry;Require(iterator->GetValue(entry),"Pending candidate absent");Check(entry);Require(key.second==entry.id,"Pending ID mismatch");const auto decoded=DecodeConsentCertificate(entry.source,genesis_);Require(decoded.id==entry.id&&decoded.operator_id==entry.operator_id&&decoded.social_script==entry.social_script,"Pending source/metadata mismatch");
            Require(bytes_<=limits_.bytes&&Charge(entry)<=limits_.bytes-bytes_,"Pending byte budget exceeded; explicit recovery required");bytes_+=Charge(entry);++operators_[entry.operator_id];iterator->Next();
        }
        // Every loaded proof remains UNVERIFIED until a real Native validator
        // checks its signature and current context again; no persisted bool.
    }
public:
    PendingAssociations(fs::path path,std::string genesis,PendingLimits limits,leveldb::Env* environment=nullptr):path_(std::move(path)),genesis_(std::move(genesis)),limits_(limits),environment_(environment){Require(Hex(genesis_,64)&&limits_.bytes>=MaxFrame&&limits_.bytes<=16*1024*1024&&limits_.per_operator>0&&limits_.per_operator<=256,"Explicit bounded transient queue policy required");Open();}
    // validate(cached) MUST always recheck actual Native source/head. cached
    // only means this exact signed source previously passed crypto in RAM.
    // Neither bool nor Native state is accepted from a remote caller.
    PendingReply Submit(const std::string& source,const std::function<SubmissionConsent(bool)>& validate){
        Require(source.size()<=MaxFrame&&bool(validate),"Bounded signed candidate/Native validator required");
        const auto digest=Hash("PGN/SOCIAL/LINK-SUBMISSION-BYTES/2\n"+source);bool cached{};
        {std::lock_guard lock(mutex_);if(uncertain_)return {"uncertain","Queue recovery required; operation preserved",{},false};cached=proof_set_.contains(digest);}
        // No CDB lock during Native reads/BIP322. Exceptions leave every
        // existing queued operation untouched, notably a pending withdrawal.
        const auto checked=validate(cached);Require(checked.certificate.source==source&&checked.coverage.complete,"Complete proof bound to exact signed bytes required");
        const auto& c=checked.certificate;Require(c.statement["genesis"].get_str()==genesis_,"Queue candidate network mismatch");
        if(checked.already_included)return {"included","Observed in canonical Native history",c.id,cached};
        const Entry entry{c.id,c.operator_id,c.social_script,source};Check(entry);
        std::lock_guard lock(mutex_);if(uncertain_)return {"uncertain","Queue recovery required; operation preserved",c.id,cached};
        Entry present;if(db_->Read(Record(c.id),present)){Check(present);Require(present.operator_id==c.operator_id&&present.social_script==c.social_script,"Pending content-ID conflict");try{Remember(digest);}catch(const std::bad_alloc&){return {"uncertain","Pending operation is durable; proof memo unavailable, retry verification",c.id,cached};}return {"pending","Already durably queued; Native inclusion not observed",c.id,cached};}
        std::string current;if(db_->Read(Subject(c.social_script),current)&&current!=c.id)return {"conflict","Another signed operation for this S is pending; preserve both user packets",c.id,cached};
        const auto count=operators_.find(c.operator_id);if(count!=operators_.end()&&count->second>=limits_.per_operator)return {"busy","This operator's transient queue turn is full; other operators remain available",c.id,cached};
        if(bytes_>limits_.bytes||Charge(entry)>limits_.bytes-bytes_)return {"busy","Transient queue byte budget reached; caller retains exact signed operation",c.id,cached};
        try{
            CDBBatch batch(*db_);batch.Write(Record(c.id),entry);batch.Write(Ordered(c.operator_id,c.id),c.id);batch.Write(Subject(c.social_script),c.id);CommitSocialBatch(*db_,batch);
            bytes_+=Charge(entry);++operators_[c.operator_id];Remember(digest);return {"pending","Durably queued; no name/right before canonical inclusion",c.id,cached};
        }catch(const std::exception&){uncertain_=true;return {"uncertain","Queue WAL/publication outcome requires reopen; keep exact signed operation",c.id,cached};}
    }
    std::vector<PendingConsent> FairPage(size_t limit){
        Require(limit>0&&limit<=100,"Bounded transient queue page required");std::lock_guard lock(mutex_);Require(!uncertain_,"Queue recovery required");std::vector<PendingConsent> out;if(operators_.empty())return out;
        auto next=operators_.upper_bound(turn_);if(next==operators_.end())next=operators_.begin();const auto first=next->first;
        do{
            const auto o=next->first;auto iterator=std::unique_ptr<CDBIterator>(db_->NewIterator());const auto previous=operator_turn_.find(o);
            iterator->Seek(Ordered(o,previous==operator_turn_.end()?std::string(64,'0'):previous->second));
            Key key;std::string id;
            if(previous!=operator_turn_.end()&&iterator->Valid()&&iterator->GetKey(key)&&key==Ordered(o,previous->second))iterator->Next();
            if(!iterator->Valid()||!iterator->GetKey(key)||key.first!='o'||key.second.size()!=128||!key.second.starts_with(o))iterator->Seek(Ordered(o,std::string(64,'0')));
            Require(iterator->Valid()&&iterator->GetKey(key)&&key.first=='o'&&key.second.size()==128&&key.second.starts_with(o)&&iterator->GetValue(id),"Pending operator index absent");operator_turn_[o]=id;
            Entry entry;Require(db_->Read(Record(id),entry),"Pending ordered record absent");Check(entry);out.push_back({entry.id,entry.operator_id,entry.social_script,entry.source});turn_=o;
            if(out.size()==limit)break;++next;if(next==operators_.end())next=operators_.begin();
        }while(next->first!=first);
        return out;
    }
    // Only a completed actual Native target result can remove an operation.
    // Context absence, disk failures and incomplete history NEVER consume it.
    PendingReply Observe(const std::string& id,const pgn::core::NativeLeaderboardSnapshot& native,const std::optional<State>& current,bool complete,const std::function<bool(const uint256&,int32_t)>& canonical){
        Require(Hex(id,64),"Canonical queued operation ID required");
        if(native.genesis.GetHex()!=genesis_||!complete||!canonical||!canonical(native.tip,native.height))return {"uncertain","Native target context unavailable; operation preserved",id,false};
        std::lock_guard lock(mutex_);if(uncertain_)return {"uncertain","Queue recovery required",id,false};Entry entry;if(!db_->Read(Record(id),entry))return {"absent","No pending operation",id,false};Check(entry);
        if(current&&(current->social_script!=entry.social_script||current->committed.height>native.height||!canonical(current->committed.block,current->committed.height)))return {"uncertain","Native lineage context unavailable; operation preserved",id,false};
        // Only exact inclusion retires this first bounded increment. Conflicts
        // remain visible/preserved until explicit successor/cancellation policy.
        if(!current||current->head!=id)return {"pending","Exact Native inclusion not observed",id,false};
        try{CDBBatch batch(*db_);batch.Erase(Record(id));batch.Erase(Ordered(entry.operator_id,id));batch.Erase(Subject(entry.social_script));CommitSocialBatch(*db_,batch);bytes_-=Charge(entry);auto at=operators_.find(entry.operator_id);Require(at!=operators_.end()&&at->second>0,"Pending operator accounting absent");if(!--at->second){operator_turn_.erase(entry.operator_id);operators_.erase(at);}return {"included","Exact canonical Native operation observed",id,false};}
        catch(const std::exception&){uncertain_=true;return {"uncertain","Queue WAL/publication outcome requires reopen",id,false};}
    }
    PendingReply Recover(){std::lock_guard lock(mutex_);try{db_.reset();Open();uncertain_=false;return {"reopened","Pending raw proofs reloaded; signatures/context require fresh verification",{},false};}catch(const std::exception&){uncertain_=true;return {"uncertain","Pending recovery unavailable; keep user packet",{},false};}}
    size_t Count(){std::lock_guard lock(mutex_);size_t count=0;for(const auto& [id,n]:operators_)count+=n;return count;}
    size_t Bytes(){std::lock_guard lock(mutex_);return bytes_;}
};
}
