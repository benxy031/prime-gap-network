#define PGN_SOCIAL_COMMITTED_HELPERS_ONLY
#include <pgn/social_identity_committed_test.cpp>
#include <pgn/social_identity_target.h>
int main(int argc,char** argv){try{
    need(argc==2,"New private target fixture required");ECC_Context ecc;SelectParams(ChainType::TESTNET);LOCK(cs_main);const auto path=fs::PathFromString(argv[1]);need(!fs::exists(path),"Target fixture already exists");Fixture f(path/"native");
    const Integer lower{"150000000000000000000"};const EncodingLimits encoding{10002,1048576,32,1};const CommittedGapPolicy evidence{encoding,{{128,32,32},10000},{256,256,256},{128,64},1,1};const RangePoolDefinition pool{lower,Integer{"1000000000000000000"},16,1,10};const CAmount unit=PGN_ATOMIC_UNITS_PER_COIN;const MonetaryPolicyDefinition money{99000000*unit,1000000*unit,{200*unit,1},100,70,25,5,FeeRoutingDefinition{100,100}};
    const auto range=BuildCandidateRangePolicyEnvelope(pool,f.policy),science=BuildCandidateSciencePolicyEnvelope(ScienceVerificationDefinition{1,evidence,encoding},f.policy),monetary=BuildCandidateMonetaryPolicyEnvelope(money,f.policy);need(range&&science&&monetary,"Target fixture policies");f.Append({CTxOut{0,*range},CTxOut{0,*science},CTxOut{0,*monetary}});
    AddLabeledReport(f,1,Attribution("SYNTHETIC Native A"),{5,6});AddLabeledReport(f,2,Attribution("SYNTHETIC Native B"),{3,4});const auto older=f.Snapshot(f.Ledger());
    CKey sa,sb;sa.MakeNewKey(true);sb.MakeNewKey(true);const auto ka=known(1),kb=known(2);const auto oa=NativeAuditOperator(NativeAuditLedger{},script(1)),ob=NativeAuditOperator(NativeAuditLedger{},script(2));const auto a=HexStr(GetScriptForDestination(WitnessV0KeyHash(sa.GetPubKey()))),b=HexStr(GetScriptForDestination(WitnessV0KeyHash(sb.GetPubKey())));
    const auto astatement=Statement(f,*f.tip,sa,ka,oa,"bind",std::string(64,'0')),bstatement=Statement(f,*f.tip,sb,kb,ob,"bind",std::string(64,'0'));const auto asource=Certificate(astatement,sa,ka);
    f.Append({CTxOut{0,committed::Envelope(Certificate(astatement,sa,kb))},CTxOut{0,committed::Envelope(asource)},CTxOut{0,committed::Envelope(Certificate(bstatement,sb,kb))}});
    auto canonical=[&](const uint256& hash,int32_t height){const auto* block=f.tip->GetAncestor(height);return block&&block->GetBlockHash()==hash;};
    committed::CandidateIndex index(path/"index",f.chain);
    auto sync=[&]{for(int32_t h=index.Tip().height+1;h<=f.tip->nHeight;++h){const auto block=f.archive.Load(*f.view,*f.tip,h,f.schedule.archive);need(block.block&&index.Append(*block.block,canonical).state=="indexed","Native target index synchronization");}};
    auto read=[&](const committed::CandidateRef& ref)->std::optional<VerifiedNativeRequestBlock>{const auto block=f.archive.Load(*f.view,*f.tip,ref.height,f.schedule.archive);if(!block.block||block.block->Block()!=ref.block)return {};return *block.block;};
    sync();committed::TargetResolver resolver(index,2);auto snapshot=f.Snapshot(f.Ledger());
    auto pending=resolver.Advance(a,snapshot,read,canonical,1);check(pending.state=="pending"&&!pending.display_name&&pending.invalid==1,"Invalid first proof leaves only A pending, no name admitted");
    auto neighbor=resolver.Advance(b,snapshot,read,canonical,1);check(neighbor.display_name==std::optional<std::string>{"SYNTHETIC Native B"},"Unrelated B resolves while A has pending proof history");
    auto ready=resolver.Advance(a,snapshot,read,canonical,1);check(ready.display_name==std::optional<std::string>{"SYNTHETIC Native A"}&&ready.association&&ready.association->head==committed::EventId(astatement),"Actual Native A resolves from dual consent and authenticated report");
    const auto old=resolver.Advance(a,older,read,canonical,1);check(old.complete&&!old.association&&!old.display_name,"Older Native anchor cannot inherit a future association");
    do{ready=resolver.Advance(a,snapshot,read,canonical,1);}while(ready.state=="pending");check(ready.display_name.has_value(),"Current anchor restores by targeted replay");
    auto* before_withdraw=f.tip;const auto withdrawal=Statement(f,*f.tip,sa,ka,oa,"withdraw",ready.association->head);f.Append({CTxOut{0,committed::Envelope(Certificate(withdrawal,sa,ka,&sa))}});sync();snapshot=f.Snapshot(f.Ledger());
    auto unavailable_reader=[&](const committed::CandidateRef&)->std::optional<VerifiedNativeRequestBlock>{return {};};
    auto missing=resolver.Advance(a,snapshot,unavailable_reader,canonical,100);check(missing.state=="unavailable"&&!missing.display_name,"Missing Native archive never advances or displays a stale name");
    bool fail_context=false;auto context_reader=[&](const committed::CandidateRef& ref){auto block=read(ref);fail_context=true;return block;};auto context_fault=[&](const uint256& hash,int32_t h){if(fail_context){fail_context=false;throw std::runtime_error("Injected Native context I/O failure");}return canonical(hash,h);};
    auto fault=resolver.Advance(a,snapshot,context_reader,context_fault,100);check(fault.state=="unavailable"&&!fault.display_name,"Context failure during signature verification preserves exact target page");
    auto memory_reader=[&](const committed::CandidateRef&)->std::optional<VerifiedNativeRequestBlock>{throw std::bad_alloc();};
    auto memory=resolver.Advance(a,snapshot,memory_reader,canonical,100);check(memory.state=="unavailable"&&!memory.display_name,"Allocation failure cannot consume a pending withdrawal");
    const auto withdrawn=resolver.Advance(a,snapshot,read,canonical,100);check(withdrawn.complete&&withdrawn.state=="unlinked"&&withdrawn.association&&!withdrawn.association->active,"Retry applies the same valid withdrawal after temporary failures");
    f.Append({CTxOut{0,committed::Envelope(asource)}});sync();snapshot=f.Snapshot(f.Ledger());const auto replay=resolver.Advance(a,snapshot,read,canonical,100);check(replay.complete&&!replay.display_name&&replay.association&&!replay.association->active,"Replayed old signed binding cannot resurrect the name");
    f.Append({CTxOut{0,CScript{}<<OP_RETURN}},before_withdraw);while(index.Tip().height>=0&&!canonical(index.Tip().hash,index.Tip().height))need(index.RollbackOne().state=="rolled_back","Target branch rollback");sync();snapshot=f.Snapshot(f.Ledger());
    do{ready=resolver.Advance(a,snapshot,read,canonical,1);}while(ready.state=="pending");check(ready.display_name==std::optional<std::string>{"SYNTHETIC Native A"},"Native reorg resets cached target generation and excludes orphaned withdrawal");
    committed::TargetResolver restarted(index,1);do{ready=restarted.Advance(a,snapshot,read,canonical,1);}while(ready.state=="pending");check(ready.display_name.has_value()&&restarted.HotEntries()==1,"Requested identity reconstructs after resolver restart with bounded cache");
    std::cout<<"CHECKS "<<checks<<"\nLIMIT Actual Native archive/ledger/BIP322 component, no CoreRPC/Qt/P2P or role activation; persisted verified checkpoints and GC pending.\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
