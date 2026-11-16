// Copyright (c) 2026 The PGN developers. MIT; see COPYING.
#pragma once
#include <pgn/social_identity_certificate.h>
#include <pgn/social_storage.h>
#include <dbwrapper.h>
#include <memory>
#include <mutex>
#include <list>
#include <functional>
#include <limits>
#include <random.h>
#include <atomic>

namespace pgn::social::display {
class ContextUnavailable:public std::runtime_error{public:using std::runtime_error::runtime_error;};
using Certificate=identity::committed::ConsentCertificate;
using identity::committed::DecodeConsentCertificate;
using identity::committed::CheckConsentProofs;
struct Context {
 std::string genesis,tip;int32_t height{-1};
 std::function<bool(int32_t,const std::string&)> canonical;
 std::function<std::pair<std::string,std::string>(const std::string&)> native;
};
using Source=std::function<Context()>;
struct Result {std::string state,reason,id;bool appended{};};
struct Head {std::string state{"unlinked"},label,id,native,op;};
// Public proofs remain in the existing append-only e/sequence journal. These
// constant-size indexes replace the per-S vector; no lifetime update quota.
struct DisplayNode {
 uint64_t sequence{},replica_sequence{};std::string first_child,second_child;bool attached{};
 SERIALIZE_METHODS(DisplayNode,obj){READWRITE(obj.sequence,obj.replica_sequence,obj.first_child,obj.second_child,obj.attached);}
};
struct DisplayState {
 std::string root,head,conflict_a,conflict_b;uint64_t detached{};bool pending{};
 SERIALIZE_METHODS(DisplayState,obj){READWRITE(obj.root,obj.head,obj.conflict_a,obj.conflict_b,obj.detached,obj.pending);}
};
class Store {
 using Key=std::pair<uint8_t,std::string>;
 fs::path path_;std::string genesis_;Source source_;std::unique_ptr<CDBWrapper> db_;std::mutex mutex_;
 uint64_t sequence_{},pending_namespaces_{},replica_sequence_{},work_head_{},work_tail_{};std::atomic<uint64_t> logical_bytes_{};std::string generation_,replica_generation_;
 std::map<std::string,std::string> crypto_;std::list<std::string> crypto_order_;
 static std::string Number(uint64_t n){auto s=std::to_string(n);return std::string(20-s.size(),'0')+s;}
 void Proof(const Certificate& p){
  const auto digest=identity::Hash(p.source);const auto found=crypto_.find(p.id);
  if(found!=crypto_.end()&&found->second==digest)return;
  CheckConsentProofs(p,true);const bool known=found!=crypto_.end();
  if(!known&&crypto_.size()>=8192){crypto_.erase(crypto_order_.front());crypto_order_.pop_front();}
  if(!known)crypto_order_.push_back(p.id);crypto_[p.id]=digest;
 }
 Certificate Read(const std::string& id,DisplayNode* output=nullptr){
  DisplayNode node;std::string raw;
  if(!db_->Read(Key{'n',id},node)||!node.sequence||!db_->Read(Key{'e',Number(node.sequence)},raw))
   throw ContextUnavailable("Display proof index incomplete; no reset inferred");
  auto p=DecodeConsentCertificate(raw,genesis_);if(p.id!=id)throw ContextUnavailable("Display proof index identity mismatch");Proof(p);
  if(output)*output=node;return p;
 }
 static bool Related(const Certificate& child,const Certificate& parent){
  return child.social_script==parent.social_script&&child.operator_script==parent.operator_script&&child.operator_id==parent.operator_id;
 }
 void Current(const Certificate& p,const Context& c){
  if(c.genesis!=genesis_||c.height<0||!c.canonical||!c.native||!c.canonical(c.height,c.tip)||!c.canonical(p.anchor_height,p.anchor))
   throw ContextUnavailable("Current display Native context unavailable or reorganized");
  const auto [op,name]=c.native(p.operator_script);
  if(op!=p.operator_id)throw std::runtime_error("Native K does not map to signed O in current snapshot");
  if(p.action=="bind"&&name.empty())throw ContextUnavailable("Native public name is not yet authenticated; keep the signed packet");
 }
 // All newly admitted descendants require their parent. The wire and signed
 // bytes are unchanged. Missing parent is transient, does not mutate a name,
 // and is never acknowledged as durable admission by the P2P caller.
 bool Parent(const Certificate& p,DisplayNode& node,Certificate& parent){
  if(p.previous==std::string(64,'0'))return true;
  if(!db_->Read(Key{'n',p.previous},node)||!node.attached)return false;
  parent=Read(p.previous);
  if(parent.social_script!=p.social_script)throw std::runtime_error("Display parent belongs to another Social identity");
  if(p.action=="withdraw"&&!Related(p,parent))throw std::runtime_error("Withdrawal cannot borrow another Native binding");
  return true;
 }
 void Index(const Certificate& p,uint64_t seq,bool pending,bool migration){
  DisplayState state;db_->Read(Key{'m',p.social_script},state);
  DisplayNode parent,node;const bool indexed=db_->Read(Key{'n',p.id},node);if(indexed&&node.attached)return;
  Certificate parent_proof;const bool attached=Parent(p,parent,parent_proof);
  if(!attached&&!migration)throw ContextUnavailable("Binding parent required before display admission");
  node.sequence=seq;node.attached=attached;
  CDBBatch b(*db_);
  if(!attached){if(!indexed)++state.detached;b.Write(Key{'w',p.previous+p.id},p.id);}
  else if(p.previous==std::string(64,'0')){
   if(state.root.empty()){state.root=state.head=p.id;}
   else if(state.root!=p.id&&state.conflict_a.empty()){state.conflict_a=state.root;state.conflict_b=p.id;}
  }else{
   if(state.root.empty())throw ContextUnavailable("Display root index missing; no authority inferred");
   if(parent.first_child.empty()){
    if(state.head!=p.previous&&state.conflict_a.empty())throw ContextUnavailable("Display parent/head index inconsistent");
    parent.first_child=p.id;
    if(state.head==p.previous&&state.conflict_a.empty())state.head=p.id;
   }else if(parent.first_child!=p.id){
    if(parent.second_child.empty())parent.second_child=p.id;
    if(state.conflict_a.empty()){state.conflict_a=parent.first_child;state.conflict_b=p.id;}
   }
   b.Write(Key{'n',p.previous},parent);
  }
  if(attached){
   if(indexed&&state.detached)--state.detached;
   b.Erase(Key{'w',p.previous+p.id});
   node.replica_sequence=replica_sequence_+1;
   b.Write(Key{'r',Number(node.replica_sequence)},p.id);
   b.Write(std::string("replica_sequence"),node.replica_sequence);
   b.Write(Key{'q',Number(work_tail_+1)},p.id);b.Write(std::string("work_tail"),work_tail_+1);
  }
  const bool new_pending=pending&&!state.pending;if(new_pending)state.pending=true;
  if(new_pending)b.Write(std::string("indexed_pending_namespaces"),pending_namespaces_+1);
  b.Write(Key{'n',p.id},node);b.Write(Key{'m',p.social_script},state);
  if(!migration){b.Write(Key{'e',Number(seq)},p.source);b.Write(std::string("sequence"),seq);}
  CommitSocialBatch(*db_,b);
  if(attached){++replica_sequence_;++work_tail_;}
  if(new_pending)++pending_namespaces_;
  if(!migration){sequence_=seq;logical_bytes_+=p.source.size()+p.id.size()+p.previous.size()+p.social_script.size()+192;}
 }
 void Drain(){
  // Disk work queue: descendants are attached parent-first without recursion,
  // an in-memory graph or a depth/lifetime quota. Queue/cursor writes are atomic.
  while(work_head_<work_tail_){
   std::string parent;if(!db_->Read(Key{'q',Number(work_head_+1)},parent))throw ContextUnavailable("Display migration work item missing");
   auto it=std::unique_ptr<CDBIterator>(db_->NewIterator());it->Seek(Key{'w',parent});
   for(;it->Valid();it->Next()){
    Key key;std::string id;if(!it->GetKey(key)||key.first!='w'||!key.second.starts_with(parent))break;
    if(!it->GetValue(id))throw ContextUnavailable("Display pending child unreadable");
    DisplayNode node;if(!db_->Read(Key{'n',id},node))throw ContextUnavailable("Display pending child index missing");
    if(!node.attached){const auto p=Read(id);Index(p,node.sequence,false,true);}
   }
   CDBBatch done(*db_);done.Erase(Key{'q',Number(work_head_+1)});done.Write(std::string("work_head"),work_head_+1);CommitSocialBatch(*db_,done);++work_head_;
  }
 }
 void Migrate(){
  // No public display store was shipped. Preserve private retained proofs,
  // including withdrawals, from its existing journal. Each atomic index write
  // is restart-idempotent; source bytes/sequence/generation never change.
  auto it=std::unique_ptr<CDBIterator>(db_->NewIterator());it->Seek(Key{'e',Number(0)});
  for(;it->Valid();it->Next()){
   Key key;std::string raw;if(!it->GetKey(key)||key.first!='e')break;
   if(!it->GetValue(raw))throw ContextUnavailable("Private display migration source unreadable");
   auto p=DecodeConsentCertificate(raw,genesis_);Proof(p);DisplayNode existing;
   if(db_->Read(Key{'n',p.id},existing))continue;
   Index(p,std::stoull(key.second),false,true);
  }
  Drain();CDBBatch done(*db_);done.Write(std::string("display_index_schema"),uint32_t{3});CommitSocialBatch(*db_,done);
 }
 void Open(){
  if(db_)return;
  db_=std::make_unique<CDBWrapper>(DBParams{.path=path_,.cache_bytes=2*1024*1024,.memory_only=false,.wipe_data=false});
  try{
  std::string genesis;
  if(db_->Read(std::string("genesis"),genesis)){
   if(genesis!=genesis_)throw std::runtime_error("Display store genesis mismatch");
   if(!db_->Read(std::string("sequence"),sequence_)||!db_->Read(std::string("generation"),generation_)||!identity::Hex(generation_,64))
    throw ContextUnavailable("Display metadata incomplete; no reset inferred");
  }else{
   generation_=GetRandHash().GetHex();CDBBatch b(*db_);b.Write(std::string("genesis"),genesis_);b.Write(std::string("sequence"),uint64_t{});b.Write(std::string("generation"),generation_);b.Write(std::string("display_index_schema"),uint32_t{3});CommitSocialBatch(*db_,b);
  }
  pending_namespaces_=0;db_->Read(std::string("indexed_pending_namespaces"),pending_namespaces_);
  replica_sequence_=work_head_=work_tail_=0;
  db_->Read(std::string("replica_sequence"),replica_sequence_);db_->Read(std::string("work_head"),work_head_);db_->Read(std::string("work_tail"),work_tail_);
  if(!db_->Read(std::string("replica_generation"),replica_generation_)){
   replica_generation_=GetRandHash().GetHex();CDBBatch init(*db_);init.Write(std::string("replica_generation"),replica_generation_);CommitSocialBatch(*db_,init);
  }
  if(!identity::Hex(replica_generation_,64)||work_head_>work_tail_||replica_sequence_>sequence_)throw ContextUnavailable("Display replica metadata inconsistent; proofs preserved");
  uint32_t schema{};if(!db_->Read(std::string("display_index_schema"),schema))Migrate();else if(schema!=3)throw ContextUnavailable("Unsupported display index schema; proofs preserved");Drain();
  }
  catch(...){db_.reset();throw;}
 }
 Head Resolve(const std::string& s,DisplayState& state){
  if(state.root.empty())return {state.detached?"pending_parent":"unlinked"};
  const auto root=Read(state.root);if(root.social_script!=s||root.previous!=std::string(64,'0')||root.action!="bind")throw ContextUnavailable("Display root proof mismatch");
  if(!state.conflict_a.empty()){
   const auto a=Read(state.conflict_a),b=Read(state.conflict_b);
   if(a.social_script!=s||b.social_script!=s||a.id==b.id||a.previous!=b.previous)throw ContextUnavailable("Display conflict evidence mismatch");
   return {"conflict"};
  }
  if(state.detached)return {"pending_parent"};
  DisplayNode node;const auto at=Read(state.head,&node);
  if(at.social_script!=s||!node.attached||!node.first_child.empty())throw ContextUnavailable("Display head index is stale or inconsistent");
  if(at.previous!=std::string(64,'0')){
   DisplayNode parent;const auto proof=Read(at.previous,&parent);
   if(proof.social_script!=s||parent.first_child!=at.id||!parent.second_child.empty()||(at.action=="withdraw"&&!Related(at,proof)))throw ContextUnavailable("Display head lineage mismatch");
  }
  if(at.action=="withdraw")return {"withdrawn",{},at.id,at.operator_script,at.operator_id};
  return {"linked",{},at.id,at.operator_script,at.operator_id};
 }
public:
 Store(fs::path path,std::string genesis,Source source):path_(std::move(path)),genesis_(std::move(genesis)),source_(std::move(source)){}
 Result Admit(const std::string& raw){
  Certificate p;try{p=DecodeConsentCertificate(raw,genesis_);}catch(const std::exception& e){return {"rejected",e.what()};}
  std::lock_guard guard(mutex_);
  try{
   Open();DisplayNode known;
   if(db_->Read(Key{'n',p.id},known)){const auto existing=Read(p.id);if(existing.source!=raw)Proof(p);Drain();return {"accepted_local",{},p.id,false};}
   DisplayNode parent;Certificate parent_proof;
   if(!Parent(p,parent,parent_proof))return {"pending_parent","Binding proof required before applying this transition",p.id};
   Proof(p);DisplayState state;db_->Read(Key{'m',p.social_script},state);bool pending=false;
   try{Current(p,source_());}catch(const ContextUnavailable&){pending=true;}catch(const UniValue&){pending=true;}
   if(pending&&!state.pending&&state.root.empty()&&pending_namespaces_>=128)return {"uncertain","Pending context inbox busy; current names unchanged",p.id};
   if(sequence_==std::numeric_limits<uint64_t>::max())return {"uncertain","Display journal sequence unavailable; existing state preserved",p.id};
   Index(p,sequence_+1,pending,false);Drain();
   return {pending?"pending_context":"accepted_local",pending?"Public proof retained; current Native context unavailable":std::string{},p.id,true};
  }catch(const StorageCommitUncertain& e){db_.reset();return {"uncertain",e.what(),p.id};}
   catch(const dbwrapper_error&){db_.reset();return {"uncertain","Display-only storage unavailable; preserve exact certificate",p.id};}
   catch(const ContextUnavailable& e){return {"uncertain",e.what(),p.id};}
   catch(const UniValue&){return {"uncertain","Current Native display context not ready",p.id};}
   catch(const std::exception& e){return {"rejected",e.what(),p.id};}
 }
 UniValue Lookup(const std::vector<std::string>& scripts){
  if(scripts.size()>100)throw std::runtime_error("At most100 display targets");
  std::lock_guard guard(mutex_);Open();const auto context=source_();
  UniValue out{UniValue::VOBJ},rows{UniValue::VARR};out.pushKV("schema","PGN_SOCIAL_CURRENT_DISPLAY_1");out.pushKV("genesis",genesis_);out.pushKV("bestblockhash",context.tip);out.pushKV("height",context.height);out.pushKV("current_display_only",true);out.pushKV("permissions_granted",false);out.pushKV("display_storage_schema","PGN_SOCIAL_DISPLAY_INDEX_3");
  for(size_t i=0;i<scripts.size();++i){
   UniValue row{UniValue::VOBJ};row.pushKV("input_index",i);row.pushKV("social_script",scripts[i]);
   try{
    if(!identity::Hex(scripts[i],44)||!scripts[i].starts_with("0014"))throw std::runtime_error("Invalid Social script");
    DisplayState state;db_->Read(Key{'m',scripts[i]},state);auto h=Resolve(scripts[i],state);
    if(h.state=="linked"){
     const auto proof=Read(h.id);Current(proof,context);h.label=context.native(proof.operator_script).second;
     if(h.label.empty())throw ContextUnavailable("Native public name unavailable");
    }
    row.pushKV("state",h.state);row.pushKV("complete",true);
    if(!h.id.empty()){row.pushKV("certificate_id",h.id);row.pushKV("operator_id",h.op);row.pushKV("operator_script",h.native);}
    if(h.state=="linked")row.pushKV("display_name",h.label);
    if(state.pending&&(h.state=="linked"||h.state=="withdrawn")){
     try{state.pending=false;CDBBatch b(*db_);b.Write(Key{'m',scripts[i]},state);b.Write(std::string("indexed_pending_namespaces"),pending_namespaces_?pending_namespaces_-1:0);CommitSocialBatch(*db_,b);if(pending_namespaces_)--pending_namespaces_;}
     catch(const StorageCommitUncertain&){row.pushKV("storage_uncertain",true);}
    }
   }catch(const std::exception& e){row.pushKV("state","unavailable");row.pushKV("complete",false);row.pushKV("reason",e.what());}
    catch(const UniValue&){row.pushKV("state","unavailable");row.pushKV("complete",false);row.pushKV("reason","Native display context unavailable");}
   rows.push_back(row);
  }out.pushKV("results",rows);return out;
 }
 UniValue Page(uint64_t cursor,const std::string& generation){
  std::lock_guard guard(mutex_);Open();if(cursor>replica_sequence_||(!generation.empty()&&generation!=replica_generation_))cursor=0;
  UniValue out{UniValue::VOBJ};out.pushKV("generation",replica_generation_);out.pushKV("next_cursor",cursor);out.pushKV("more",false);
  if(cursor==std::numeric_limits<uint64_t>::max())return out;
  auto it=std::unique_ptr<CDBIterator>(db_->NewIterator());it->Seek(Key{'r',Number(cursor+1)});Key key;std::string id,raw;DisplayNode node;
  if(it->Valid()&&it->GetKey(key)&&key.first=='r'&&it->GetValue(id)){
   if(!db_->Read(Key{'n',id},node)||!node.attached||!db_->Read(Key{'e',Number(node.sequence)},raw))throw ContextUnavailable("Display replica source missing");
   const auto seq=std::stoull(key.second);out.pushKV("certificate",raw);out.pushKV("next_cursor",seq);out.pushKV("more",seq<replica_sequence_);
  }return out;
 }
 uint64_t LogicalBytesSinceStart()const{return logical_bytes_.load();}
};
}
