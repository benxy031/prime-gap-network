// Synthetic authenticated history and six REAL exact gap certificates.
// This exercises the group verifier, not native block acceptance, full-range
// completeness, fresh work/fork choice, currency issuance or network activation.
#include "native_ticket_gold_group.h"
#include "ticket_gold_witnesses_test.h"
#include <pgn/native_report_commitment.h>
#include <pgn/chain_monetary_policy.h>
#include <common/signmessage.h>
#include <consensus/merkle.h>
#include <crypto/sha256.h>
#include <key.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/solver.h>
#include <streams.h>
#include <txdb.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <array>
#include <algorithm>
#include <memory>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
extern const TranslateFn G_TRANSLATION_FUN=[](const char* text){return std::string(text);};
using namespace pgn::core;
static void need(bool value, const char* text) { if (!value) throw std::runtime_error(text); }
static CKey known(unsigned scalar) {
    std::array<unsigned char,32> bytes{}; bytes.back()=scalar;
    CKey key; key.Set(bytes.begin(),bytes.end(),true); return key;
}
static Bytes sign(const std::string& message, const CKey& key) {
    auto status=MessageVerificationResult::ERR_INVALID;
    const auto pair=BIP322Txs::Create(WitnessV0KeyHash(key.GetPubKey()),message,status);
    need(pair.has_value(),"Native BIP322 challenge could not be prepared");
    CMutableTransaction tx{pair->m_to_sign}; FillableSigningProvider provider; provider.AddKey(key);
    SignatureData data;
    need(ProduceSignature(provider,MutableTransactionSignatureCreator(tx,0,pair->m_to_spend.vout[0].nValue,SignOptions{SIGHASH_ALL}),
                          pair->m_to_spend.vout[0].scriptPubKey,data),"Actual fixture key did not sign");
    UpdateInput(tx.vin[0],data);
    DataStream wire; wire<<tx.vin[0].scriptWitness.stack;
    Bytes signature(wire.size()); std::memcpy(signature.data(),wire.data(),wire.size()); return signature;
}

