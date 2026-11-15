// Private CDB queue: actual Native ledger, BIP322 and injected LevelDB WAL I/O.
#define PGN_SOCIAL_COMMITTED_HELPERS_ONLY
#include <pgn/social_identity_committed_test.cpp>
#include <pgn/social_identity_pending.h>
#include <leveldb/env.h>
#include <leveldb/status.h>
#include <atomic>
using namespace pgn::core;
class FailAfterSync final:public leveldb::EnvWrapper {
public:
 std::atomic<bool> armed{false};
 class File final:public leveldb::WritableFile {
  std::unique_ptr<leveldb::WritableFile> target_;bool wal_;FailAfterSync& owner_;
 public:
  File(leveldb::WritableFile* target,bool wal,FailAfterSync& owner):target_(target),wal_(wal),owner_(owner){}
  leveldb::Status Append(const leveldb::Slice& data)override{return target_->Append(data);}
  leveldb::Status Close()override{return target_->Close();}
  leveldb::Status Flush()override{return target_->Flush();}
  std::string GetName()const override{return target_->GetName();}
  leveldb::Status Sync()override{const auto status=target_->Sync();if(status.ok()&&wal_&&owner_.armed.exchange(false))return leveldb::Status::IOError("PRIVATE injected error after durable WAL sync");return status;}
 };
 explicit FailAfterSync():leveldb::EnvWrapper(leveldb::Env::Default()){}
 leveldb::Status NewWritableFile(const std::string& name,leveldb::WritableFile** file)override{leveldb::WritableFile* actual{};auto result=target()->NewWritableFile(name,&actual);if(result.ok())*file=new File(actual,name.ends_with(".log"),*this);return result;}
 leveldb::Status NewAppendableFile(const std::string& name,leveldb::WritableFile** file)override{leveldb::WritableFile* actual{};auto result=target()->NewAppendableFile(name,&actual);if(result.ok())*file=new File(actual,name.ends_with(".log"),*this);return result;}
};
int main(int argc,char**argv){try{
 need(argc==2,"New private queue fixture path required");ECC_Context ecc;SelectParams(ChainType::TESTNET);LOCK(cs_main);const auto path=fs::PathFromString(argv[1]);need(!fs::exists(path),"Fixture already exists");Fixture f(path/"native");
 const Integer lower{"150000000000000000000"};const EncodingLimits encoding{10002,1048576,32,1};const CommittedGapPolicy evidence{encoding,{{128,32,32},10000},{256,256,256},{128,64},1,1};const RangePoolDefinition pool{lower,Integer{"1000000000000000000"},16,1,10};const CAmount unit=PGN_ATOMIC_UNITS_PER_COIN;const MonetaryPolicyDefinition money{99000000*unit,1000000*unit,{200*unit,1},100,70,25,5,FeeRoutingDefinition{100,100}};
 const auto range=BuildCandidateRangePolicyEnvelope(pool,f.policy),science=BuildCandidateSciencePolicyEnvelope(ScienceVerificationDefinition{1,evidence,encoding},f.policy),monetary=BuildCandidateMonetaryPolicyEnvelope(money,f.policy);need(range&&science&&monetary,"Fixture policies");f.Append({CTxOut{0,*range},CTxOut{0,*science},CTxOut{0,*monetary}});AddLabeledReport(f,1,Attribution("SYNTHETIC Queue A"),{5,6});AddLabeledReport(f,2,Attribution("SYNTHETIC Queue B"),{7,8});
 auto native=f.Snapshot(f.Ledger());const auto canonical=[&](const uint256& hash,int32_t height){const auto* at=height>=0&&height<=f.tip->nHeight?f.tip->GetAncestor(height):nullptr;return at&&at->GetBlockHash()==hash;};
 size_t checks=0;const auto expect=[&](bool value,const char* text){need(value,text);++checks;std::cout<<"PASS "<<text<<'\n';};
 std::vector<CKey> socials(6);for(auto& s:socials)s.MakeNewKey(true);const auto ss=[&](size_t i){return HexStr(GetScriptForDestination(WitnessV0KeyHash(socials.at(i).GetPubKey())));};
 const auto binding=[&](size_t i,int key){auto prepared=committed::PrepareConsent(native,ss(i),HexStr(script(key)),"bind",std::string(64,'0'),{},true,GetRandHash().GetHex(),canonical);return Certificate(prepared.statement,socials.at(i),known(key));};
 const auto a1=binding(0,1),a2=binding(1,1),a3=binding(2,1),b1=binding(3,2),b2=binding(4,2);const auto id=[&](const std::string& source){return committed::DecodeConsentCertificate(source,native.genesis.GetHex()).id;};
 committed::Projection projection;int32_t replayed=-1;const auto replay=[&]{native=f.Snapshot(f.Ledger());for(int32_t h=replayed+1;h<=f.tip->nHeight;++h){auto block=f.archive.Load(*f.view,*f.tip,h,f.schedule.archive);need(bool(block.block),"Native archive proof");projection.ApplyBlock(native,*block.block,canonical);replayed=h;}};
 size_t crypto_calls=0,context_calls=0;
 const auto validator=[&](const std::string& source,bool cached){++context_calls;auto decoded=committed::DecodeConsentCertificate(source,native.genesis.GetHex());std::optional<committed::State> state;if(const auto* found=projection.Find(decoded.social_script))state=*found;auto context=committed::ValidateSubmissionContext(source,native,state,true,canonical);committed::ConsentCoverage coverage;
  if(cached){coverage.complete=true;if(context.certificate.action=="bind")coverage.social=coverage.operator_key=true;else coverage.transition=true;}
  else{++crypto_calls;coverage=committed::CheckConsentProofs(context.certificate,true);}return committed::SubmissionConsent{std::move(context.certificate),coverage,context.already_included};};
 FailAfterSync environment;const committed::PendingLimits limits{65536,2};auto queue=std::make_unique<committed::PendingAssociations>(path/"pending",native.genesis.GetHex(),limits,&environment);
 const auto put=[&](const std::string& source){return queue->Submit(source,[&](bool cached){return validator(source,cached);});};
 expect(put(a1).state=="pending"&&queue->Count()==1,"A real dual-consent operation is durable Pending, never a name or right");
 const auto before_crypto=crypto_calls,before_context=context_calls;const auto duplicate=put(a1);expect(duplicate.state=="pending"&&duplicate.signature_cached&&crypto_calls==before_crypto&&context_calls==before_context+1&&queue->Count()==1,"Exact replay skips costly crypto but rechecks Native context without duplication");
 auto conflict=binding(0,1);expect(put(conflict).state=="conflict"&&queue->Count()==1,"Different operation for the same S cannot silently replace pending consent");
 expect(put(a2).state=="pending"&&put(a3).state=="busy"&&queue->Count()==2,"One operator has only its explicit transient turn, not a global name hold");
 expect(put(b1).state=="pending"&&queue->Count()==3,"Another recognized operator remains admissible during A pressure");
 const auto fair=queue->FairPage(2);expect(fair.size()==2&&fair[0].operator_id!=fair[1].operator_id,"Fair page interleaves operators instead of exhausting one backlog");
 const auto next_turn=queue->FairPage(2);bool a_rotated=false;for(const auto& prior:fair)for(const auto& next:next_turn)if(prior.operator_id==next.operator_id&&prior.operator_id==committed::DecodeConsentCertificate(a1,native.genesis.GetHex()).operator_id)a_rotated=prior.id!=next.id;
 expect(a_rotated,"A stale first operation cannot starve the same operator's next candidate");
 const auto bytes=queue->Bytes();queue.reset();queue=std::make_unique<committed::PendingAssociations>(path/"pending",native.genesis.GetHex(),limits,&environment);const auto cold=crypto_calls;
 expect(put(a1).state=="pending"&&crypto_calls==cold+1&&queue->Count()==3&&queue->Bytes()==bytes,"Restart retains raw operations and never trusts a persisted crypto boolean");
 bool failed=false;try{queue->Submit(a1,[&](bool)->committed::SubmissionConsent{throw committed::AssociationContextUnavailable("Private absent context");});}catch(const committed::AssociationContextUnavailable&){failed=true;}
 expect(failed&&queue->Count()==3&&queue->Bytes()==bytes,"Context failure leaves every pending operation untouched");
 f.Append({CTxOut{0,committed::Envelope(a1)},CTxOut{0,committed::Envelope(a2)}});replay();std::optional<committed::State> active{*projection.Find(ss(0))};
 expect(queue->Observe(id(a1),native,active,false,canonical).state=="uncertain"&&queue->Count()==3,"Incomplete target result never consumes a signed operation");
 auto other_network=native;other_network.genesis=GetRandHash();expect(queue->Observe(id(a1),other_network,active,true,canonical).state=="uncertain"&&queue->Count()==3,"Another genesis cannot retire a pending operation");
 expect(queue->Observe(id(a1),native,active,true,canonical).state=="included"&&queue->Count()==2,"Only exact canonical inclusion retires that pending event");
 std::optional<committed::State> active_second{*projection.Find(ss(1))};expect(queue->Observe(id(a2),native,active_second,true,canonical).state=="included"&&queue->Count()==1,"Two included certificates retire individually, not as one block count");
 expect(put(a3).state=="pending"&&queue->Count()==2,"Inclusion releases A's transient capacity for fresh operations");
 const auto prepare=committed::PrepareConsent(native,ss(0),HexStr(script(1)),"withdraw",active->head,active,true,GetRandHash().GetHex(),canonical);const auto withdrawal=Certificate(prepare.statement,socials[0],known(1),&socials[0]);expect(put(withdrawal).state=="pending","Actual participant-signed withdrawal uses the same durable queue");
 const auto withdrawal_bytes=queue->Bytes();expect(queue->Observe(id(withdrawal),native,{},false,canonical).state=="uncertain"&&queue->Bytes()==withdrawal_bytes,"Unknown context cannot drop a pending withdrawal");
 environment.armed=true;const auto ambiguous=put(b2);expect(ambiguous.state=="uncertain"&&!environment.armed,"Injected error after real WAL sync is Uncertain, never Rejected");
 expect(queue->Recover().state=="reopened","Only the Social queue reopens after transient WAL uncertainty");const auto count=queue->Count();expect(put(b2).state=="pending"&&queue->Count()==count&&count==4,"Ambiguous durable operation reconciles exactly once after reopen");
 const auto after=queue->Count();expect(put(b2).state=="pending"&&queue->Count()==after,"Retry after recovery cannot create a second operation");
 f.Append({CTxOut{0,committed::Envelope(withdrawal)}});replay();std::optional<committed::State> withdrawn{*projection.Find(ss(0))};expect(!withdrawn->active&&queue->Observe(id(withdrawal),native,withdrawn,true,canonical).state=="included","Canonical withdrawal remains observable and retires only its own pending proof");
 std::cout<<"CHECKS "<<checks<<"\nLIMIT Explicit private queue policy64KiB/2 per operator; no production defaults, RPC, P2P or template hook yet.\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
