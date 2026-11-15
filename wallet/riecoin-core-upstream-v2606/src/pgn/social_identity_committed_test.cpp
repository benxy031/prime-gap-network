// Native archive/attribution fixture, distinct from real2Core anchoring witness.
#define main ExistingNativeLeaderboardReferenceMain
#include <pgn/native_leaderboard_test.cpp>
#undef main
#include <pgn/social_identity_committed.h>
#include <chainparams.h>
#include <util/time.h>
#include <random.h>
namespace identity=pgn::social::identity;
namespace committed=pgn::social::identity::committed;
static UniValue LegacyLease(const CKey& social,const CKey& operator_key,const identity::Contract& contract,const uint256& operator_id,const CBlockIndex& anchor,int64_t created){
    auto c=identity::Obj();c.pushKV("v",1);c.pushKV("policy_id",contract.GetPolicy().Id());c.pushKV("genesis",contract.Genesis());c.pushKV("origin","pgn-social://"+contract.Genesis());c.pushKV("social_script",HexStr(GetScriptForDestination(WitnessV0KeyHash(social.GetPubKey()))));c.pushKV("operator_script",HexStr(GetScriptForDestination(WitnessV0KeyHash(operator_key.GetPubKey()))));c.pushKV("operator_id",operator_id.GetHex());c.pushKV("anchor_hash",anchor.GetBlockHash().GetHex());c.pushKV("anchor_height",anchor.nHeight);c.pushKV("created_ms",created);c.pushKV("not_after_ms",created+identity::WorkingLifetimeMs);c.pushKV("nonce",GetRandHash().GetHex());c.pushKV("scope",identity::Scope);
    auto b=identity::Obj();b.pushKV("kind","binding");b.pushKV("binding_id",identity::BindingId(c));b.pushKV("statement",c);b.pushKV("social_proof",EncodeBase64(sign(identity::BindingMessage(c,true),social)));b.pushKV("operator_proof",EncodeBase64(sign(identity::BindingMessage(c,false),operator_key)));return b;
}
static UniValue Statement(const Fixture& f,const CBlockIndex& anchor,const CKey& s,const CKey& k,const uint256& o,const std::string& action,const std::string& previous){
    auto v=identity::Obj();v.pushKV("v",2);v.pushKV("genesis",f.chain.GetHex());v.pushKV("origin","pgn-social://"+f.chain.GetHex());v.pushKV("scope",identity::Scope);v.pushKV("action",action);v.pushKV("social_script",HexStr(GetScriptForDestination(WitnessV0KeyHash(s.GetPubKey()))));v.pushKV("operator_script",HexStr(GetScriptForDestination(WitnessV0KeyHash(k.GetPubKey()))));v.pushKV("operator_id",o.GetHex());v.pushKV("anchor_hash",anchor.GetBlockHash().GetHex());v.pushKV("anchor_height",anchor.nHeight);v.pushKV("previous_link",previous);v.pushKV("nonce",GetRandHash().GetHex());return v;
}
static std::string Certificate(const UniValue& statement,const CKey& s,const CKey& k,const CKey* withdrawal=nullptr){
    auto v=identity::Obj();v.pushKV("statement",statement);v.pushKV("social_proof",withdrawal?std::string{}:EncodeBase64(sign(committed::Message(statement,"SOCIAL-CONSENT"),s)));v.pushKV("operator_proof",withdrawal?std::string{}:EncodeBase64(sign(committed::Message(statement,"OPERATOR-CONSENT"),k)));v.pushKV("authorizer_script",withdrawal?HexStr(GetScriptForDestination(WitnessV0KeyHash(withdrawal->GetPubKey()))):std::string{});v.pushKV("authorization_proof",withdrawal?EncodeBase64(sign(committed::Message(statement,"STATE-TRANSITION"),*withdrawal)):std::string{});return identity::Canonical(v);
}
#ifndef PGN_SOCIAL_COMMITTED_HELPERS_ONLY
int main(int argc,char** argv){try{
    need(argc==2,"New private Native fixture path required");ECC_Context ecc;SelectParams(ChainType::TESTNET);LOCK(cs_main);const auto path=fs::PathFromString(argv[1]);need(!fs::exists(path),"Private fixture path exists");Fixture f(path/"native");
    const Integer lower{"150000000000000000000"};const EncodingLimits encoding{10002,1048576,32,1};const CommittedGapPolicy evidence{encoding,{{128,32,32},10000},{256,256,256},{128,64},1,1};const RangePoolDefinition pool{lower,Integer{"1000000000000000000"},16,1,10};const CAmount unit=PGN_ATOMIC_UNITS_PER_COIN;const MonetaryPolicyDefinition money{99000000*unit,1000000*unit,{200*unit,1},100,70,25,5,FeeRoutingDefinition{100,100}};
    const auto range=BuildCandidateRangePolicyEnvelope(pool,f.policy),science=BuildCandidateSciencePolicyEnvelope(ScienceVerificationDefinition{1,evidence,encoding},f.policy),monetary=BuildCandidateMonetaryPolicyEnvelope(money,f.policy);need(range&&science&&monetary,"Native fixture policies");f.Append({CTxOut{0,*range},CTxOut{0,*science},CTxOut{0,*monetary}});AddLabeledReport(f,1,Attribution("SYNTHETIC Native Operator"),{5,6});
    const auto o=NativeAuditOperator(NativeAuditLedger{},script(1));const auto k=known(1);CKey s;s.MakeNewKey(true);const auto social=HexStr(GetScriptForDestination(WitnessV0KeyHash(s.GetPubKey())));auto* before=f.tip;
    const auto canonical=[&](const uint256& hash,int32_t height){const auto* node=height>=0&&height<=f.tip->nHeight?f.tip->GetAncestor(height):nullptr;return node&&node->GetBlockHash()==hash;};
    committed::Projection projection;int32_t replayed=-1;
    const auto replay=[&](){committed::Projection::BlockResult total;const auto snapshot=f.Snapshot(f.Ledger());for(int32_t height=replayed+1;height<=f.tip->nHeight;++height){const auto read=f.archive.Load(*f.view,*f.tip,height,f.schedule.archive);need(bool(read.block),"Actual authenticated Native coinbase proof required");const auto result=projection.ApplyBlock(snapshot,*read.block,canonical);total.applied+=result.applied;total.stale+=result.stale;total.invalid+=result.invalid;replayed=height;}return total;};
    const auto binding=Statement(f,*before,s,k,o,"bind",std::string(64,'0'));const auto source=Certificate(binding,s,k);f.Append({CTxOut{0,committed::Envelope(source)}});const auto applied=replay();
    check(applied.applied==1&&applied.invalid==0,"V2 two-key consent is applied only from full verified Native coinbase history");
    const auto* active=projection.Find(social);need(active,"Committed Social row");check(committed::CurrentName(*active,f.Snapshot(f.Ledger()),canonical)==std::optional<std::string>{"SYNTHETIC Native Operator"},"Persistent Social name comes from exact Native operator, not certificate text");
    check(source.find("not_after")==std::string::npos&&source.find("expires")==std::string::npos,"Persistent V2 does not silently inherit or renew a24-hour lease");
    CKey unknown;unknown.MakeNewKey(true);CKey other;other.MakeNewKey(true);const auto borrowed=Certificate(Statement(f,*f.tip,other,unknown,o,"bind",std::string(64,'0')),other,unknown);f.Append({CTxOut{0,committed::Envelope(borrowed)}});const auto rejected=replay();
    check(rejected.invalid==1&&committed::CurrentName(*projection.Find(social),f.Snapshot(f.Ledger()),canonical).has_value(),"Valid signatures by unknown K cannot borrow O or suspend another name");
    identity::Contract lease{identity::Policy{identity::WorkingLifetimeMs},f.chain.GetHex()};const auto v1=identity::Canonical(LegacyLease(s,k,lease,o,*f.tip,GetTime<std::chrono::milliseconds>().count()));check(lease.Verify(v1).social_script==social,"Existing V1 lease proof remains independently verifiable");f.Append({CTxOut{0,committed::Envelope(v1)}});check(replay().invalid==1,"V1 lease bytes under aV2 tag are rejected instead of upgraded");
    const auto old_head=projection.Find(social)->head;const auto withdraw_k=Statement(f,*f.tip,s,k,o,"withdraw",old_head),withdraw_s=Statement(f,*f.tip,s,k,o,"withdraw",old_head);
    f.Append({CTxOut{0,committed::Envelope(Certificate(withdraw_k,s,k,&k))},CTxOut{0,committed::Envelope(Certificate(withdraw_s,s,k,&s))}});const auto withdrawals=replay();
    check(withdrawals.applied==1&&withdrawals.stale==1&&projection.Find(social)->head==committed::EventId(withdraw_k),"Concurrent valid children choose first Native output position, not arrival/hash/time");
    check(!projection.Find(social)->active&&!committed::CurrentName(*projection.Find(social),f.Snapshot(f.Ledger()),canonical),"K alone can withdraw a lost-S association without acquiring a role");
    f.Append({CTxOut{0,committed::Envelope(source)}});check(replay().stale==1&&!projection.Find(social)->active,"Reinjected old signed bind cannot resurrect a withdrawn name");
    const auto renewed=Statement(f,*f.tip,s,k,o,"bind",projection.Find(social)->head);f.Append({CTxOut{0,committed::Envelope(Certificate(renewed,s,k))}});check(replay().applied==1&&projection.Find(social)->active,"New explicit dual consent to the withdrawal head can rebind the stable S");
    const auto current=*projection.Find(social);committed::Projection restarted;const auto snapshot=f.Snapshot(f.Ledger());for(int32_t height=0;height<=f.tip->nHeight;++height){const auto block=f.archive.Load(*f.view,*f.tip,height,f.schedule.archive);restarted.ApplyBlock(snapshot,*block.block,canonical);}check(restarted.Find(social)->head==current.head&&restarted.Find(social)->active,"Restart reconstruction from authenticated archive yields the exact lineage head");
    f.Append({CTxOut{0,CScript{}<<OP_RETURN}},before);check(!committed::CurrentName(current,f.Snapshot(f.Ledger()),canonical),"A reorganized commit never retains an active display name");
    committed::Projection fork;const auto fork_snapshot=f.Snapshot(f.Ledger());for(int32_t height=0;height<=f.tip->nHeight;++height){const auto block=f.archive.Load(*f.view,*f.tip,height,f.schedule.archive);fork.ApplyBlock(fork_snapshot,*block.block,canonical);}check(!fork.Find(social),"Canonical branch replay excludes orphaned binding and retirement history");
    // Real Native enrollment transition on a non-authority key. The initial
    // owner1 establishes Native authority; owner5 remains initially unenrolled.
    Fixture changed(path/"membership-transition");changed.Append({CTxOut{0,*range},CTxOut{0,*science},CTxOut{0,*monetary}});
    AddLabeledReport(changed,1,Attribution("SYNTHETIC authority fixture"),{5,6});
    AddLabeledReport(changed,5,Attribution("SYNTHETIC Native Operator Five"),{3,6});
    const auto five=NativeAuditOperator(NativeAuditLedger{},script(5));const auto fifth_key=known(5);CKey social_key;social_key.MakeNewKey(true);
    const auto fifth_social=HexStr(GetScriptForDestination(WitnessV0KeyHash(social_key.GetPubKey())));
    auto chain_now=[&](const uint256& h,int32_t height){const auto* block=changed.tip->GetAncestor(height);return block&&block->GetBlockHash()==h;};
    auto enroll=[&](unsigned scalar,const uint256& target){const auto ledger=changed.Ledger();UniValue body{UniValue::VOBJ};body.pushKV("schema","PGN_TESTNET_PARTICIPANT_V1");body.pushKV("operator_id",target.GetHex());body.pushKV("signing_script",HexStr(script(scalar)));body.pushKV("may_audit",true);const auto text=body.write();NativeAuditEvent event{ledger.authority_request,{},NativeAuditAction::Enroll,0,0,ledger.authority,Bytes{text.begin(),text.end()}};changed.Append({CTxOut{0,changed.Event(event,ledger)}});};
    const auto five_statement=Statement(changed,*changed.tip,social_key,fifth_key,five,"bind",std::string(64,'0'));
    changed.Append({CTxOut{0,committed::Envelope(Certificate(five_statement,social_key,fifth_key))}});
    committed::Projection before_membership;auto original_snapshot=changed.Snapshot(changed.Ledger());
    for(int32_t height=0;height<=changed.tip->nHeight;++height){const auto block=changed.archive.Load(*changed.view,*changed.tip,height,changed.schedule.archive);before_membership.ApplyBlock(original_snapshot,*block.block,chain_now);}
    check(before_membership.Find(fifth_social)&&committed::CurrentName(*before_membership.Find(fifth_social),original_snapshot,chain_now)==std::optional<std::string>{"SYNTHETIC Native Operator Five"},"Actual Native operator5 binding resolves before enrollment transition");
    enroll(5,NativeAuditOperator(NativeAuditLedger{},script(6)));
    const auto changed_snapshot=changed.Snapshot(changed.Ledger());auto changed_block=changed.archive.Load(*changed.view,*changed.tip,changed.tip->nHeight,changed.schedule.archive);before_membership.ApplyBlock(changed_snapshot,*changed_block.block,chain_now);
    check(!committed::CurrentName(*before_membership.Find(fifth_social),changed_snapshot,chain_now),"Actual current Native reassignment suppresses old display association");
    committed::Projection after_membership;
    for(int32_t height=0;height<=changed.tip->nHeight;++height){const auto block=changed.archive.Load(*changed.view,*changed.tip,height,changed.schedule.archive);after_membership.ApplyBlock(changed_snapshot,*block.block,chain_now);}
    check(after_membership.Find(fifth_social)&&after_membership.Find(fifth_social)->head==before_membership.Find(fifth_social)->head,"Actual Native reassignment must not erase historical consent lineage on restart");
    const auto withdrawal=Statement(changed,*changed.tip,social_key,fifth_key,five,"withdraw",after_membership.Find(fifth_social)->head);
    changed.Append({CTxOut{0,committed::Envelope(Certificate(withdrawal,social_key,fifth_key,&social_key))}});auto withdrawn_snapshot=changed.Snapshot(changed.Ledger());auto withdrawal_block=changed.archive.Load(*changed.view,*changed.tip,changed.tip->nHeight,changed.schedule.archive);after_membership.ApplyBlock(withdrawn_snapshot,*withdrawal_block.block,chain_now);
    check(!after_membership.Find(fifth_social)->active,"Social key can withdraw old lineage after Native key reassignment");
    enroll(8,five);const auto eighth_key=known(8);const auto replacement=Statement(changed,*changed.tip,social_key,eighth_key,five,"bind",after_membership.Find(fifth_social)->head);
    // Advance the empty Social projection across the real enrollment block.
    auto new_snapshot=changed.Snapshot(changed.Ledger());auto enrollment_block=changed.archive.Load(*changed.view,*changed.tip,changed.tip->nHeight,changed.schedule.archive);after_membership.ApplyBlock(new_snapshot,*enrollment_block.block,chain_now);
    changed.Append({CTxOut{0,committed::Envelope(Certificate(replacement,social_key,eighth_key))}});new_snapshot=changed.Snapshot(changed.Ledger());auto replacement_block=changed.archive.Load(*changed.view,*changed.tip,changed.tip->nHeight,changed.schedule.archive);after_membership.ApplyBlock(new_snapshot,*replacement_block.block,chain_now);
    check(committed::CurrentName(*after_membership.Find(fifth_social),new_snapshot,chain_now)==std::optional<std::string>{"SYNTHETIC Native Operator Five"},"New Native-authorized key requires explicit new dual consent to restore display");
    std::cout<<"CHECKS "<<checks<<"\nLIMIT Native authenticated archive/crypto/projection component; no public RPC, producer insertion, CDB index updater, live-user consent or role grant.\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}

#endif