int main(int argc,char** argv){try{
    need(argc==2||argc==3,"Explicit disposable group-fixture path required");
    const fs::path path=fs::PathFromString(argv[1]);fs::create_directories(path);
    const bool one=argc==3&&std::string_view(argv[2])=="--one-auditor";
    const bool failed=argc==3&&std::string_view(argv[2])=="--failed-auditor";
    LOCK(cs_main);
    ECC_Context ecc;
    const auto key=known(1);
    const auto miner=GetScriptForDestination(WitnessV0KeyHash(key.GetPubKey()));
    const auto alternate=GetScriptForDestination(WitnessV0KeyHash(known(2).GetPubKey()));
    uint256 chain; chain.begin()[0]=21;
    const Integer lower{std::string("150127204438841990509")};
    const EncodingLimits encoding{10002,1048576,32,100};
    const CommittedGapPolicy evidence_policy{encoding,{{128,32,32},10000},{256,256,256},{128,64},1,1};
    const RangePolicyReadBudget policy_budget{16,4096,16384,8192};
    const RangePoolDefinition pool{lower,Integer{"10000000000000000000"},2,1,10};
    const auto range_script=BuildCandidateRangePolicyEnvelope(pool,policy_budget);
    const auto science_script=BuildCandidateSciencePolicyEnvelope(ScienceVerificationDefinition{100,evidence_policy,encoding},policy_budget);
    const CAmount unit=PGN_ATOMIC_UNITS_PER_COIN;
    const MonetaryPolicyDefinition money_definition{99000000*unit,1000000*unit,{200*unit,1},100,70,25,5,FeeRoutingDefinition{100,100}};
    const auto money_script=BuildCandidateMonetaryPolicyEnvelope(money_definition,policy_budget);
    need(range_script&&science_script&&money_script,"Private policy envelopes rejected");
    std::array<CBlock,4> blocks; std::array<uint256,4> hashes;
    CMutableTransaction coinbase; coinbase.vin.resize(1); coinbase.vin[0].prevout.SetNull(); coinbase.vin[0].scriptSig=CScript{}<<1<<OP_0;
    coinbase.vout={CTxOut{0,*range_script},CTxOut{0,*science_script},CTxOut{0,*money_script}};
    blocks[0].nTime=100; blocks[0].vtx={MakeTransactionRef(coinbase)}; blocks[0].hashMerkleRoot=BlockMerkleRoot(blocks[0]); hashes[0]=blocks[0].GetHash();
    const TicketRequestBudget request_budget{128,128,4096};
    TicketRequest first{chain,hashes[0],0,miner,miner}; auto second=first; second.reward=alternate;
    const auto request_signature=[&](const TicketRequest& request) {
        const auto identity=TicketRequestIdentity(request,request_budget); need(identity.has_value(),"Request identity missing");
        const auto challenge=ScriptWorkChallenge({request.chain,*identity,request.anchor},request.miner,{128,4096});
        need(challenge.has_value(),"Request challenge missing"); return sign(*challenge,key);
    };
    const auto a=BuildTicketRequestEnvelope(first,request_signature(first),request_budget);
    const auto b=BuildTicketRequestEnvelope(second,request_signature(second),request_budget);
    need(a.envelope&&b.envelope,"Native signed request envelopes rejected");
    coinbase.vout={CTxOut{0,a.envelope->Script()},CTxOut{0,b.envelope->Script()}};
    blocks[1].nTime=200; blocks[1].hashPrevBlock=hashes[0]; blocks[1].vtx={MakeTransactionRef(coinbase)};
    blocks[1].hashMerkleRoot=BlockMerkleRoot(blocks[1]); hashes[1]=blocks[1].GetHash();

    const auto auditor0=GetScriptForDestination(WitnessV0KeyHash(known(3).GetPubKey()));
    const auto auditor1=GetScriptForDestination(WitnessV0KeyHash(known(4).GetPubKey()));
    const auto enroll=[&](const CScript& actor,unsigned identity){
        uint256 operator_id;operator_id.begin()[0]=identity;
        const std::string text="{\"schema\":\"PGN_TESTNET_PARTICIPANT_V1\",\"may_audit\":true,\"operator_id\":\""+operator_id.GetHex()+"\",\"signing_script\":\""+HexStr(actor)+"\"}";
        NativeAuditEvent event{a.envelope->Request().Id(),hashes[1],NativeAuditAction::Enroll,0,0,miner,Bytes{text.begin(),text.end()},{}};
        const auto work=NativeAuditWorkIdentity(chain,uint256{},event);const auto challenge=ScriptWorkChallenge({chain,work,event.parent},miner,{128,4096});need(challenge.has_value(),"Enrollment challenge missing");event.signature=sign(*challenge,key);
        const auto script=BuildNativeAuditEnvelope(event);need(script.has_value(),"Enrollment envelope missing");return CTxOut{0,*script};
    };
    const std::vector<CTxOut> enrollment{enroll(auditor0,17),enroll(auditor1,18)};
    coinbase.vout={CTxOut{0,CScript{}<<OP_RETURN}};
    for (size_t height=2;height<blocks.size();++height) {
        coinbase.vout=height==2?enrollment:std::vector<CTxOut>{CTxOut{0,CScript{}<<OP_RETURN}};
        blocks[height].nTime=300+height; blocks[height].hashPrevBlock=hashes[height-1]; blocks[height].vtx={MakeTransactionRef(coinbase)};
        blocks[height].hashMerkleRoot=BlockMerkleRoot(blocks[height]); hashes[height]=blocks[height].GetHash();
    }
    std::vector<std::unique_ptr<CBlockIndex>> indices;
    for (size_t height=0;height<blocks.size();++height) {
        auto index=std::make_unique<CBlockIndex>(blocks[height]); index->phashBlock=&hashes[height]; index->nHeight=height;
        index->nTx=1; index->nStatus=BLOCK_VALID_SCRIPTS; index->pprev=height?indices.back().get():nullptr; index->BuildSkip(); indices.push_back(std::move(index));
    }
    auto& parent=*indices.back();
    CCoinsViewDB coins{{.path=path/"coins",.cache_bytes=1<<20,.memory_only=true,.wipe_data=false},{}};
    CCoinsViewCache view{&coins}; view.SetBestBlock(hashes.back()); view.StagePgnFactCommit({{},FactHead{chain,hashes.back()},{}});
    NativeRequestHistoryArchive history{path/"history",1<<20};
    const ConfirmedRequestScanBudget scan{16,4,16384,1,request_budget};
    const RequestReplayBudget replay{32,16,{16384,4096},scan};
    for (size_t height=0;height<blocks.size();++height)
        need(history.StoreBlock(view,parent,height,blocks[height],replay.archive)==RequestArchiveVerdict::Stored,"Whole native history was not stored");
    const auto ranges=ReadChainRangePolicy(view,parent,history,replay.archive,policy_budget);
    const auto science=ReadChainSciencePolicy(view,parent,history,replay.archive,policy_budget);
    const auto money=ReadChainMonetaryPolicy(view,parent,history,replay.archive,policy_budget);
    need(ranges.policy&&science.policy&&money.policy,"Policies were not authenticated from fixture genesis");
    const RangeScheduleBudget schedule{32,100,100,replay.archive,scan,{64,4096,1024}};
    auto reserved=DeriveNativeCandidateReservation(view,parent,*ranges.policy,history,schedule,a.envelope->Request().Id());
    need(reserved.reservation.has_value(),"First complete native reservation missing");
    if (reserved.reservation->Lease().assignment.slot!=0)
        reserved=DeriveNativeCandidateReservation(view,parent,*ranges.policy,history,schedule,b.envelope->Request().Id());
    need(reserved.reservation&&reserved.reservation->Lease().assignment.slot==0,"Whole request set did not cover supplied source slot");

    const auto& lease=reserved.reservation->Lease();
    RangeContract range{{},lease.assignment.start,lease.assignment.end-lease.assignment.start,100};
    std::copy(reserved.reservation->Ticket().begin(),reserved.reservation->Ticket().end(),range.ticket_id.begin());
    std::vector<SegmentResult> report(100);for(uint32_t i=0;i<100;++i)report[i].index=i;
    std::array<uint32_t,6> segments{},records{};
    for(size_t i=0;i<6;++i){
        const Integer value{std::string(Witnesses[i].lower)};
        const auto segment=(((value-range.start+1)*100-1)/range.length).convert_to<uint32_t>();
        need(segment<100,"Existing certificate outside fixture ticket");segments[i]=segment;records[i]=report[segment].records.size();report[segment].records.push_back({value,Integer{Witnesses[i].gap},0});
    }
    const auto sha=[](std::span<const uint8_t> bytes){Digest d;CSHA256{}.Write(bytes.data(),bytes.size()).Finalize(d.data());return d;};
    const auto root=commitRange(range,report,encoding,sha).root;
    // Synthetic COUNT fixture for the native signed state machine, not a claim
    // that this enormous interval was exhaustively searched by a GPU.
    std::ostringstream text;
    text<<"{\"schema\":\"PGN_NETWORK_RANGE_REPORT_V1\",\"genesis\":\""<<chain.GetHex()<<"\",\"request_id\":\""<<lease.owner.Id().GetHex()<<"\",\"ticket\":\""<<HexStr(range.ticket_id)<<"\",\"commitment_nonce\":\""<<std::string(64,'1')<<"\",\"computed\":{\"schema\":\"PGN_HORIZON_COMPUTED_REPORT_V1\",\"ticket_id\":\""<<HexStr(range.ticket_id)<<"\",\"start\":\""<<range.start<<"\",\"end\":\""<<(range.start+range.length)<<"\",\"positions\":\""<<range.length<<"\",\"counts\":[";
    for(size_t i=0;i<100;++i){if(i)text<<',';text<<'"'<<report[i].records.size()<<'"';}
    text<<"],\"best\":[";
    for(size_t i=0;i<6;++i){if(i)text<<',';text<<"{\"lower_prime\":\""<<Witnesses[i].lower<<"\",\"upper_prime\":\""<<(Integer{std::string(Witnesses[i].lower)}+Witnesses[i].gap)<<"\",\"gap\":"<<Witnesses[i].gap<<'}';}
    text<<"]}}";const auto report_text=text.str();const Bytes report_bytes{report_text.begin(),report_text.end()};
    NativeReportCommitment hiding{lease.owner.Id(),reserved.reservation->Ticket(),*uint256::FromHex(HexStr(AuditSha(report_bytes))),hashes.back(),{}};
    const auto hide_work=NativeReportWorkIdentity(chain,hiding);const auto hide_challenge=ScriptWorkChallenge({chain,hide_work,hashes.back()},miner,{128,4096});need(hide_challenge.has_value(),"Hiding signature context missing");hiding.signature=sign(*hide_challenge,key);
    const auto hiding_script=BuildNativeReportEnvelope(hiding);need(hiding_script.has_value(),"Hiding report envelope missing");

    const NativeBlockScienceBudget body_budget{policy_budget,schedule,{128,4096},16,65536,65536};
    std::vector<std::unique_ptr<uint256>> extra_hashes;
    std::vector<std::unique_ptr<CBlockIndex>> extra_indices;
    const auto append=[&](std::vector<CTxOut> outputs){
        const auto& prior=extra_indices.empty()?parent:*extra_indices.back();
        CMutableTransaction tx;tx.vin.resize(1);tx.vin[0].prevout.SetNull();
        tx.vin[0].scriptSig=CScript{}<<(prior.nHeight+1)<<OP_0;tx.vout=std::move(outputs);
        CBlock block;block.hashPrevBlock=prior.GetBlockHash();block.nTime=prior.nTime+100;
        block.vtx={MakeTransactionRef(tx)};block.hashMerkleRoot=BlockMerkleRoot(block);
        auto hash=std::make_unique<uint256>(block.GetHash());
        auto index=std::make_unique<CBlockIndex>(block);index->phashBlock=hash.get();
        index->pprev=const_cast<CBlockIndex*>(&prior);index->nHeight=prior.nHeight+1;
        index->nTx=1;index->nStatus=BLOCK_VALID_SCRIPTS;index->BuildSkip();
        const auto before=view.GetPgnFactHead();view.SetBestBlock(*hash);
        view.StagePgnFactCommit({before,FactHead{chain,*hash},{}});
        need(history.StoreBlock(view,*index,index->nHeight,block,replay.archive)==RequestArchiveVerdict::Stored,
             "Synthetic signed block archive failed");
        extra_hashes.push_back(std::move(hash));extra_indices.push_back(std::move(index));
    };
    append({CTxOut{0,*hiding_script}});
    append({CTxOut{0,CScript{}<<OP_RETURN}});
    auto& source=*extra_indices.back();
    const auto event=[&](NativeAuditAction action,uint8_t slot,const CScript& actor,const Bytes& body,unsigned scalar){
        NativeAuditEvent e{lease.owner.Id(),source.GetBlockHash(),action,slot,0,actor,body,{}};
        const auto identity=NativeAuditWorkIdentity(chain,hiding.root,e);
        e.signature=sign(*ScriptWorkChallenge({chain,identity,e.parent},actor,{128,4096}),known(scalar));
        const auto script=BuildNativeAuditEnvelope(e);need(script.has_value(),"Signed audit envelope failed");
        return CTxOut{0,*script};
    };
    std::vector<CTxOut> events;
    const auto partitions=NativeAuditPartitions(hiding.root,source.GetBlockHash());
    for(uint8_t slot=0;slot<2;++slot)
        events.push_back(event(NativeAuditAction::Assign,slot,slot?auditor1:auditor0,{},3+slot));
    std::array<Bytes,2> answers;
    for(uint8_t slot=0;slot<2;++slot){
        std::ostringstream answer;
        answer<<"{\"schema\":\"PGN_AUDIT_ANSWER_V1\",\"ticket\":\""<<HexStr(range.ticket_id)
              <<"\",\"attempt\":0,\"nonce\":\""<<std::string(64,char('2'+slot))<<"\",\"partitions\":[";
        for(size_t k=0;k<5;++k){if(k)answer<<',';answer<<partitions[slot][k];}answer<<"],\"counts\":[";
        for(size_t k=0;k<5;++k){if(k)answer<<',';answer<<'"'<<(report[partitions[slot][k]].records.size()+((failed&&slot==1)?1:0))<<'"';}
        answer<<"]}";const auto text=answer.str();answers[slot]=Bytes{text.begin(),text.end()};
        const auto digest=AuditSha(answers[slot]);
        events.push_back(event(NativeAuditAction::Commit,slot,slot?auditor1:auditor0,Bytes{digest.begin(),digest.end()},3+slot));
    }
    events.push_back(event(NativeAuditAction::ReportReveal,0,miner,report_bytes,1));
    for(uint8_t slot=0;slot<(one?1:2);++slot)
        events.push_back(event(NativeAuditAction::Reveal,slot,slot?auditor1:auditor0,answers[slot],3+slot));
    append(events);
    const auto& at=*extra_indices.back();
    const auto current=DeriveNativeCandidateReservation(view,at,*ranges.policy,history,schedule,lease.owner.Id());
    need(current.reservation.has_value(),"Current ticket reservation failed");
    const auto context=NativeProfiledWorkContext(view,at,*ranges.policy,*science.policy,*current.reservation,root);
    need(context.has_value(),"Current Gold context missing");
    const auto identity=ScientificWorkIdentity(*context);need(identity.has_value(),"Scientific identity missing");
    const auto signature=sign(*ScriptWorkChallenge({chain,*identity,at.GetBlockHash()},miner,{128,4096}),key);
    std::vector<CandidateBlockScienceProof> proofs;
    for(size_t j=0;j<6;++j){
        const auto cert=ParseHex(Witnesses[j].proof);
        EncodedCommittedGapEvidence evidence{encodeSegment(range,report[segments[j]],encoding),
            proveSegment(range,report,segments[j],encoding,sha),records[j],Bytes{cert.begin(),cert.end()}};
        proofs.push_back({lease.owner.Id(),root,signature,std::move(evidence)});
    }
    const auto lower_of=[&](const auto& proof){return decodeCertifiedGap(proof.evidence.scientific_bytes,
        {context->proof_encoding,context->policy.scientific}).lower;};
    std::sort(proofs.begin(),proofs.end(),[&](const auto& a,const auto& b){return lower_of(a)<lower_of(b);});
    const auto checked=VerifyNativeTicketGoldGroup(view,at,history,proofs,body_budget);
    if(one||failed){
        need(!checked.group&&checked.verdict==TicketGoldGroupVerdict::AuditsIncomplete,"Missing or failed audits accepted");
        std::cout<<"{\"PASS\":true,\"missing_or_failed_audits_refused\":true,\"production_changed\":false}\n";return 0;
    }
    need(checked.group&&checked.group->GoldCount()==6,"Six exact Golds did not verify under one ticket");
    auto wrong=proofs;wrong[1]=wrong[0];
    need(VerifyNativeTicketGoldGroup(view,at,history,wrong,body_budget).verdict==TicketGoldGroupVerdict::DuplicateGold,"Duplicate accepted");
    wrong=proofs;std::swap(wrong[0],wrong[1]);
    need(VerifyNativeTicketGoldGroup(view,at,history,wrong,body_budget).verdict==TicketGoldGroupVerdict::NoncanonicalOrder,"Order changed");
    wrong=proofs;wrong[0].evidence.scientific_bytes.back()^=1;
    need(!VerifyNativeTicketGoldGroup(view,at,history,wrong,body_budget).group,"Bad exact certificate accepted");
    wrong=proofs;wrong.back().signature[0]^=1;
    need(!VerifyNativeTicketGoldGroup(view,at,history,wrong,body_budget).group,"Bad owner accepted");
    uint256 next_hash;next_hash.begin()[0]=71;
    CBlockIndex proposed;proposed.phashBlock=&next_hash;proposed.pprev=const_cast<CBlockIndex*>(&at);
    proposed.nHeight=at.nHeight+1;proposed.nTx=1;
    const auto forward=PrepareNativeTicketGoldGroupForward(view,at,proposed,*checked.group);
    need(forward.forward.Valid()&&forward.forward.changes.size()==6,"One atomic forward lost a Gold");
    for(const auto& change:forward.forward.changes)need(!view.GetPgnFact(change.fact),"Preparation mutated current state");
    std::cout<<"{\"PASS\":true,\"exact_golds_in_one_ticket\":6,\"atomic_fact_changes\":6,"
               "\"duplicates_order_bad_proof_bad_signature_refused\":true,\"production_changed\":false}\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
