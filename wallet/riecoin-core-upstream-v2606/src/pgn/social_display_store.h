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
struct Namespace {
 std::vector<std::string> certificates;bool reset{},pending{};
 SERIALIZE_METHODS(Namespace,obj){READWRITE(obj.certificates,obj.reset,obj.pending);}
};
class Store {
 fs::path path_;std::string genesis_;Source source_;std::unique_ptr<CDBWrapper> db_;std::mutex mutex_;
 uint64_t sequence_{},pending_namespaces_{};std::string generation_;std::map<std::string,std::string> crypto_;std::list<std::string> crypto_order_;
 static constexpr size_t PerIdentityRecords=16,PerIdentityBytes=65536; // PRIVATE first-lot policy, surfaced in RPC.
 using IdentityKey=std::pair<uint8_t,std::string>;
 static std::string Number(uint64_t n){auto s=std::to_string(n);return std::string(20-s.size(),'0')+s;}
 void Open(){
  if(db_)return;
  db_=std::make_unique<CDBWrapper>(DBParams{.path=path_,.cache_bytes=2*1024*1024,.memory_only=false,.wipe_data=false});
  std::string genesis;
  if(db_->Read(std::string("genesis"),genesis)){if(genesis!=genesis_)throw std::runtime_error("Display store genesis mismatch");if(!db_->Read(std::string("sequence"),sequence_)||!db_->Read(std::string("generation"),generation_)||!identity::Hex(generation_,64))throw std::runtime_error("Display metadata incomplete; no reset inferred");}
  else {generation_=GetRandHash().GetHex();CDBBatch b(*db_);b.Write(std::string("genesis"),genesis_);b.Write(std::string("sequence"),uint64_t{});b.Write(std::string("generation"),generation_);CommitSocialBatch(*db_,b);}
  db_->Read(std::string("pending_namespaces"),pending_namespaces_);
 }
 void Proof(const Certificate& p){
  const auto digest=identity::Hash(p.source);const auto found=crypto_.find(p.id);
  if(found!=crypto_.end()&&found->second==digest)return;
  CheckConsentProofs(p,true);
  const bool known=found!=crypto_.end();
  if(!known&&crypto_.size()>=8192){crypto_.erase(crypto_order_.front());crypto_order_.pop_front();}
  if(!known)crypto_order_.push_back(p.id);crypto_[p.id]=digest;
 }
 static bool RelatedWithdrawal(const Certificate& p,const Certificate& parent){
  return p.social_script==parent.social_script&&p.operator_script==parent.operator_script&&p.operator_id==parent.operator_id;
 }
 void Current(const Certificate& p,const Context& c){
  if(c.genesis!=genesis_||c.height<0||!c.canonical||!c.native||!c.canonical(c.height,c.tip)||!c.canonical(p.anchor_height,p.anchor))
   throw ContextUnavailable("Current display Native context unavailable or reorganized");
  const auto [op,name]=c.native(p.operator_script);
  if(op!=p.operator_id)throw std::runtime_error("Native K does not map to signed O in current snapshot");
  if(p.action=="bind"&&name.empty())throw ContextUnavailable("Native public name is not yet authenticated; keep the signed packet");
 }
 std::map<std::string,Certificate> Decode(const Namespace& ns,const std::string& s){
  std::map<std::string,Certificate> out;
  if(ns.certificates.size()>PerIdentityRecords)throw std::runtime_error("Per-S public proof bounds exceeded");
  size_t bytes{};
  for(const auto& raw:ns.certificates){bytes+=raw.size();if(bytes>PerIdentityBytes)throw std::runtime_error("Per-S public proof byte bound exceeded");
   auto p=DecodeConsentCertificate(raw,genesis_);if(p.social_script!=s)throw std::runtime_error("Display store target mismatch");Proof(p);if(!out.emplace(p.id,std::move(p)).second)throw std::runtime_error("Duplicate persisted display proof");
  }return out;
 }
 Head Resolve(const Namespace& ns,const std::map<std::string,Certificate>& records,const Context& c){
  if(ns.reset)return {"retention_reset"};
  std::map<std::string,std::vector<std::string>> children;std::vector<std::string> roots;
  for(const auto& [id,p]:records){
   if(p.previous==std::string(64,'0')){if(p.action=="bind")roots.push_back(id);continue;}
   const auto parent=records.find(p.previous);if(parent==records.end())continue;
   if(p.action=="withdraw"&&!RelatedWithdrawal(p,parent->second))continue;
   children[p.previous].push_back(id);
  }
  if(roots.empty())return {records.empty()?"unlinked":"pending"};if(roots.size()>1)return {"conflict"};
  std::string id=roots.front();std::set<std::string> seen;
  for(;;){if(!seen.insert(id).second)return {"conflict"};const auto& next=children[id];if(next.size()>1)return {"conflict"};if(next.empty())break;id=next.front();}
  for(const auto& [key,p]:records)if(p.action=="bind"&&p.previous!=std::string(64,'0')&&!records.contains(p.previous))return {"pending"};
  const auto& at=records.at(id);if(at.action=="withdraw")return {"withdrawn",{},id,at.operator_script,at.operator_id};
  Current(at,c);const auto [op,label]=c.native(at.operator_script);if(label.empty())return {"unavailable",{},id,at.operator_script,at.operator_id};
  return {"linked",label,id,at.operator_script,op};
 }
public:
 Store(fs::path path,std::string genesis,Source source):path_(std::move(path)),genesis_(std::move(genesis)),source_(std::move(source)){}
 Result Admit(const std::string& raw){
  Certificate p;
  try{p=DecodeConsentCertificate(raw,genesis_);}catch(const std::exception& e){return {"rejected",e.what()};}
  std::lock_guard guard(mutex_);
  try{
   Open();Namespace ns;db_->Read(IdentityKey{'s',p.social_script},ns);auto records=Decode(ns,p.social_script);
   // Existing exact records remain idempotent, including retired heads.
   if(records.contains(p.id)){if(records.at(p.id).source!=raw)Proof(p);return {"accepted_local",{},p.id,false};}
   Proof(p);
   if(p.action=="withdraw"){
    const auto parent=records.find(p.previous);
    if(parent==records.end())return {"pending_parent","Binding proof required before applying K withdrawal",p.id};
    if(!RelatedWithdrawal(p,parent->second))return {"rejected","Withdrawal cannot borrow another Native binding",p.id};
   }
   bool pending=false;
   try{const auto context=source_();Current(p,context);}
   catch(const ContextUnavailable&){pending=true;}
   catch(const UniValue&){pending=true;}
   // Context-held proofs are durable but grant NO display. A saturated pending
   // inbox never prevents a context-ready identity or known withdrawal.
   if(pending&&!ns.pending&&ns.certificates.empty()&&pending_namespaces_>=128)return {"uncertain","Pending context inbox busy; current verified names unaffected",p.id};
   if(ns.reset)return {"retention_reset","Only this S is held at the private retention boundary",p.id};
   size_t bytes=raw.size();for(const auto& source:ns.certificates)bytes+=source.size();
   if(ns.certificates.size()>=PerIdentityRecords||bytes>PerIdentityBytes){
    ns.reset=true;CDBBatch b(*db_);b.Write(IdentityKey{'s',p.social_script},ns);CommitSocialBatch(*db_,b);
    return {"retention_reset","Private per-S retention reached; Chat and other identities unaffected",p.id};
   }
   const bool new_pending=pending&&!ns.pending;if(new_pending)ns.pending=true;
   ns.certificates.push_back(raw);CDBBatch b(*db_);const auto seq=sequence_+1;
   if(new_pending)b.Write(std::string("pending_namespaces"),pending_namespaces_+1);
   b.Write(IdentityKey{'s',p.social_script},ns);b.Write(IdentityKey{'e',Number(seq)},raw);b.Write(std::string("sequence"),seq);CommitSocialBatch(*db_,b);sequence_=seq;
   if(new_pending)++pending_namespaces_;
   return {pending?"pending_context":"accepted_local",pending?"Public proof retained; current Native context unavailable":std::string{},p.id,true};
  }catch(const StorageCommitUncertain& e){db_.reset();return {"uncertain",e.what(),p.id};}
   catch(const ContextUnavailable& e){return {"uncertain",e.what(),p.id};}
   catch(const dbwrapper_error& e){db_.reset();return {"uncertain","Display-only storage unavailable; preserve exact certificate",p.id};}
   catch(const UniValue&){return {"uncertain","Current Native display context not ready",p.id};}
   catch(const std::exception& e){return {"rejected",e.what(),p.id};}
 }
 UniValue Lookup(const std::vector<std::string>& scripts){
  if(scripts.size()>100)throw std::runtime_error("At most100 display targets");
  std::lock_guard guard(mutex_);Open();const auto context=source_();
  UniValue out{UniValue::VOBJ},rows{UniValue::VARR};out.pushKV("schema","PGN_SOCIAL_CURRENT_DISPLAY_1");out.pushKV("genesis",genesis_);out.pushKV("bestblockhash",context.tip);out.pushKV("height",context.height);out.pushKV("current_display_only",true);out.pushKV("permissions_granted",false);out.pushKV("private_per_s_record_limit",PerIdentityRecords);
  for(size_t i=0;i<scripts.size();++i){
   UniValue row{UniValue::VOBJ};row.pushKV("input_index",i);row.pushKV("social_script",scripts[i]);
   try{
    if(!identity::Hex(scripts[i],44)||!scripts[i].starts_with("0014"))throw std::runtime_error("Invalid Social script");
    Namespace ns;db_->Read(IdentityKey{'s',scripts[i]},ns);const auto records=Decode(ns,scripts[i]);const auto h=Resolve(ns,records,context);
    row.pushKV("state",h.state);row.pushKV("complete",true);
    if(!h.id.empty()){row.pushKV("certificate_id",h.id);row.pushKV("operator_id",h.op);row.pushKV("operator_script",h.native);}
    if(h.state=="linked")row.pushKV("display_name",h.label);
    if(ns.pending&&(h.state=="linked"||h.state=="withdrawn")){
   try{ns.pending=false;CDBBatch promote(*db_);promote.Write(IdentityKey{'s',scripts[i]},ns);promote.Write(std::string("pending_namespaces"),pending_namespaces_?pending_namespaces_-1:0);CommitSocialBatch(*db_,promote);if(pending_namespaces_)--pending_namespaces_;}
   catch(const StorageCommitUncertain&){row.pushKV("storage_uncertain",true);}
    }
   }catch(const UniValue&){row.pushKV("state","unavailable");row.pushKV("complete",false);row.pushKV("reason","Native display context unavailable");}
    catch(const std::exception& e){row.pushKV("state","unavailable");row.pushKV("complete",false);row.pushKV("reason",e.what());}
   rows.push_back(row);
  }out.pushKV("results",rows);return out;
 }
 // Global disk cursor pages one bounded record; no all-history RAM copy.
 UniValue Page(uint64_t cursor,const std::string& generation){
  std::lock_guard guard(mutex_);Open();if(cursor>sequence_||(!generation.empty()&&generation!=generation_))cursor=0;UniValue out{UniValue::VOBJ};out.pushKV("generation",generation_);out.pushKV("next_cursor",cursor);out.pushKV("more",false);
  if(cursor==std::numeric_limits<uint64_t>::max())return out;
  auto iterator=std::unique_ptr<CDBIterator>(db_->NewIterator());iterator->Seek(IdentityKey{'e',Number(cursor+1)});
  IdentityKey key;std::string source;
  if(iterator->Valid()&&iterator->GetKey(key)&&key.first=='e'&&iterator->GetValue(source)){
   const auto seq=std::stoull(key.second);out.pushKV("certificate",source);out.pushKV("next_cursor",seq);out.pushKV("more",seq<sequence_);
  }return out;
 }
};
} // namespace pgn::social::display

