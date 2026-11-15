// Copyright (c) 2026 The PGN developers. MIT.
// Existing V2 only: queued consent is NOT a name or permission. Each pair
// verifies complete proofs and uses only canonical Native inclusion as order.
#include <pgn/social_identity_rpc.h>
#include <pgn/social_identity_batch.h>
#include <pgn/social_identity_snapshot.h>
#include <pgn/social_identity_pending.h>
#include <pgn/social.h>
#include <pgn/native_node_options.h>
#include <pgn/request_history_archive.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <common/args.h>
#include <validation.h>
#include <chainparams.h>
#include <logging.h>
#include <random.h>
#include <semaphore>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <chrono>
#include <type_traits>
namespace {
using namespace pgn::social::identity;
using namespace pgn::social::identity::committed;
using namespace pgn::core;
struct NativeView {
 ChainstateManager& manager;std::shared_ptr<const NativeLeaderboardSnapshot> snapshot;
 bool Canonical(const uint256& hash,int32_t height)const{
  LOCK(cs_main);const auto* block=height>=0&&height<=manager.ActiveChain().Height()?manager.ActiveChain()[height]:nullptr;
  return snapshot->chainstate==&manager.ActiveChainstate()&&snapshot->genesis==manager.GetConsensus().hashGenesisBlock&&block&&block->GetBlockHash()==hash;
 }
 std::optional<VerifiedNativeRequestBlock> Read(int32_t height,const std::optional<uint256>& expected)const{
  LOCK(cs_main);
  if(snapshot->chainstate!=&manager.ActiveChainstate()||snapshot->genesis!=manager.GetConsensus().hashGenesisBlock)throw AssociationContextUnavailable("Native chainstate changed");
  const auto* anchor=manager.ActiveChain()[snapshot->height];const auto* block=height>=0&&height<=manager.ActiveChain().Height()?manager.ActiveChain()[height]:nullptr;
  const auto* tip=manager.ActiveChain().Tip();const auto* archive=manager.ActiveChainstate().PgnRequestHistory();
  if(!anchor||anchor->GetBlockHash()!=snapshot->tip||!block||!tip||height>snapshot->height||(expected&&*expected!=block->GetBlockHash())||!archive||!manager.m_options.pgn_candidate_science_budget)throw AssociationContextUnavailable("Canonical Native archive unavailable");
  if(height==0)return VerifyNativeRequestBlock(manager.ActiveChainstate().CoinsTip(),*tip,0,manager.GetParams().GenesisBlock()).block;
  return archive->Load(manager.ActiveChainstate().CoinsTip(),*tip,height,manager.m_options.pgn_candidate_science_budget->schedule.archive).block;
 }
 auto Check()const{return [this](const uint256& hash,int32_t height){return Canonical(hash,height);};}
};
class NativeIdentityService {
 ChainstateManager& manager_;CandidateIndex index_;TargetResolver targets_;PendingAssociations pending_;std::mutex indexing_;
 std::counting_semaphore<2> verification_{2}; // Never acquired by mining.
 std::atomic<bool> stop_{false},active_{false};std::thread worker_;std::condition_variable wake_;std::mutex wake_mutex_;uint64_t wake_epoch_{};
 struct Incoming{int64_t peer;std::string source,digest;};
 std::mutex incoming_mutex_;std::deque<Incoming> incoming_;std::map<int64_t,size_t> incoming_peers_;std::set<std::string> incoming_ids_;size_t incoming_bytes_{};
 std::mutex ready_mutex_;uint256 ready_parent_;std::vector<std::string> ready_sources_;uint64_t ready_version_{1};
 NativeView View(){if(stop_)throw AssociationContextUnavailable("Social association service stopping");return {manager_,ReadNativeSocialIdentitySnapshot(manager_)};}
 bool Sync(const NativeView& view){
  std::unique_lock lock(indexing_,std::try_to_lock);if(!lock.owns_lock())return false;const auto canonical=view.Check();
  for(unsigned n=0;n<16;++n){const auto tip=index_.Tip();if(tip.height<0||canonical(tip.hash,tip.height))break;const auto result=index_.RollbackOne();if(result.state=="uncertain"){index_.Recover();return false;}}
  const auto tip=index_.Tip();if(tip.height>=0&&!canonical(tip.hash,tip.height))return false;
  for(unsigned n=0;n<16&&index_.Tip().height<view.snapshot->height;++n){const auto block=view.Read(index_.Tip().height+1,std::nullopt);if(!block)throw AssociationContextUnavailable("Authenticated Native history missing");const auto result=index_.Append(*block,canonical);if(result.state=="uncertain"){index_.Recover();return false;}if(result.state=="reorg")return false;}
  return index_.Tip().height>=view.snapshot->height&&canonical(view.snapshot->tip,view.snapshot->height);
 }
 TargetResolution Target(const NativeView& view,const std::string& s){
  if(!Sync(view)){Wake();throw AssociationContextUnavailable("Native identity history is advancing; retry without signing");}
  return targets_.Advance(s,*view.snapshot,[&](const CandidateRef& ref){return view.Read(ref.height,ref.block);},view.Check(),4);
 }
 SubmissionConsent Check(const std::string& source,const NativeView& view,bool cached,bool complete){
  const auto decoded=DecodeConsentCertificate(source,view.snapshot->genesis.GetHex());const auto target=Target(view,decoded.social_script);
  auto context=ValidateSubmissionContext(source,*view.snapshot,target.association,target.complete,view.Check());ConsentCoverage coverage;
  if(cached){coverage.complete=true;if(context.certificate.action=="bind")coverage.social=coverage.operator_key=true;else coverage.transition=true;}
  else coverage=CheckConsentProofs(context.certificate,complete);
  if(!view.Canonical(view.snapshot->tip,view.snapshot->height))throw AssociationContextUnavailable("Native context changed after consent verification");
  return {std::move(context.certificate),coverage,context.already_included};
 }
 void Publish(const NativeView& view,std::vector<std::string> ready){
  if(!view.Canonical(view.snapshot->tip,view.snapshot->height))return;std::lock_guard lock(ready_mutex_);
  if(ready_parent_!=view.snapshot->tip||ready_sources_!=ready){ready_parent_=view.snapshot->tip;ready_sources_=std::move(ready);++ready_version_;}
 }
 void Cycle(){
  uint64_t epoch;{std::lock_guard lock(wake_mutex_);epoch=wake_epoch_;}const auto view=View();if(!Sync(view))return;std::optional<Incoming> incoming;
  {std::lock_guard lock(incoming_mutex_);if(!incoming_.empty()){incoming=std::move(incoming_.front());incoming_.pop_front();incoming_bytes_-=incoming->source.size();incoming_ids_.erase(incoming->digest);auto it=incoming_peers_.find(incoming->peer);if(!--it->second)incoming_peers_.erase(it);}}
  if(incoming){try{Submit(incoming->source);}catch(const std::exception&){/* Sender keeps exact proof and offers again. */}}
  std::vector<std::string> ready;size_t bytes=0;std::vector<PendingConsent> page;
  try{page=pending_.FairPage(4);}catch(const std::exception&){pending_.Recover();Publish(view,{});return;}
  for(const auto& item:page)try{
   if(!verification_.try_acquire())break;struct Release{std::counting_semaphore<2>& gate;~Release(){gate.release();}}release{verification_};
   const auto outcome=pending_.Submit(item.source,[&](bool cached){return Check(item.source,view,cached,true);});
   const auto target=Target(view,item.social_script);const auto observed=pending_.Observe(item.id,*view.snapshot,target.association,target.complete,view.Check());
   if(observed.state=="included"||outcome.state!="pending"||!target.complete)continue;
   const auto script=Envelope(item.source);if(bytes+script.size()+64>8192)continue;bytes+=script.size()+64;ready.push_back(item.source);
  }catch(const std::exception&){/* No missing context or failed verification consumes a withdrawal. */}
  Publish(view,std::move(ready));bool empty;{std::lock_guard lock(incoming_mutex_);empty=incoming_.empty();}
  if(empty&&pending_.Count()==0){std::lock_guard lock(wake_mutex_);if(epoch==wake_epoch_)active_=false;}
 }
 void Loop(){
  std::unique_lock lock(wake_mutex_);while(!stop_){wake_.wait(lock,[&]{return stop_.load()||active_.load();});if(stop_)break;lock.unlock();
   try{Cycle();}catch(const std::exception&){/* Association unavailable only; mining state untouched. */}
   lock.lock();wake_.wait_for(lock,std::chrono::milliseconds(100),[&]{return stop_.load();});
  }
 }
public:
 NativeIdentityService(ChainstateManager& manager,const fs::path& path,const uint256& genesis):manager_(manager),index_(path/"native-identity-v2",genesis),targets_(index_,128),pending_(path/"pending-identity-v2",genesis.GetHex(),PendingLimits{1024*1024,4}){active_=pending_.Count()!=0;worker_=std::thread([this]{Loop();});}
 void Stop(){{std::lock_guard lock(wake_mutex_);stop_=true;}wake_.notify_all();if(worker_.joinable())worker_.join();}
 ~NativeIdentityService(){Stop();}
 void Wake(){{std::lock_guard lock(wake_mutex_);++wake_epoch_;active_=true;}wake_.notify_all();}
 UniValue Read(const UniValue& scripts){
  Wake();const auto view=View();const bool synced=Sync(view);const auto& snapshot=*view.snapshot;
  UniValue out(UniValue::VOBJ);out.pushKV("schema","PGN_SOCIAL_ASSOCIATIONS_2");out.pushKV("genesis",snapshot.genesis.GetHex());out.pushKV("bestblockhash",snapshot.tip.GetHex());out.pushKV("height",snapshot.height);out.pushKV("index_height",index_.Tip().height);out.pushKV("generation",std::to_string(index_.Generation()));out.pushKV("state",synced?"ready":"pending");out.pushKV("reason",synced?"":"Native identity index is advancing");
  out.pushKV("results",ResolveBatch(scripts,[&](const std::string& s){
   TargetResolution reply;reply.social_script=s;reply.anchor={snapshot.height,snapshot.tip};if(!synced){reply.state="pending";reply.reason="Native identity history is advancing";return reply;}
   if(!verification_.try_acquire()){reply.state="pending";reply.reason="Social proof verification busy";return reply;}struct Release{std::counting_semaphore<2>& gate;~Release(){gate.release();}}release{verification_};
   return targets_.Advance(s,snapshot,[&](const CandidateRef& ref){return view.Read(ref.height,ref.block);},view.Check(),4);
  }));
  if(!view.Canonical(snapshot.tip,snapshot.height))throw AssociationContextUnavailable("Native association anchor reorganized");return out;
 }
 UniValue Prepare(const std::string& s,const std::string& k,const std::string& action,const std::string& previous,const std::string& existing){
  Wake();Require(Hex(s,44)&&s.starts_with("0014"),"Canonical Social script required");
  if(!verification_.try_acquire())throw AssociationContextUnavailable("Social proof verification busy; packet preserved");
  struct Release{std::counting_semaphore<2>& gate;~Release(){gate.release();}}release{verification_};
  const auto view=View();const auto target=Target(view,s);const auto& native=*view.snapshot;std::string source;
  if(existing.empty()){
   const auto prepared=PrepareConsent(native,s,k,action,previous,target.association,target.complete,GetRandHash().GetHex(),view.Check());
   auto cert=Obj();cert.pushKV("statement",prepared.statement);cert.pushKV("social_proof","");cert.pushKV("operator_proof","");cert.pushKV("authorizer_script",action=="withdraw"?s:"");cert.pushKV("authorization_proof","");source=Canonical(cert);
  }else{
   source=existing;const auto decoded=DecodeConsentCertificate(source,native.genesis.GetHex());Require(decoded.social_script==s&&decoded.operator_script==k&&decoded.action==action&&decoded.previous==previous,"Imported packet differs from selected keys/action/head");
  }
  const auto checked=Check(source,view,false,false);const auto& proof=checked.certificate;
  UniValue out(UniValue::VOBJ);out.pushKV("schema","PGN_SOCIAL_CONSENT_PREPARE_2");out.pushKV("genesis",native.genesis.GetHex());out.pushKV("bestblockhash",native.tip.GetHex());out.pushKV("height",native.height);out.pushKV("event_id",proof.id);out.pushKV("certificate",source);out.pushKV("social_verified",checked.coverage.social);out.pushKV("operator_verified",checked.coverage.operator_key);out.pushKV("transition_verified",checked.coverage.transition);out.pushKV("included",checked.already_included);
  out.pushKV("social_challenge",action=="bind"?Message(proof.statement,"SOCIAL-CONSENT"):"");out.pushKV("operator_challenge",action=="bind"?Message(proof.statement,"OPERATOR-CONSENT"):"");out.pushKV("transition_challenge",action=="withdraw"?Message(proof.statement,"STATE-TRANSITION"):"");
  for(const auto& row:native.rows)if(row.operator_id.GetHex()==proof.operator_id&&row.display_name){out.pushKV("display_name",*row.display_name);break;}return out;
 }
 UniValue Submit(const std::string& source){
  Require(!source.empty()&&source.size()<=MaxFrame,"Bounded canonical V2 certificate required");Wake();const auto view=View();const auto decoded=DecodeConsentCertificate(source,view.snapshot->genesis.GetHex());
  if(!verification_.try_acquire())throw AssociationContextUnavailable("Social proof verification busy; packet preserved");
  struct Release{std::counting_semaphore<2>& gate;~Release(){gate.release();}}release{verification_};
  const auto reply=pending_.Submit(source,[&](bool cached){return Check(source,view,cached,true);});
  UniValue out(UniValue::VOBJ);out.pushKV("schema","PGN_SOCIAL_CONSENT_SUBMIT_2");out.pushKV("genesis",view.snapshot->genesis.GetHex());out.pushKV("event_id",decoded.id);out.pushKV("state",reply.state);out.pushKV("reason",reply.reason);out.pushKV("permissions_granted",false);return out;
 }
 void Offer(int64_t peer,const std::string& source){
  if(source.empty()||source.size()>MaxFrame)return;const auto digest=Hash(source);std::lock_guard lock(incoming_mutex_);const auto found=incoming_peers_.find(peer);
  if(incoming_ids_.contains(digest)||incoming_.size()>=32||incoming_bytes_+source.size()>256*1024||(found!=incoming_peers_.end()&&found->second>=2))return;
  incoming_.push_back({peer,source,digest});incoming_ids_.insert(digest);++incoming_peers_[peer];incoming_bytes_+=source.size();Wake();
 }
 std::optional<std::string> NextOffer(uint64_t& cursor){
  std::unique_lock lock(ready_mutex_,std::try_to_lock);if(!lock.owns_lock()||ready_sources_.empty())return {};
  size_t offset=(cursor>>16)==ready_version_?size_t(cursor&65535):0;if(offset>=ready_sources_.size())return {};
  auto source=ready_sources_[offset];cursor=(ready_version_<<16)|(offset+1);return source;
 }
 bool Append(CMutableTransaction& cb,const uint256& parent,size_t available){
  static_assert(std::is_nothrow_move_constructible_v<CTxOut>);
  Wake();std::unique_lock lock(ready_mutex_,std::try_to_lock);if(!lock.owns_lock()||ready_parent_!=parent||ready_sources_.empty())return false;
  try{std::vector<CTxOut> additions;for(const auto& source:ready_sources_){const auto script=Envelope(source);const size_t charge=script.size()+64;
   if(charge>available||cb.vout.size()+additions.size()>=256)break;if(std::any_of(cb.vout.begin(),cb.vout.end(),[&](const CTxOut& output){return output.nValue==0&&output.scriptPubKey==script;}))continue;
   additions.emplace_back(0,script);available-=charge;
  }
  if(additions.empty())return false;cb.vout.reserve(cb.vout.size()+additions.size());for(auto& output:additions)cb.vout.push_back(std::move(output));return true;
  }catch(const std::exception&){return false;} // No partial transaction mutation.
 }
};
std::atomic<std::shared_ptr<NativeIdentityService>> service;
std::shared_ptr<NativeIdentityService> Current(){auto result=service.load();if(!result)throw AssociationContextUnavailable("Native public associations unavailable; Chat remains available");return result;}
RPCMethod getsocialassociations(){
 return RPCMethod{"getsocialassociations","Resolve bounded Social scripts through dual consent and Native public attribution. Display only, never roles or presence.",
 {{"social_scripts",RPCArg::Type::ARR,RPCArg::Optional::NO,"One through100 canonical lower-case P2WPKH scripts",{{"script",RPCArg::Type::STR,RPCArg::Optional::NO,"Social signing script"}}}},
 RPCResult{RPCResult::Type::OBJ_DYN,"","Canonical anchor, index progress and per-target state",{{RPCResult::Type::ELISION,"","Display provenance"}}},RPCExamples{""},
 [](const RPCMethod&,const JSONRPCRequest& request)->UniValue{const auto& scripts=request.params[0];if(!scripts.isArray()||scripts.empty()||scripts.size()>100)throw JSONRPCError(RPC_INVALID_PARAMETER,"One through100 Social scripts required");try{return Current()->Read(scripts);}catch(const std::exception& e){throw JSONRPCError(RPC_MISC_ERROR,e.what());}}};
}
RPCMethod preparesocialassociation(){
 return RPCMethod{"preparesocialassociation","Prepare or validate an existing V2 display-only consent. O is derived from Native. No wallet, signature, unlock, submission or permission action.",
 {{"social_script",RPCArg::Type::STR_HEX,RPCArg::Optional::NO,"Target Social S"},{"operator_script",RPCArg::Type::STR_HEX,RPCArg::Optional::NO,"Native K whose confirmed O is derived by Core"},{"action",RPCArg::Type::STR,RPCArg::Optional::NO,"bind or withdraw"},{"previous_head",RPCArg::Type::STR_HEX,RPCArg::Optional::NO,"Exact current link ID or64 zeroes"},{"certificate",RPCArg::Type::STR,RPCArg::Optional::OMITTED,"Existing canonical V2 public packet for validation/import"}},
 RPCResult{RPCResult::Type::OBJ_DYN,"","Certificate, scoped challenges, verified partial proofs and anchor",{{RPCResult::Type::ELISION,"","No name/rights before inclusion"}}},RPCExamples{""},
 [](const RPCMethod&,const JSONRPCRequest& request)->UniValue{try{return Current()->Prepare(request.params[0].get_str(),request.params[1].get_str(),request.params[2].get_str(),request.params[3].get_str(),request.params[4].isNull()?"":request.params[4].get_str());}catch(const std::exception& e){throw JSONRPCError(RPC_MISC_ERROR,e.what());}}};
}
RPCMethod submitsocialassociation(){
 return RPCMethod{"submitsocialassociation","Queue a complete existing V2 certificate for optional inclusion in already requested Native work. Pending is not a name/right. No new block is created.",
 {{"certificate",RPCArg::Type::STR,RPCArg::Optional::NO,"Canonical V2 certificate at most24KiB"}},
 RPCResult{RPCResult::Type::OBJ_DYN,"","Exact content ID and Pending/Included/Uncertain",{{RPCResult::Type::ELISION,"","Caller retains exact signed packet"}}},RPCExamples{""},
 [](const RPCMethod&,const JSONRPCRequest& request)->UniValue{try{return Current()->Submit(request.params[0].get_str());}catch(const std::exception& e){throw JSONRPCError(RPC_MISC_ERROR,e.what());}}};
}
}
namespace pgn::social::identity {
void StartNativeAssociations(ChainstateManager& manager,const ArgsManager& args){
 if(!pgn::social::Enabled())return;
 try{service.store(std::make_shared<NativeIdentityService>(manager,args.GetDataDirNet()/"social",manager.GetConsensus().hashGenesisBlock));}
 catch(const std::exception& error){LogWarning("Social association storage unavailable: %s",error.what());}
}
void StopNativeAssociations(){auto current=service.exchange({});if(current)current->Stop();}
bool NativeAssociationsAvailable(){return bool(service.load());}
void OfferNativeAssociation(int64_t peer,const std::string& source){if(auto current=service.load())current->Offer(peer,source);}
std::optional<std::string> NextNativeAssociationOffer(uint64_t& cursor){if(auto current=service.load())return current->NextOffer(cursor);return {};}
bool AppendNativeAssociations(CMutableTransaction& cb,const uint256& parent,size_t available){if(auto current=service.load())return current->Append(cb,parent,available);return false;}
}
void RegisterPgnSocialIdentityRPCCommands(CRPCTable& table){
 static const CRPCCommand commands[]={{"social",&getsocialassociations},{"social",&preparesocialassociation},{"social",&submitsocialassociation}};
 for(const auto& command:commands)table.appendCommand(command.name,&command);
}
