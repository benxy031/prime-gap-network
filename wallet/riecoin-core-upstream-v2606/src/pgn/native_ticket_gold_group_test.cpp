// Synthetic authenticated history and six REAL exact gap certificates.
// This exercises the group verifier, not native block acceptance, full-range
// completeness, fresh work/fork choice, currency issuance or network activation.
#include "native_ticket_gold_money.h"
#include "ticket_gold_witnesses_test.h"
#include <pgn/native_report_commitment.h>
#include <pgn/chain_monetary_policy.h>
#include <common/signmessage.h>
#include <consensus/merkle.h>
#include <consensus/tx_check.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <policy/policy.h>
#include <script/interpreter.h>
#include <crypto/sha256.h>
#include <key.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/solver.h>
#include <streams.h>
#include <txdb.h>
#include <validation.h>
#include <consensus/params.h>
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
    const Integer lower{std::string("150000000000000000000")};
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
    // Seed the disposable funding coin during fixture initialization, BEFORE
    // any tracked issuance transition. Never flush an unrelated dirty child
    // through a parent with an uncommitted monetary history.
    {uint256 funding=chain;funding.begin()[1]=81;
     view.AddCoin(COutPoint{Txid::FromUint256(funding),0},Coin{CTxOut{unit,miner},0,false},false);}
    NativeRequestHistoryArchive history{path/"history",1<<20};
    // Actual node uses 4,000,000 total script bytes; the old fixture was limited to16KiB.
    const ConfirmedRequestScanBudget scan{16,4,4000000,1,request_budget};
    const RequestReplayBudget replay{32,16,{4000000,1U<<20},scan};
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
        need(value>=range.start&&value<range.start+range.length,"Fixture witness outside the ticket");
        uint32_t segment=0;
        while(segment<100){const auto bounds=segmentBounds(range,segment);if(value>=bounds.first&&value<bounds.second)break;++segment;}
        need(segment<100,"Existing certificate outside fixture ticket");segments[i]=segment;records[i]=report[segment].records.size();report[segment].records.push_back({value,Integer{Witnesses[i].gap},0});
    }
    for(auto& segment:report)std::sort(segment.records.begin(),segment.records.end(),[](const auto& a,const auto& b){return a.lower<b.lower;});
    for(size_t i=0;i<6;++i){const Integer value{std::string(Witnesses[i].lower)};const auto& list=report[segments[i]].records;
        const auto at=std::find_if(list.begin(),list.end(),[&](const auto& r){return r.lower==value;});
        need(at!=list.end(),"Fixture record lost after canonical ordering");records[i]=std::distance(list.begin(),at);}
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
    CMutableTransaction batch_tx;batch_tx.vout=events;
    CBlock batched;batched.vtx={MakeTransactionRef(batch_tx)};
    need(VerifyProposedNativeAuditEvents(view,source,history,batched,policy_budget,schedule,{128,4096}),
         "Real signed audit events cannot share one block");
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
    const auto make_block=[&](const auto& supplied){
        CMutableTransaction tx;tx.vin.resize(1);tx.vin[0].prevout.SetNull();
        tx.vin[0].scriptSig=CScript{}<<int64_t(at.nHeight+1)<<OP_0;
        for(const auto& proof:supplied){
            const auto envelope=BuildCandidateBlockScienceEnvelope(proof,body_budget);
            need(envelope.has_value(),"Fixture Gold envelope failed");tx.vout.push_back(CTxOut{0,*envelope});
        }
        CBlock block;block.hashPrevBlock=at.GetBlockHash();block.nTime=at.nTime+1;
        block.vtx={MakeTransactionRef(tx)};block.hashMerkleRoot=BlockMerkleRoot(block);return block;
    };
    const auto actual=make_block(proofs);
    const auto whole=VerifyNativeTicketGoldBlock(view,at,history,actual,body_budget);
    if(one||failed)need(!whole.block&&whole.group_verdict==TicketGoldGroupVerdict::AuditsIncomplete,"Whole block bypassed audits");
    const auto checked=VerifyNativeTicketGoldGroup(view,at,history,proofs,body_budget);
    if(one||failed){
        need(!checked.group&&checked.verdict==TicketGoldGroupVerdict::AuditsIncomplete,"Missing or failed audits accepted");
        std::cout<<"{\"PASS\":true,\"missing_or_failed_audits_refused\":true,\"production_changed\":false}\n";return 0;
    }
    need(checked.group&&checked.group->GoldCount()==6,"Six exact Golds did not verify under one ticket");
    if(!whole.block)std::cerr<<"Whole-body verdict="<<int(whole.verdict)<<" group="<<(whole.group_verdict?int(*whole.group_verdict):-1)<<'\n';
    need(whole.block&&whole.block->Block()==actual.GetHash()&&whole.block->Group().GoldCount()==6,"Actual six-Gold block did not verify");
    auto old_scan=body_budget;old_scan.schedule.scan.maximum_script_bytes=16384;
    need(!VerifyNativeTicketGoldBlock(view,at,history,actual,old_scan).block,"Original undersized scan fixture should be rejected");
    CBlock invalid=actual;invalid.hashMerkleRoot.begin()[0]^=1;
    need(!VerifyNativeTicketGoldBlock(view,at,history,invalid,body_budget).block,"Wrong native Merkle root accepted");
    invalid=actual;invalid.hashPrevBlock.begin()[0]^=1;
    need(!VerifyNativeTicketGoldBlock(view,at,history,invalid,body_budget).block,"Wrong parent accepted");
    auto duplicate=proofs;duplicate[1]=duplicate[0];
    need(!VerifyNativeTicketGoldBlock(view,at,history,make_block(duplicate),body_budget).block,"Whole block duplicated Gold");
    auto reversed=proofs;std::swap(reversed[0],reversed[1]);
    need(!VerifyNativeTicketGoldBlock(view,at,history,make_block(reversed),body_budget).block,"Whole block changed canonical order");
    auto mixed=proofs;mixed.back().request.begin()[0]^=1;
    need(!VerifyNativeTicketGoldBlock(view,at,history,make_block(mixed),body_budget).block,"Whole block mixed tickets");
    auto tiny=body_budget;tiny.maximum_payload_bytes=100;
    need(!VerifyNativeTicketGoldBlock(view,at,history,actual,tiny).block,"Whole block exceeded aggregate payload budget");
    const auto actual_hash=actual.GetHash();CBlockIndex native_index;
    native_index.phashBlock=&actual_hash;native_index.pprev=const_cast<CBlockIndex*>(&at);
    native_index.nHeight=at.nHeight+1;native_index.nTx=actual.vtx.size();
    const auto native_forward=PrepareNativeTicketGoldBlockForward(view,at,native_index,*whole.block);
    need(native_forward.forward.Valid()&&native_forward.forward.changes.size()==6,"Actual body lost atomic Gold changes");
    CBlockIndex changed_index;uint256 changed_hash=actual_hash;changed_hash.begin()[0]^=1;changed_index.phashBlock=&changed_hash;
    changed_index.pprev=native_index.pprev;changed_index.nHeight=native_index.nHeight;changed_index.nTx=native_index.nTx;
    bool changed_rejected=false;try{PrepareNativeTicketGoldBlockForward(view,at,changed_index,*whole.block);}catch(const std::logic_error&){changed_rejected=true;}
    need(changed_rejected,"Prepared forward accepted changed actual block");
    IssuanceSnapshot initial{at.GetBlockHash(),PGN_MAX_SUPPLY_ATOMIC,0,0,
        IssuanceBudgetPartition{money.policy->Id(),money.policy->Definition().ordinary_cap,0,0,0,0},OrdinaryRewardPools{0,0}};
    need(initial.Valid(),"Fixture initial monetary snapshot invalid");view.StagePgnIssuanceCommit({{},initial});
    const auto money_plan=PrepareNativeTicketGoldMonetaryPlan(view,at,actual,*whole.block,*money.policy);
    need(money_plan&&money_plan->GoldCount()==6&&money_plan->Gross()>0,"Six-Gold monetary quote missing");
    need(!VerifyNativeTicketGoldCoinbase(*money_plan,actual,0),"Missing six-Gold payout was accepted");
    CBlock paid=actual;CMutableTransaction paid_cb{*actual.vtx.front()};
    paid_cb.vout.insert(paid_cb.vout.begin(),CTxOut{money_plan->WinnerSubsidy(),money_plan->Recipient()});
    paid.vtx.front()=MakeTransactionRef(paid_cb);paid.hashMerkleRoot=BlockMerkleRoot(paid);
    const auto paid_whole=VerifyNativeTicketGoldBlock(view,at,history,paid,body_budget);
    need(paid_whole.block.has_value(),"Paid whole body lost exact Gold verification");
    const auto paid_plan=PrepareNativeTicketGoldMonetaryPlan(view,at,paid,*paid_whole.block,*money.policy);
    need(paid_plan&&VerifyNativeTicketGoldCoinbase(*paid_plan,paid,0),"Exact six-Gold payout rejected");
    need(paid_plan->Gross()==money_plan->Gross()&&paid_plan->WinnerSubsidy()==money_plan->WinnerSubsidy(),"Payout changed grant quote");
    CBlock overpaid=paid;CMutableTransaction bad_cb{*paid.vtx.front()};++bad_cb.vout.front().nValue;
    overpaid.vtx.front()=MakeTransactionRef(bad_cb);overpaid.hashMerkleRoot=BlockMerkleRoot(overpaid);
    const auto bad_whole=VerifyNativeTicketGoldBlock(view,at,history,overpaid,body_budget);
    need(bad_whole.block.has_value(),"Payout fixture should retain valid science only");
    const auto bad_plan=PrepareNativeTicketGoldMonetaryPlan(view,at,overpaid,*bad_whole.block,*money.policy);
    need(bad_plan&&!VerifyNativeTicketGoldCoinbase(*bad_plan,overpaid,0),"One atomic unit overpaid was accepted");
    const auto final_hash=paid.GetHash();CBlockIndex final_index;
    final_index.phashBlock=&final_hash;final_index.pprev=const_cast<CBlockIndex*>(&at);
    final_index.nHeight=at.nHeight+1;final_index.nTx=paid.vtx.size();
    const auto final_facts=PrepareNativeTicketGoldBlockForward(view,at,final_index,*paid_whole.block);
    // The synthetic history above was staged directly. Publish that fixture
    // prefix before testing tracked native history, exactly as a loaded parent.
    view.Flush();
    const auto before_money=view.GetPgnIssuanceState();const auto before_head=view.GetPgnFactHead();
    {CCoinsViewCache child{&view};child.SetBestBlock(final_hash);
     child.StagePgnFactHistoryStep(final_facts);child.StagePgnIssuanceHistoryStep(paid_plan->Forward());child.Flush();}
    need(view.GetPgnIssuanceState()==std::optional{paid_plan->Forward().forward.after}&&view.GetPgnFactHead()==std::optional{final_facts.forward.after},"Grouped native state mismatch");
    for(const auto& change:final_facts.forward.changes)need(view.GetPgnFact(change.fact)==change.after,"Grouped fact was lost");
    {CCoinsViewCache undo{&view};undo.SetBestBlock(at.GetBlockHash());
     undo.StagePgnIssuanceHistoryStep({IssuanceDirection::Rewind,paid_plan->Forward().forward});
     undo.StagePgnFactHistoryStep({FactDirection::Rewind,final_facts.forward});undo.Flush();}
    need(view.GetPgnIssuanceState()==before_money&&view.GetPgnFactHead()==before_head,"Grouped monetary/fact undo differs");
    for(const auto& change:final_facts.forward.changes)need(!view.GetPgnFact(change.fact),"Grouped fact survived undo");
    { // Boundary authentication only: these snapshots isolate ordinal crossing;
    // the separate financial test below supplies its exact accrued audit reserve.
    auto boundary_before=initial;boundary_before.epochs=GoldEpochBalances{1007,at.nHeight-1,0,0,0,0};
    auto boundary_after=boundary_before;boundary_after.tip=final_hash;
    boundary_after.epochs->golds=1013;boundary_after.epochs->last_gold_height=final_index.nHeight;
    IssuanceCommit crossing{boundary_before,boundary_after};
    need(crossing.Valid()&&NativeAuditCutoffCrossed(crossing,final_index,1008),"Multi-Gold cutoff crossing was lost");
    need(!NativeAuditCutoffCrossed(crossing,final_index,1007)&&!NativeAuditCutoffCrossed(crossing,final_index,1014),"Uncrossed cutoff accepted");
    auto corrupt_crossing=crossing;corrupt_crossing.after.tip.begin()[0]^=1;
    need(!NativeAuditCutoffCrossed(corrupt_crossing,final_index,1008),"Wrong cutoff block accepted");
    corrupt_crossing=crossing;corrupt_crossing.before->tip.begin()[0]^=1;
    need(!NativeAuditCutoffCrossed(corrupt_crossing,final_index,1008),"Wrong cutoff parent accepted");
    corrupt_crossing=crossing;corrupt_crossing.after.epochs->last_gold_height--;
    need(!NativeAuditCutoffCrossed(corrupt_crossing,final_index,1008),"Non-Gold cutoff anchor accepted");
    auto exact_crossing=crossing;exact_crossing.after.epochs->golds=1008;
    need(NativeAuditCutoffCrossed(exact_crossing,final_index,1008),"Legacy single-Gold crossing changed");
    corrupt_crossing=crossing;corrupt_crossing.before->epochs->golds=1013;
    need(!NativeAuditCutoffCrossed(corrupt_crossing,final_index,1008),"Cutoff reused by a later block");
    const auto audited=ReadNativeAuditLedger(view,at,history,policy_budget,schedule,{128,4096});
    need(audited.has_value(),"Authenticated audit ledger missing for settlement test");
    const GoldEpochDefinition epochs{1008000,1008,10*unit,3*unit,2*unit,unit,12600*unit,unit,32U*65536U};
    const auto accrued=QuoteCumulativeAuditAccrual(epochs,1013),reserved=QuoteCumulativeAuditAccrual(epochs,1008);
    need(accrued&&reserved&&*accrued>*reserved,"Crossing reserve fixture invalid");
    auto settled=initial;settled.promised=*accrued;settled.partition->ordinary_promised=*accrued;
    settled.pools->audit=*accrued;settled.epochs=GoldEpochBalances{1013,at.nHeight,0,0,0,0};
    settled.audit_settlement=AuditSettlementBalances{};need(settled.Valid(),"Settlement fixture invalid");
    const auto unsettled=settled;
    const auto page=ApplyAuditSettlementPage(*audited,epochs,128,1008,at.nHeight,at.GetBlockHash(),settled);
    need(page&&page->count==2&&page->paid==*reserved&&settled.minted==*reserved&&
         settled.promised==*accrued-*reserved&&settled.pools->audit==*accrued-*reserved&&
         settled.audit_settlement->completed_golds==1008&&!settled.audit_settlement->active,
         "Crossing settlement changed pot, lost tasks or paid unreserved Golds");
    const auto once=settled;const auto repeat=ApplyAuditSettlementPage(*audited,epochs,128,1008,at.nHeight,at.GetBlockHash(),settled);
    need(repeat&&repeat->paid==0&&repeat->count==0&&settled==once,"Audit pot paid twice after multi-Gold crossing");
    auto partial=unsettled;const auto first_page=ApplyAuditSettlementPage(*audited,epochs,1,1008,at.nHeight,at.GetBlockHash(),partial);
    need(first_page&&first_page->count==1&&partial.audit_settlement->active,"Partial payout fixture missing");
    auto altered=*audited;bool changed_task=false;
    for(auto& [id,r]:altered){for(auto& job:r.jobs)if(job&&job->stage==NativeAuditJob::Passed){job->passed_height=0;changed_task=true;break;}if(changed_task)break;}
    need(changed_task,"No authenticated passed audit for changed-root negative test");
    const auto partial_before=partial;
    need(!ApplyAuditSettlementPage(altered,epochs,1,1008,at.nHeight,at.GetBlockHash(),partial)&&partial==partial_before,
         "Changed payout task set caused a partial mutation");
    // Spend a real signed native UTXO. The candidate's fee input comes only
    // from inherited native amount/input validation, never from a sender DTO.
    CCoinsViewCache fee_inputs{&view};uint256 funding=chain;funding.begin()[1]=81;
    const COutPoint input{Txid::FromUint256(funding),0};const CAmount funding_value=unit;
    need(fee_inputs.AccessCoin(input).out==CTxOut{funding_value,miner},"Disposable funding coin missing");
    CMutableTransaction transfer;transfer.vin.emplace_back(input);transfer.vout.emplace_back(funding_value-1530,alternate);
    FillableSigningProvider signer;signer.AddKey(key);SignatureData input_signature;
    need(ProduceSignature(signer,MutableTransactionSignatureCreator(transfer,0,funding_value,SignOptions{SIGHASH_ALL}),
                          miner,input_signature),"Native ordinary transaction could not sign");
    UpdateInput(transfer.vin[0],input_signature);const CTransaction signed_transfer{transfer};
    TxValidationState tx_state;CAmount native_fees{};
    need(CheckTransaction(signed_transfer,tx_state,Consensus::AmountRules::ApprovedPgn())&&
         Consensus::CheckTxInputs(signed_transfer,tx_state,fee_inputs,at.nHeight+1,native_fees)&&native_fees==1530,
         "Native signed UTXO fee calculation failed");
    const PrecomputedTransactionData txdata{signed_transfer};ScriptError script_error;
    need(VerifyScript(signed_transfer.vin[0].scriptSig,miner,&signed_transfer.vin[0].scriptWitness,
                      STANDARD_SCRIPT_VERIFY_FLAGS,TransactionSignatureChecker(&signed_transfer,0,funding_value,txdata,MissingDataBehavior::FAIL),&script_error),
         "Native ordinary input script failed");
    auto wrong_signature=transfer;wrong_signature.vin[0].scriptWitness.stack[0][0]^=1;
    const CTransaction bad_transfer{wrong_signature};const PrecomputedTransactionData bad_txdata{bad_transfer};
    need(!VerifyScript(bad_transfer.vin[0].scriptSig,miner,&bad_transfer.vin[0].scriptWitness,
                       STANDARD_SCRIPT_VERIFY_FLAGS,TransactionSignatureChecker(&bad_transfer,0,funding_value,bad_txdata,MissingDataBehavior::FAIL),&script_error),
         "Corrupt native ordinary transaction signature accepted");
    auto overspend=transfer;overspend.vout.front().nValue=funding_value+1;
    TxValidationState overspend_state;CAmount overspend_fee{};
    need(!Consensus::CheckTxInputs(CTransaction{overspend},overspend_state,fee_inputs,at.nHeight+1,overspend_fee),
         "Native ordinary transaction overspend accepted");
    CBlock fee_block=paid;fee_block.vtx.push_back(MakeTransactionRef(transfer));CMutableTransaction fee_cb{*paid.vtx.front()};
    fee_cb.vout.front().nValue+=native_fees;
    // Same witness commitment construction as the inherited Core generator.
    const Bytes witness_nonce(32,0);uint256 witness_root=BlockWitnessMerkleRoot(fee_block);
    CHash256{}.Write(witness_root).Write(witness_nonce).Finalize(witness_root);
    CScript commitment;commitment.resize(38);commitment[0]=OP_RETURN;commitment[1]=0x24;
    commitment[2]=0xaa;commitment[3]=0x21;commitment[4]=0xa9;commitment[5]=0xed;
    std::memcpy(&commitment[6],witness_root.begin(),32);fee_cb.vout.emplace_back(0,commitment);
    fee_cb.vin[0].scriptWitness.stack={witness_nonce};fee_block.vtx.front()=MakeTransactionRef(fee_cb);
    fee_block.hashMerkleRoot=BlockMerkleRoot(fee_block);
    const auto fee_science=VerifyNativeTicketGoldBlock(view,at,history,fee_block,body_budget);
    need(fee_science.block.has_value(),"Fee block lost verified science");
    const auto fee_plan=PrepareNativeTicketGoldMonetaryPlan(view,at,fee_block,*fee_science.block,*money.policy);
    need(fee_plan&&VerifyNativeTicketGoldCoinbase(*fee_plan,fee_block,native_fees)&&
         !VerifyNativeTicketGoldCoinbase(*fee_plan,fee_block,native_fees-1)&&
         !VerifyNativeTicketGoldCoinbase(*fee_plan,fee_block,-1),"Fees were multiplied per Gold or overpayment accepted");
    Consensus::Params native_rules{};native_rules.amount_rules=Consensus::AmountRules::ApprovedPgn();
    BlockValidationState native_block_state;
    // These context-free checks deliberately do not assert header/fork PoW.
    need(CheckBlock(fee_block,native_block_state,native_rules,false,true)&&!IsBlockMutated(fee_block,true),
         "Inherited native body/Merkle/witness checks rejected the six-Gold block");
    CBlock bad_witness=fee_block;auto changed_transfer=transfer;
    changed_transfer.vin[0].scriptWitness.stack[0][0]^=1;bad_witness.vtx[1]=MakeTransactionRef(changed_transfer);
    bad_witness.fChecked=false;bad_witness.checked_amount_max.reset();bad_witness.m_checked_merkle_root=false;bad_witness.m_checked_witness_commitment=false;
    need(IsBlockMutated(bad_witness,true),"Native witness commitment accepted a changed transaction witness");
    auto duplicate_input=transfer;duplicate_input.vin.push_back(duplicate_input.vin.front());TxValidationState duplicate_state;
    need(!CheckTransaction(CTransaction{duplicate_input},duplicate_state,Consensus::AmountRules::ApprovedPgn()),
         "Inherited duplicate-input check was bypassed");
    const auto fee_hash=fee_block.GetHash();CBlockIndex fee_index;fee_index.phashBlock=&fee_hash;
    fee_index.pprev=const_cast<CBlockIndex*>(&at);fee_index.nHeight=at.nHeight+1;fee_index.nTx=fee_block.vtx.size();
    const auto fee_facts=PrepareNativeTicketGoldBlockForward(view,at,fee_index,*fee_science.block);
    view.Flush();
    const auto fee_before_money=view.GetPgnIssuanceState();const auto fee_before_head=view.GetPgnFactHead();
    Coin spent_input;const COutPoint created{signed_transfer.GetHash(),0};
    {CCoinsViewCache child{&view};need(child.SpendCoin(input,&spent_input),"Native input disappeared before combined commit");
     child.AddCoin(created,Coin{signed_transfer.vout[0],fee_index.nHeight,false},false);child.SetBestBlock(fee_hash);
     child.StagePgnFactHistoryStep(fee_facts);child.StagePgnIssuanceHistoryStep(fee_plan->Forward());child.Flush();}
    need(view.AccessCoin(input).IsSpent()&&!view.AccessCoin(created).IsSpent()&&view.GetBestBlock()==fee_hash&&
         view.GetPgnIssuanceState()==std::optional{fee_plan->Forward().forward.after},
         "Combined native UTXO/fact/money commit diverged");
    {CCoinsViewCache undo{&view};need(undo.SpendCoin(created),"Native created output disappeared before undo");
     undo.AddCoin(input,std::move(spent_input),false);undo.SetBestBlock(at.GetBlockHash());
     undo.StagePgnIssuanceHistoryStep({IssuanceDirection::Rewind,fee_plan->Forward().forward});
     undo.StagePgnFactHistoryStep({FactDirection::Rewind,fee_facts.forward});undo.Flush();}
    need(view.GetBestBlock()==at.GetBlockHash()&&view.GetPgnIssuanceState()==fee_before_money&&view.GetPgnFactHead()==fee_before_head&&
         view.AccessCoin(created).IsSpent()&&view.AccessCoin(input).out==CTxOut{funding_value,miner},
         "Combined native UTXO/fact/money undo failed to restore its parent");
    }
    { // A derived ticket closure follows native ancestry, not a per-process
    // counter. Historical bodies here have already passed the verifier above.
    view.Flush();CBlockIndex closed_index{paid};closed_index.phashBlock=&final_hash;
    closed_index.pprev=const_cast<CBlockIndex*>(&at);closed_index.nHeight=at.nHeight+1;
    closed_index.nTx=paid.vtx.size();closed_index.nStatus=BLOCK_VALID_SCRIPTS;closed_index.BuildSkip();
    {CCoinsViewCache child{&view};child.SetBestBlock(final_hash);
     child.StagePgnFactHistoryStep(final_facts);child.StagePgnIssuanceHistoryStep(paid_plan->Forward());child.Flush();}
    need(history.StoreBlock(view,closed_index,closed_index.nHeight,paid,replay.archive)==RequestArchiveVerdict::Stored,
         "Verified closed-ticket body was not retained");
    const auto closed=ReadNativeTicketClosures(view,closed_index,history,body_budget);
    need(closed&&closed->extra.tickets.size()==1&&closed->extra.tickets.contains(current.reservation->Ticket()),
         "Native history did not reconstruct the closed ticket");
    const auto& closure=closed->extra.tickets.at(current.reservation->Ticket());
    need(closure.request==lease.owner.Id()&&closure.block==final_hash&&closure.golds==6,
         "Derived ticket closure lost its actual block or Gold count");
    const auto closed_money=view.GetPgnIssuanceState();const auto closed_head=view.GetPgnFactHead();
    const auto second=VerifyNativeTicketGoldGroup(view,closed_index,history,proofs,body_budget);
    need(!second.group&&second.verdict==TicketGoldGroupVerdict::TicketAlreadyClosed&&
         view.GetPgnIssuanceState()==closed_money&&view.GetPgnFactHead()==closed_head,
         "Closed ticket was admitted twice or the refusal mutated native state");
    auto caller_copy=*closed;caller_copy.extra.tickets.clear();
    const auto warm=ReadNativeTicketClosures(view,closed_index,history,body_budget);
    need(warm&&warm->extra.tickets==closed->extra.tickets,"Caller changed cached closure authority");
    DataStream closure_wire;native_audit_replay_detail::ExtraCodec<NativeTicketClosures>::Write(closure_wire,closed->extra,nullptr);
    need(native_audit_replay_detail::ExtraCodec<NativeTicketClosures>::Read(closure_wire).tickets==closed->extra.tickets&&closure_wire.empty(),
         "Native closure checkpoint did not round-trip exactly");
    DataStream duplicate_closure;WriteCompactSize(duplicate_closure,2);
    duplicate_closure<<current.reservation->Ticket()<<closure<<current.reservation->Ticket()<<closure;
    bool duplicate_closed=false;try{native_audit_replay_detail::ExtraCodec<NativeTicketClosures>::Read(duplicate_closure);}catch(const std::exception&){duplicate_closed=true;}
    need(duplicate_closed,"Duplicate closure checkpoint was accepted");
    NativeRequestHistoryArchive missing_history{path/"missing-closure-history",1<<20};
    need(!ReadNativeTicketClosures(view,closed_index,missing_history,body_budget),"Warm closure cache bypassed unavailable history");
    {CCoinsViewCache undo{&view};undo.SetBestBlock(at.GetBlockHash());
     undo.StagePgnIssuanceHistoryStep({IssuanceDirection::Rewind,paid_plan->Forward().forward});
     undo.StagePgnFactHistoryStep({FactDirection::Rewind,final_facts.forward});undo.Flush();}
    const auto reopened=ReadNativeTicketClosures(view,at,history,body_budget);
    need(reopened&&reopened->extra.tickets.empty()&&view.GetPgnIssuanceState()==before_money&&view.GetPgnFactHead()==before_head,
         "Closure index survived a native rollback to its parent");
    const auto after_undo=VerifyNativeTicketGoldGroup(view,at,history,proofs,body_budget);
    need(after_undo.group&&after_undo.group->GoldCount()==6,"Legitimate ticket was not reopened on the restored parent");
    }
    auto wrong=proofs;wrong[1]=wrong[0];
    need(VerifyNativeTicketGoldGroup(view,at,history,wrong,body_budget).verdict==TicketGoldGroupVerdict::DuplicateGold,"Duplicate accepted");
    wrong=proofs;std::swap(wrong[0],wrong[1]);
    need(VerifyNativeTicketGoldGroup(view,at,history,wrong,body_budget).verdict==TicketGoldGroupVerdict::NoncanonicalOrder,"Order changed");
    wrong=proofs;wrong[0].evidence.scientific_bytes.front()^=0xff;
    need(!VerifyNativeTicketGoldGroup(view,at,history,wrong,body_budget).group,"Bad exact certificate accepted");
    wrong=proofs;wrong.back().signature[0]^=1;
    need(!VerifyNativeTicketGoldGroup(view,at,history,wrong,body_budget).group,"Bad owner accepted");
    uint256 next_hash;next_hash.begin()[0]=71;
    CBlockIndex proposed;proposed.phashBlock=&next_hash;proposed.pprev=const_cast<CBlockIndex*>(&at);
    proposed.nHeight=at.nHeight+1;proposed.nTx=1;
    const auto forward=PrepareNativeTicketGoldGroupForward(view,at,proposed,*checked.group);
    need(forward.forward.Valid()&&forward.forward.changes.size()==6,"One atomic forward lost a Gold");
    for(const auto& change:forward.forward.changes)need(!view.GetPgnFact(change.fact),"Preparation mutated current state");
    std::cout<<"{\"PASS\":true,\"exact_golds_in_one_ticket\":6,\"atomic_fact_changes\":6,\"actual_whole_body_checked\":true,"
               "\"duplicates_order_bad_proof_bad_signature_refused\":true,\"production_changed\":false}\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
