// Copyright (c) 2026 The PGN developers. MIT.
#pragma once
#include <pgn/social_identity_committed.h>
#include <pgn/social_storage.h>
#include <dbwrapper.h>
#include <mutex>
#include <tuple>
#include <memory>
#include <limits>

namespace pgn::social::identity::committed {
// This index contains UNTRUSTED candidates at authenticated Native positions.
// It grants no name/permission. Signature and lineage verification is per S;
// a flood targeting S must not spend the verification budget of another S.
struct CandidateRef {int32_t height{};uint32_t output{};uint256 block;};
struct CandidateAnchor {int32_t height{-1};uint256 hash;};
struct CandidatePage {
    CandidateAnchor anchor;uint64_t generation{};std::vector<CandidateRef> refs;
    std::string next;bool complete{};
};
struct IndexReply {std::string state,reason;size_t candidates{};};
class CandidateIndex {
    using Key=std::pair<uint8_t,std::string>;
    struct Meta {std::string genesis;int32_t height{-1};uint256 hash;uint64_t generation{};
        SERIALIZE_METHODS(Meta,obj){READWRITE(obj.genesis,obj.height,obj.hash,obj.generation);}};
    struct Ref {int32_t height{};uint32_t output{};uint256 hash;
        SERIALIZE_METHODS(Ref,obj){READWRITE(obj.height,obj.output,obj.hash);}};
    struct Undo {int32_t height{-1};uint256 hash;std::vector<Key> keys;
        SERIALIZE_METHODS(Undo,obj){READWRITE(obj.height,obj.hash,obj.keys);}};
    const fs::path path_;const uint256 chain_;
    std::unique_ptr<CDBWrapper> db_;mutable std::mutex mutex_;
    CandidateAnchor tip_;uint64_t generation_{};bool uncertain_{};
    static Key MetaKey(){return {'m',"native-link2-candidates"};}
    static std::string Number(uint64_t n){auto s=std::to_string(n);Require(s.size()<=10,"Native position overflow");return std::string(10-s.size(),'0')+s;}
    static std::string Position(int32_t h,uint32_t out){Require(h>=0,"Native height required");return Number(uint32_t(h))+Number(out);}
    static Key UndoKey(int32_t h){return {'u',Number(uint32_t(h))};}
    Meta Metadata(const CandidateAnchor& tip,uint64_t gen)const{return {chain_.GetHex(),tip.height,tip.hash,gen};}
    void Open(){
        db_=std::make_unique<CDBWrapper>(DBParams{.path=path_,.cache_bytes=2*1024*1024,.memory_only=false,.wipe_data=false});
        Meta meta;
        if(db_->Read(MetaKey(),meta)){
            Require(meta.genesis==chain_.GetHex()&&meta.height>=-1,"Candidate index context mismatch");
            tip_={meta.height,meta.hash};generation_=meta.generation;
        }else{Require(!db_->Exists(MetaKey()),"Candidate index metadata corruption");tip_={};generation_=0;CDBBatch batch(*db_);batch.Write(MetaKey(),Metadata(tip_,generation_));CommitSocialBatch(*db_,batch);}
    }
    std::optional<std::string> Candidate(const CTxOut& out)const{
        if(out.nValue!=0||out.scriptPubKey.size()>MaxFrame)return {};
        const auto& script=out.scriptPubKey;auto it=script.begin();opcodetype op;std::vector<unsigned char> tag,payload;
        if(!script.GetOp(it,op)||op!=OP_RETURN||!script.GetOp(it,op,tag)||std::string(tag.begin(),tag.end())!=Tag||!script.GetOp(it,op,payload)||it!=script.end())return {};
        const std::string source(payload.begin(),payload.end());UniValue value;
        if(!value.read(source)||!value.isObject()||!value["statement"].isObject())return {};
        const auto& statement=value["statement"];
        if(!statement["v"].isNum()||statement["v"].getInt<int>()!=2||!statement["genesis"].isStr()||statement["genesis"].get_str()!=chain_.GetHex()||!statement["scope"].isStr()||statement["scope"].get_str()!=Scope||!statement["social_script"].isStr())return {};
        const auto s=statement["social_script"].get_str();if(!Hex(s,44)||!s.starts_with("0014"))return {};
        // No BIP322 here. The resolver must load this exact authenticated
        // coinbase, run Verify(), and apply the signed per-S parent lineage.
        return s;
    }
public:
    CandidateIndex(fs::path path,uint256 chain):path_(std::move(path)),chain_(chain){Open();}
    CandidateAnchor Tip()const{std::lock_guard lock(mutex_);return tip_;}
    uint64_t Generation()const{std::lock_guard lock(mutex_);return generation_;}
    IndexReply Append(const pgn::core::VerifiedNativeRequestBlock& block,const std::function<bool(const uint256&,int32_t)>& canonical){
        Require(bool(canonical)&&block.Chain()==chain_&&block.Height()>=0&&block.Coinbase(),"Verified Native block required");
        Require(canonical(block.Block(),block.Height()),"Candidate block is not canonical");
        std::vector<std::pair<Key,Ref>> refs;
        for(uint32_t output=0;output<block.Coinbase()->vout.size();++output){
            try{if(const auto s=Candidate(block.Coinbase()->vout[output]))refs.emplace_back(Key{'r',*s+Position(block.Height(),output)},Ref{block.Height(),output,block.Block()});}
            catch(const std::runtime_error&){/* malformed metadata carries no authority; resource failures propagate */}
        }
        std::unique_lock lock(mutex_);if(uncertain_)return {"uncertain","Candidate index WAL recovery required",0};
        if(block.Height()<=tip_.height){const auto current=tip_;lock.unlock();Require(canonical(current.hash,current.height),"Candidate index needs Native rollback");return {"already_indexed","Retained canonical block",0};}
        Require(block.Height()==tip_.height+1,"Contiguous Native candidate indexing required");
        const auto previous=tip_;const auto gen=generation_;lock.unlock();
        Require(previous.height<0||canonical(previous.hash,previous.height),"Candidate index needs Native rollback");
        lock.lock();Require(tip_.height==previous.height&&tip_.hash==previous.hash&&generation_==gen,"Concurrent candidate index change");
        try{
            CDBBatch batch(*db_);std::vector<Key> keys;keys.reserve(refs.size());
            for(const auto& [key,ref]:refs){batch.Write(key,ref);keys.push_back(key);}
            const CandidateAnchor next{block.Height(),block.Block()};batch.Write(UndoKey(next.height),Undo{previous.height,previous.hash,std::move(keys)});batch.Write(MetaKey(),Metadata(next,generation_));
            CommitSocialBatch(*db_,batch);tip_=next;lock.unlock();
            if(!canonical(block.Block(),block.Height()))return {"reorg","Committed index anchor became noncanonical; no name may be inferred",refs.size()};
            return {"indexed","Untrusted candidates indexed; signatures not admitted",refs.size()};
        }catch(const StorageCommitUncertain&){uncertain_=true;return {"uncertain","Candidate index WAL outcome requires reopen",0};}
    }
    CandidatePage Read(const std::string& s,const CandidateAnchor& anchor,const std::string& after,size_t limit,const std::function<bool(const uint256&,int32_t)>& canonical)const{
        Require(Hex(s,44)&&s.starts_with("0014")&&limit>0&&limit<=100&&bool(canonical),"Bounded canonical-script candidate page required");
        Require(after.empty()||(after.size()==20&&std::all_of(after.begin(),after.end(),[](char c){return c>='0'&&c<='9';})),"Malformed candidate continuation");
        Require(anchor.height>=0&&canonical(anchor.hash,anchor.height),"Candidate page anchor is not canonical");
        std::unique_lock lock(mutex_);Require(!uncertain_&&tip_.height>=anchor.height,"Candidate index is not ready");
        const auto observed_tip=tip_;const auto gen=generation_;lock.unlock();
        Require(canonical(observed_tip.hash,observed_tip.height),"Candidate index branch requires rollback");
        lock.lock();Require(!uncertain_&&generation_==gen,"Candidate page generation changed");
        // std::string has a serialized length prefix: seek the full64-byte
        // key, not a shorter raw prefix that would sort before every S.
        CandidatePage page{anchor,gen};auto iterator=std::unique_ptr<CDBIterator>(db_->NewIterator());iterator->Seek(Key{'r',s+(after.empty()?std::string(20,'0'):after)});
        bool more=false;
        while(iterator->Valid()){
            Key key;if(!iterator->GetKey(key)||key.first!='r'||!key.second.starts_with(s))break;
            Require(key.second.size()==64,"Candidate key corruption");const auto position=key.second.substr(44);
            if(!after.empty()&&position<=after){iterator->Next();continue;}
            Ref ref;Require(iterator->GetValue(ref),"Candidate reference corruption");
            const auto [height,output,hash]=ref;Require(position==Position(height,output),"Candidate position corruption");
            if(height>anchor.height)break;
            if(page.refs.size()==limit){more=true;break;}
            page.refs.push_back({height,output,hash});page.next=position;iterator->Next();
        }
        page.complete=!more;if(page.complete)page.next.clear();lock.unlock();
        Require(canonical(anchor.hash,anchor.height),"Candidate page anchor reorganized");
        lock.lock();Require(!uncertain_&&generation_==gen,"Candidate page invalidated by rollback");return page;
    }
    IndexReply RollbackOne(){
        std::lock_guard lock(mutex_);if(uncertain_)return {"uncertain","Candidate index WAL recovery required",0};
        Require(tip_.height>=0,"No candidate block to roll back");Undo undo;Require(db_->Read(UndoKey(tip_.height),undo),"Native candidate undo unavailable");
        const auto& [height,hash,keys]=undo;Require(height==tip_.height-1,"Candidate undo cursor corruption");
        try{CDBBatch batch(*db_);for(const auto& key:keys)batch.Erase(key);batch.Erase(UndoKey(tip_.height));const CandidateAnchor next{height,hash};batch.Write(MetaKey(),Metadata(next,generation_+1));CommitSocialBatch(*db_,batch);tip_=next;++generation_;return {"rolled_back","Native candidate branch reverted",keys.size()};}
        catch(const StorageCommitUncertain&){uncertain_=true;return {"uncertain","Candidate rollback WAL requires reopen",0};}
    }
    IndexReply Recover(){
        std::lock_guard lock(mutex_);try{db_.reset();Open();uncertain_=false;return {"reopened","Native cursor reloaded; caller resumes the same authenticated block",0};}
        catch(const std::exception& e){uncertain_=true;return {"uncertain",e.what(),0};}
    }
};
// The native archive remains the proof source. This CDB index stores one compact
// reference per on-chain candidate, not a world-sized RAM map or a global
// admission quota. Undo/reference disk growth follows indexed Native history;
// pruning/checkpoint rebuild and a typed name resolver are separate gates.
}
