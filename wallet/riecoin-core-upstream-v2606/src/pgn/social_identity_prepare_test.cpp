// Real Native archive/ledger/BIP322 component; no fake wallet or signature.
#define PGN_SOCIAL_COMMITTED_HELPERS_ONLY
#include <pgn/social_identity_committed_test.cpp>
#include <pgn/social_identity_prepare.h>
#include <fstream>
using namespace pgn::core;
int main(int argc,char**argv){try{
 need(argc==2,"New private Native preparation fixture path required");ECC_Context ecc;SelectParams(ChainType::TESTNET);LOCK(cs_main);const auto path=fs::PathFromString(argv[1]);need(!fs::exists(path),"Private fixture already exists");Fixture f(path/"native");
 const Integer lower{"150000000000000000000"};const EncodingLimits encoding{10002,1048576,32,1};const CommittedGapPolicy evidence{encoding,{{128,32,32},10000},{256,256,256},{128,64},1,1};const RangePoolDefinition pool{lower,Integer{"1000000000000000000"},16,1,10};const CAmount unit=PGN_ATOMIC_UNITS_PER_COIN;const MonetaryPolicyDefinition money{99000000*unit,1000000*unit,{200*unit,1},100,70,25,5,FeeRoutingDefinition{100,100}};
 const auto range=BuildCandidateRangePolicyEnvelope(pool,f.policy),science=BuildCandidateSciencePolicyEnvelope(ScienceVerificationDefinition{1,evidence,encoding},f.policy),monetary=BuildCandidateMonetaryPolicyEnvelope(money,f.policy);need(range&&science&&monetary,"Fixture policies");f.Append({CTxOut{0,*range},CTxOut{0,*science},CTxOut{0,*monetary}});AddLabeledReport(f,1,Attribution("SYNTHETIC Prepared Native Operator"),{5,6});
 const auto snapshot=f.Snapshot(f.Ledger());const auto k=known(1);CKey s;s.MakeNewKey(true);const auto social=HexStr(GetScriptForDestination(WitnessV0KeyHash(s.GetPubKey()))),operator_script=HexStr(script(1));const std::string zero(64,'0');
 const auto canonical=[&](const uint256& hash,int32_t height){const auto* node=height>=0&&height<=f.tip->nHeight?f.tip->GetAncestor(height):nullptr;return node&&node->GetBlockHash()==hash;};
 const auto reject=[&](const auto& fn){try{fn();return false;}catch(const std::exception&){return true;}};
 auto prepared=committed::PrepareConsent(snapshot,social,operator_script,"bind",zero,{},true,GetRandHash().GetHex(),canonical);
 check(prepared.statement["operator_id"].get_str()==NativeAuditOperator(NativeAuditLedger{},script(1)).GetHex()&&prepared.display_name==std::optional<std::string>{"SYNTHETIC Prepared Native Operator"},"Prepare derives O and label from actual Native attribution, never a caller username");
 check(prepared.social_challenge!=prepared.operator_challenge&&prepared.transition_challenge.empty(),"Only required bind challenges exposed, with separate existing domains");
 check(reject([&]{committed::PrepareConsent(snapshot,social,operator_script,"bind",zero,{},false,GetRandHash().GetHex(),canonical);}),"Incomplete target history cannot authorize a prepared consent");
 CKey unknown;unknown.MakeNewKey(true);const auto unknown_script=HexStr(GetScriptForDestination(WitnessV0KeyHash(unknown.GetPubKey())));
 check(reject([&]{committed::PrepareConsent(snapshot,social,unknown_script,"bind",zero,{},true,GetRandHash().GetHex(),canonical);}),"Unknown Native K cannot prepare another operator name");
 auto unsigned_certificate=identity::Obj();unsigned_certificate.pushKV("statement",prepared.statement);unsigned_certificate.pushKV("social_proof","");unsigned_certificate.pushKV("operator_proof","");unsigned_certificate.pushKV("authorizer_script","");unsigned_certificate.pushKV("authorization_proof","");
 auto partial=unsigned_certificate;partial.pushKV("operator_proof",EncodeBase64(sign(prepared.operator_challenge,k)));
 // UniValue pushKV appends duplicate keys: construct exact fields instead.
 partial=identity::Obj();partial.pushKV("statement",prepared.statement);partial.pushKV("social_proof","");partial.pushKV("operator_proof",EncodeBase64(sign(prepared.operator_challenge,k)));partial.pushKV("authorizer_script","");partial.pushKV("authorization_proof","");
 auto inspected=committed::ValidateSubmission(identity::Canonical(partial),snapshot,{},true,false,canonical);
 check(inspected.coverage.operator_key&&!inspected.coverage.social&&!inspected.coverage.complete,"Imported genuine K proof is verifiable without granting a link or signing S");
 check(reject([&]{committed::ValidateSubmission(identity::Canonical(partial),snapshot,{},true,true,canonical);}),"Incomplete consent cannot be submitted for inclusion");
 const auto source=Certificate(prepared.statement,s,k);const auto verified=committed::ValidateSubmission(source,snapshot,{},true,true,canonical);
 check(verified.coverage.complete&&!verified.already_included,"Two real domain-scoped BIP322 consents validate as pending, not Native inclusion");
 auto bad=identity::Obj();bad.pushKV("statement",prepared.statement);bad.pushKV("social_proof",EncodeBase64(sign(prepared.social_challenge,s)));bad.pushKV("operator_proof",EncodeBase64(sign(prepared.social_challenge,s)));bad.pushKV("authorizer_script","");bad.pushKV("authorization_proof","");
 check(reject([&]{committed::ValidateSubmission(identity::Canonical(bad),snapshot,{},true,true,canonical);}),"One Social consent cannot substitute for Native consent");
 auto same=committed::PrepareConsent(snapshot,operator_script,operator_script,"bind",zero,{},true,GetRandHash().GetHex(),canonical);
 check(committed::ValidateSubmission(Certificate(same.statement,k,k),snapshot,{},true,true,canonical).coverage.complete,"Legitimate S equals K is supported with both purpose-specific consents");
 f.Append({CTxOut{0,committed::Envelope(source)}});const auto native=f.Snapshot(f.Ledger());committed::Projection projection;for(int32_t height=0;height<=f.tip->nHeight;++height){const auto block=f.archive.Load(*f.view,*f.tip,height,f.schedule.archive);need(bool(block.block),"Contiguous Native included consent archive");projection.ApplyBlock(native,*block.block,canonical);}const auto* state=projection.Find(social);need(state,"Included current state");std::optional<committed::State> current{*state};
 check(committed::ValidateSubmission(source,native,current,true,true,canonical).already_included,"Exact valid retry recognizes its canonical Native inclusion idempotently");
 check(reject([&]{committed::PrepareConsent(native,social,operator_script,"bind",state->head,current,true,GetRandHash().GetHex(),canonical);}),"An active link cannot be silently replaced");
 check(reject([&]{committed::PrepareConsent(native,social,operator_script,"withdraw",zero,current,true,GetRandHash().GetHex(),canonical);}),"Withdrawal preparation requires exact current head");
 const auto withdrawing=committed::PrepareConsent(native,social,operator_script,"withdraw",state->head,current,true,GetRandHash().GetHex(),canonical);const auto withdrawal=Certificate(withdrawing.statement,s,k,&k);
 check(withdrawing.social_challenge.empty()&&withdrawing.operator_challenge.empty()&&!withdrawing.transition_challenge.empty()&&committed::ValidateSubmission(withdrawal,native,current,true,true,canonical).coverage.transition,"Owned K can explicitly withdraw current lineage without obtaining S again");
 bool context=false;try{committed::ValidateSubmission(withdrawal,native,current,false,true,canonical);}catch(const committed::AssociationContextUnavailable&){context=true;}
 check(context&&current->active&&current->head==state->head,"Missing history cannot consume a withdrawal or mutate its Native head");
 bool memory=false;try{committed::ValidateSubmission(withdrawal,native,current,true,true,[](const auto&,int32_t)->bool{throw std::bad_alloc{};});}catch(const std::bad_alloc&){memory=true;}
 check(memory&&committed::ValidateSubmission(withdrawal,native,current,true,true,canonical).coverage.complete,"Resource failure preserves exact withdrawal for retry");
 auto wrong=prepared.statement;auto evil=identity::Obj();for(const auto& key:wrong.getKeys())evil.pushKV(key,key=="operator_id"?UniValue(std::string(64,'e')):wrong[key]);
 check(reject([&]{committed::ValidateSubmission(Certificate(evil,s,k),native,{},true,true,canonical);}),"Valid signatures cannot borrow a different Native operator");
 auto extra=unsigned_certificate;extra.pushKV("display_name","Administrator");check(reject([&]{committed::DecodeConsentCertificate(identity::Canonical(extra),native.genesis.GetHex());}),"No username, role or extra field enters the existing certificate");
 auto golden=identity::Obj();golden.pushKV("certificate",source);golden.pushKV("event_id",prepared.event_id);golden.pushKV("social_challenge",prepared.social_challenge);golden.pushKV("operator_challenge",prepared.operator_challenge);golden.pushKV("genesis",snapshot.genesis.GetHex());std::ofstream output((path/"consent-golden.json").std_path(),std::ios::binary);output<<golden.write();output.close();need(!output.fail(),"Cross-language golden write failed");
 std::cout<<"CHECKS 17\nLIMIT Native component prepare/validation, no queue, RPC, public signing or activation.\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
