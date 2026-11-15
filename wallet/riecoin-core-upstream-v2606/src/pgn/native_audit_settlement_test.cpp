// Closed native financial-seam QA. Controlled operators and synthetic counts;
// this does not qualify independent mathematics, economic independence or P2P.
#undef NDEBUG
#include <pgn/native_audit_settlement.h>
#include <chainparams.h>
#include <key.h>
#include <script/descriptor.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/solver.h>
#include <txdb.h>
#include <wallet/receive.h>
#include <wallet/sqlite.h>
#include <wallet/wallet.h>
#include <sqlite3.h>
#include <cassert>
#include <cstdio>
#include <deque>
#include <memory>
#include <numeric>
using namespace pgn::core;
const TranslateFn G_TRANSLATION_FUN{nullptr};
namespace {
uint256 Id(unsigned n){char s[65];std::snprintf(s,sizeof(s),"%064x",n);return *uint256::FromHex(s);}
struct Actor {
    CKey key{GenerateRandomKey()};
    CTxDestination destination{WitnessV0KeyHash(key.GetPubKey())};
    CScript script{GetScriptForDestination(destination)};
    Bytes Sign(const std::string& challenge)const{
        MessageVerificationResult result;const auto txs=BIP322Txs::Create(destination,challenge,result);assert(txs);
        CMutableTransaction tx{txs->m_to_sign};FillableSigningProvider keys;keys.AddKey(key);
        std::map<COutPoint,Coin> coins{{tx.vin[0].prevout,Coin{txs->m_to_spend.vout[0],1,false}}};
        std::map<int,bilingual_str> errors;assert(SignTransaction(tx,&keys,coins,{.sighash_type=SIGHASH_ALL},errors));
        DataStream bytes;bytes<<tx.vin[0].scriptWitness.stack;
        return Bytes{reinterpret_cast<const unsigned char*>(bytes.data()),reinterpret_cast<const unsigned char*>(bytes.data())+bytes.size()};
    }
};
struct MemoryWalletDB:wallet::SQLiteDatabase {
    MemoryWalletDB():SQLiteDatabase(fs::PathFromString("qa"),fs::PathFromString("qa/wallet.dat"),wallet::DatabaseOptions{},SQLITE_OPEN_MEMORY){}
};
std::unique_ptr<wallet::CWallet> QaWallet(const Actor& actor){
    auto wallet=std::make_unique<wallet::CWallet>(nullptr,"audit-settlement-qa",std::make_unique<MemoryWalletDB>());
    LOCK(wallet->cs_wallet);wallet->SetWalletFlag(wallet::WALLET_FLAG_DESCRIPTORS);
    FlatSigningProvider provider;std::string error;
    auto descriptors=Parse("wpkh("+HexStr(actor.key.GetPubKey())+")",provider,error);assert(descriptors.size()==1);
    provider.keys.emplace(actor.key.GetPubKey().GetID(),actor.key);
    wallet::WalletDescriptor desc{std::move(descriptors.front()),1,0,1,1};
    assert(wallet->AddWalletDescriptor(desc,provider,"QA only",false));return wallet;
}
CMutableTransaction Coinbase(const AuditSettlementPage& page,unsigned height){
    CMutableTransaction tx;tx.vin.resize(1);tx.vin[0].prevout.SetNull();tx.vin[0].scriptSig=CScript{}<<int64_t(height)<<OP_0;
    tx.vout=page.outputs;if(page.receipt)tx.vout.emplace_back(0,*page.receipt);return tx;
}
struct Indexes {
    std::deque<uint256> hashes;std::deque<CBlockIndex> blocks;
    Indexes(){for(unsigned i=0;i<40;++i){hashes.push_back(Id(100+i));blocks.emplace_back();auto& b=blocks.back();b.phashBlock=&hashes.back();b.nHeight=i;b.nStatus=BLOCK_VALID_SCRIPTS;if(i)b.pprev=&blocks[i-1];b.BuildSkip();}}
};
void Enroll(NativeAuditLedger& ledger,const Actor& authority,const Actor& actor,unsigned id,const CBlockIndex& parent,const uint256& chain){
    NativeAuditEvent event{ledger.authority_request,parent.GetBlockHash(),NativeAuditAction::Enroll,0,0,authority.script};
    const auto body="{\"schema\":\"PGN_TESTNET_PARTICIPANT_V1\",\"operator_id\":\""+Id(id).GetHex()+"\",\"signing_script\":\""+HexStr(actor.script)+"\",\"may_audit\":true}";
    event.body=Bytes{body.begin(),body.end()};
    event.signature=authority.Sign(*ScriptWorkChallenge({chain,NativeAuditWorkIdentity(chain,{},event),event.parent},event.actor,{4096,4096}));
    ApplyNativeAuditEvent(ledger,event,parent,chain,{4096,4096});
}
std::string AuditAnswer(const NativeReportAudit& report,uint8_t slot,const CBlockIndex& parent){
    const auto partitions=NativeAuditPartitions(report.report.commitment.root,parent.GetAncestor(report.report.height+1)->GetBlockHash());
    std::string body="{\"schema\":\"PGN_AUDIT_ANSWER_V1\",\"ticket\":\""+HexStr(report.range.ticket_id)+"\",\"attempt\":0,\"partitions\":[";
    for(unsigned i=0;i<5;++i){if(i)body+=",";body+=std::to_string(partitions[slot][i]);}
    body+="],\"counts\":[\"0\",\"0\",\"0\",\"0\",\"0\"],\"nonce\":\""+std::string(64,'a')+"\"}";
    return body;
}
void PassedReceipt(NativeAuditLedger& ledger,const Actor& actor,const uint256& request,uint8_t slot,const CBlockIndex& parent,const uint256& chain){
    auto& report=ledger.at(request);auto& job=*report.jobs[slot];
    const auto body=AuditAnswer(report,slot,parent);
    NativeAuditEvent event{request,parent.GetBlockHash(),NativeAuditAction::Reveal,slot,0,actor.script,Bytes{body.begin(),body.end()}};
    job.commitment=HexStr(AuditSha(event.body)); // controlled precommitted QA fixture
    event.signature=actor.Sign(*ScriptWorkChallenge({chain,NativeAuditWorkIdentity(chain,report.report.commitment.root,event),event.parent},event.actor,{4096,4096}));
    auto forged=event;forged.body.back()^=1;bool rejected{};try{ApplyNativeAuditEvent(ledger,forged,parent,chain,{4096,4096});}catch(...){rejected=true;}assert(rejected);
    ApplyNativeAuditEvent(ledger,event,parent,chain,{4096,4096});assert(job.stage==NativeAuditJob::Passed&&job.passed_height==parent.nHeight+1);
    ApplyNativeAuditEvent(ledger,event,parent,chain,{4096,4096});assert(job.passed_height==parent.nHeight+1);
}
// Real BIP322 keys and the production state transition, with no enrollment.
void PublicAdmissionExercise(){
    Actor owner,first,second;Indexes indexes;const auto chain=Id(1),request=Id(701);
    NativeAuditLedger ledger;ledger.public_admission=true;NativeReportAudit report;
    report.owner=owner.script;report.report.height=1;report.report.commitment.root=Id(601);report.expected=std::array<uint64_t,100>{};
    ledger.emplace(request,report);const auto& parent=indexes.blocks[8];
    const auto eventFor=[&](const Actor& actor,NativeAuditAction action,uint8_t slot,Bytes body){
        NativeAuditEvent event{request,parent.GetBlockHash(),action,slot,0,actor.script,std::move(body)};
        event.signature=actor.Sign(*ScriptWorkChallenge({chain,NativeAuditWorkIdentity(chain,report.report.commitment.root,event),event.parent},event.actor,{4096,4096}));return event;
    };
    const auto rejected=[&](const NativeAuditEvent& event){bool refused=false;try{ApplyNativeAuditEvent(ledger,event,parent,chain,{4096,4096});}catch(...){refused=true;}assert(refused);};
    auto assign=eventFor(first,NativeAuditAction::Assign,0,NativeTimedAuditAssignmentBody());
    auto bad=assign;bad.signature.back()^=1;rejected(bad);assert(!ledger.at(request).jobs[0]);
    rejected(eventFor(owner,NativeAuditAction::Assign,0,NativeTimedAuditAssignmentBody()));
    ApplyNativeAuditEvent(ledger,assign,parent,chain,{4096,4096});
    rejected(eventFor(first,NativeAuditAction::Assign,1,NativeTimedAuditAssignmentBody()));
    ApplyNativeAuditEvent(ledger,eventFor(second,NativeAuditAction::Assign,1,NativeTimedAuditAssignmentBody()),parent,chain,{4096,4096});
    assert(ledger.participants.empty()&&ledger.authority.empty()&&NativeAuditOperatorsIndependent(ledger,ledger.at(request)));
    for(uint8_t slot=0;slot<2;++slot){const auto body=AuditAnswer(ledger.at(request),slot,parent);const auto digest=HexStr(AuditSha(std::span{reinterpret_cast<const unsigned char*>(body.data()),body.size()}));
        const auto& actor=slot?second:first;ApplyNativeAuditEvent(ledger,eventFor(actor,NativeAuditAction::Commit,slot,ParseHex(digest)),parent,chain,{4096,4096});}
    for(uint8_t slot=0;slot<2;++slot){const auto body=AuditAnswer(ledger.at(request),slot,parent);const auto& actor=slot?second:first;
        auto reveal=eventFor(actor,NativeAuditAction::Reveal,slot,Bytes{body.begin(),body.end()});bad=reveal;bad.body.back()^=1;rejected(bad);
        ApplyNativeAuditEvent(ledger,reveal,parent,chain,{4096,4096});ApplyNativeAuditEvent(ledger,reveal,parent,chain,{4096,4096});}
    const auto& checked=ledger.at(request);
    assert(checked.jobs[0]->stage==NativeAuditJob::Passed&&checked.jobs[1]->stage==NativeAuditJob::Passed&&ledger.participants.empty());
    // Signed audit answers do not substitute for the missing owner disclosure.
    assert(!checked.Audited());
    ledger.at(request).details_root=Digest{}; // controlled owner-disclosure fixture
    assert(ledger.at(request).Audited());
}
IssuanceSnapshot Initial(const GoldEpochDefinition& d){
    const auto pot=*QuoteCumulativeAuditAccrual(d,10);
    IssuanceSnapshot s{Id(110),PGN_MAX_SUPPLY_ATOMIC,0,pot,IssuanceBudgetPartition{Id(500),PGN_MAX_SUPPLY_ATOMIC,0,pot,0,0},OrdinaryRewardPools{0,pot},GoldEpochBalances{10,10,0,0,0,0},AuditSettlementBalances{}};
    assert(s.Valid());return s;
}
void Initialize(CCoinsViewDB& db,const IssuanceSnapshot& snapshot){
    CCoinsViewCache cache{&db};cache.SetBestBlock(snapshot.tip);
    cache.StagePgnFactCommit({{},FactHead{Id(1),snapshot.tip},{}});
    cache.StagePgnIssuanceCommit({{},snapshot});cache.Flush();
}
void Publish(CCoinsViewDB& db,const IssuanceSnapshot& before,const IssuanceSnapshot& after,const CTransaction& tx,unsigned height){
    CCoinsViewCache cache{&db};AddCoins(cache,tx,height,false);cache.SetBestBlock(after.tip);
    cache.StagePgnFactHistoryStep({FactDirection::Forward,{FactHead{Id(1),before.tip},FactHead{Id(1),after.tip},{}}});
    cache.StagePgnIssuanceHistoryStep({IssuanceDirection::Forward,{before,after}});cache.Flush();
}
}
int main(int argc,char** argv){
    assert(argc==2);ECC_Context ecc;SelectParams(ChainType::TESTNET);LOCK(cs_main);
    PublicAdmissionExercise();
    constexpr CAmount coin=PGN_ATOMIC_UNITS_PER_COIN;
    const GoldEpochDefinition definition{200000,10,171*coin,32*coin,24*coin,8*coin,125*coin,1000*coin,40*65536};
    assert(QuoteCumulativeAuditAccrual(definition,10)==125*coin);
    Actor authority,first,second;Indexes indexes;NativeAuditLedger ledger;ledger.authority=authority.script;ledger.authority_request=Id(2);
    Enroll(ledger,authority,first,3,indexes.blocks[2],Id(1));Enroll(ledger,authority,second,4,indexes.blocks[2],Id(1));
    for(unsigned i=0;i<2;++i){
        NativeReportAudit report;report.owner=authority.script;report.report.height=1;report.report.commitment.root=Id(600+i);report.details_root=Digest{};report.expected=std::array<uint64_t,100>{};
        report.jobs[0]=NativeAuditJob{first.script,0,3,30,NativeAuditJob::Committed};
        report.jobs[1]=NativeAuditJob{second.script,0,3,30,NativeAuditJob::Committed};ledger.emplace(Id(700+i),report);
        PassedReceipt(ledger,first,Id(700+i),0,indexes.blocks[7+i],Id(1));
        PassedReceipt(ledger,second,Id(700+i),1,indexes.blocks[i?11:8],Id(1));
    }
    { // Frozen pre-v6 bytes: no inferred state or automatic migration.
        {auto snapshot=Initial(definition);
        snapshot.audit_settlement.reset();
        snapshot.epochs.reset();
        snapshot.pools.reset();
        snapshot.partition.reset();
        assert(snapshot.Valid());DataStream encoded;encoded<<snapshot;
        const auto expected=ParseHex("016e00000000000000000000000000000000000000000000000000000000000000000064a7b3b6e00d00000000000000000054ce0923010000");
        assert(HexStr(encoded)==HexStr(expected));
        DataStream legacy{expected};IssuanceSnapshot decoded;legacy>>decoded;assert(decoded==snapshot&&legacy.empty());}
        {auto snapshot=Initial(definition);
        snapshot.audit_settlement.reset();
        snapshot.epochs.reset();
        snapshot.pools.reset();
        assert(snapshot.Valid());DataStream encoded;encoded<<snapshot;
        const auto expected=ParseHex("026e00000000000000000000000000000000000000000000000000000000000000000064a7b3b6e00d00000000000000000054ce0923010000f401000000000000000000000000000000000000000000000000000000000000000064a7b3b6e00d00000000000000000054ce092301000000000000000000000000000000000000");
        assert(HexStr(encoded)==HexStr(expected));
        DataStream legacy{expected};IssuanceSnapshot decoded;legacy>>decoded;assert(decoded==snapshot&&legacy.empty());}
        {auto snapshot=Initial(definition);
        snapshot.audit_settlement.reset();
        snapshot.epochs.reset();
        assert(snapshot.Valid());DataStream encoded;encoded<<snapshot;
        const auto expected=ParseHex("036e00000000000000000000000000000000000000000000000000000000000000000064a7b3b6e00d00000000000000000054ce0923010000f401000000000000000000000000000000000000000000000000000000000000000064a7b3b6e00d00000000000000000054ce09230100000000000000000000000000000000000000000000000000000054ce0923010000");
        assert(HexStr(encoded)==HexStr(expected));
        DataStream legacy{expected};IssuanceSnapshot decoded;legacy>>decoded;assert(decoded==snapshot&&legacy.empty());}
        {auto snapshot=Initial(definition);
        snapshot.audit_settlement.reset();
        assert(snapshot.Valid());DataStream encoded;encoded<<snapshot;
        const auto expected=ParseHex("046e00000000000000000000000000000000000000000000000000000000000000000064a7b3b6e00d00000000000000000054ce0923010000f401000000000000000000000000000000000000000000000000000000000000000064a7b3b6e00d00000000000000000054ce09230100000000000000000000000000000000000000000000000000000054ce09230100000a000000000000000a00000000000000000000000000000000000000000000000000000000000000");
        assert(HexStr(encoded)==HexStr(expected));
        DataStream legacy{expected};IssuanceSnapshot decoded;legacy>>decoded;assert(decoded==snapshot&&legacy.empty());}
        {auto snapshot=Initial(definition);
        assert(snapshot.Valid());DataStream encoded;encoded<<snapshot;
        const auto expected=ParseHex("056e00000000000000000000000000000000000000000000000000000000000000000064a7b3b6e00d00000000000000000054ce0923010000f401000000000000000000000000000000000000000000000000000000000000000064a7b3b6e00d00000000000000000054ce09230100000000000000000000000000000000000000000000000000000054ce09230100000a000000000000000a00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000");
        assert(HexStr(encoded)==HexStr(expected));
        DataStream legacy{expected};IssuanceSnapshot decoded;legacy>>decoded;assert(decoded==snapshot&&legacy.empty());}
        {auto snapshot=Initial(definition);
        snapshot.minted=416666666667LL;snapshot.promised=833333333333LL;
        snapshot.partition->ordinary_minted=416666666667LL;snapshot.partition->ordinary_promised=833333333333LL;
        snapshot.pools->audit=833333333333LL;snapshot.audit_settlement->total_paid=416666666667LL;
        snapshot.audit_settlement->active=AuditSettlementActive{10,3,1,10,Id(110),Id(701),125*coin};
        assert(snapshot.Valid());DataStream encoded;encoded<<snapshot;
        const auto expected=ParseHex("056e00000000000000000000000000000000000000000000000000000000000000000064a7b3b6e00dabc6440361000000558d8906c2000000f401000000000000000000000000000000000000000000000000000000000000000064a7b3b6e00dabc6440361000000558d8906c2000000000000000000000000000000000000000000000000000000558d8906c20000000a000000000000000a00000000000000000000000000000000000000000000000000000000000000000000000000000000000000abc6440361000000010a00000000000000030000000000000001000000000000000a0000006e00000000000000000000000000000000000000000000000000000000000000bd020000000000000000000000000000000000000000000000000000000000000054ce0923010000");
        assert(HexStr(encoded)==HexStr(expected));
        DataStream legacy{expected};IssuanceSnapshot decoded;legacy>>decoded;assert(decoded==snapshot&&legacy.empty());}
    }
    auto before=Initial(definition);auto after=before;after.tip=Id(900);
    unsigned reads{};
    const auto missing_ledger=[&]()->const NativeAuditLedger*{++reads;return nullptr;};
    auto idle=before;
    const auto empty=ApplyAuditSettlementPageLazy(missing_ledger,definition,2,0,0,{},idle);
    assert(empty&&empty->count==0&&empty->paid==0&&empty->outputs.empty()&&!empty->receipt&&idle==before&&reads==0);
    auto corrupt_idle=before;--corrupt_idle.pools->audit;
    assert(!ApplyAuditSettlementPageLazy(missing_ledger,definition,2,0,0,{},corrupt_idle)&&reads==0);
    auto due_missing=before;
    assert(!ApplyAuditSettlementPageLazy(missing_ledger,definition,2,10,10,Id(110),due_missing)&&reads==1&&due_missing==before);
    auto lazy_after=after;
    const auto lazy=ApplyAuditSettlementPageLazy([&]{++reads;return &ledger;},definition,2,10,10,Id(110),lazy_after);
    assert(lazy&&reads==2);
    const auto page=ApplyAuditSettlementPage(ledger,definition,2,10,10,Id(110),after);assert(page&&page->count==2&&after.audit_settlement->active->tasks==3);
    assert(lazy_after==after&&lazy->outputs==page->outputs&&lazy->paid==page->paid&&lazy->receipt==page->receipt);
    assert(page->paid==833333333334LL&&after.pools->audit==416666666666LL);
    const auto tx=MakeTransactionRef(Coinbase(*page,11));assert(VerifyAuditSettlementOutputs(*page,*tx,{},0));
    auto bad=Coinbase(*page,11);bad.vout[0].scriptPubKey=authority.script;assert(!VerifyAuditSettlementOutputs(*page,CTransaction{bad},{},0));
    bad=Coinbase(*page,11);++bad.vout[0].nValue;assert(!VerifyAuditSettlementOutputs(*page,CTransaction{bad},{},0));
    bad=Coinbase(*page,11);bad.vout.push_back(bad.vout.back());assert(!VerifyAuditSettlementOutputs(*page,CTransaction{bad},{},0));
    auto wa=QaWallet(first),wb=QaWallet(second);const auto credit=wallet::TxGetCredit(*wa,*tx)+wallet::TxGetCredit(*wb,*tx);assert(credit==page->paid);
    const auto path=fs::PathFromString(argv[1]);
    {CCoinsViewDB db{{.path=path,.cache_bytes=1U<<20},{}};Initialize(db,before);Publish(db,before,after,*tx,11);assert(db.GetPgnIssuanceState()==after);
        for(unsigned i=0;i<page->outputs.size();++i)assert(db.GetCoin({tx->GetHash(),i})->out==page->outputs[i]);
        CCoinsViewCache child{&db};child.SetBestBlock(after.tip);child.StagePgnIssuanceHistoryStep({IssuanceDirection::Forward,{before,after}});
        assert(child.GetPgnIssuanceState()==after&&!child.GetStagedPgnIssuanceCommit()); // journal replay is deliberately idempotent
    }
    // A cold DB reopen must keep the exact next ordinal and already-created UTXO.
    {CCoinsViewDB db{{.path=path,.cache_bytes=1U<<20},{}};assert(db.GetPgnIssuanceState()==after);
        auto done=after;done.tip=Id(901);const auto page2=ApplyAuditSettlementPage(ledger,definition,2,10,10,Id(110),done);
        assert(page2&&page2->first==2&&page2->count==1&&page2->paid==416666666666LL&&!done.audit_settlement->active&&done.pools->audit==0&&done.audit_settlement->total_paid==125*coin);
        assert(!VerifyAuditSettlementOutputs(*page2,*tx,{},0));const auto tx2=MakeTransactionRef(Coinbase(*page2,12));Publish(db,after,done,*tx2,12);
        CCoinsViewCache undo{&db};for(unsigned i=0;i<page2->outputs.size();++i)assert(undo.SpendCoin({tx2->GetHash(),i}));undo.SetBestBlock(after.tip);
        const auto retained=db.GetPgnIssuanceForward(done.tip);assert(retained&&retained->before==after);
        undo.StagePgnIssuanceHistoryStep({IssuanceDirection::Rewind,*retained});undo.StagePgnFactHistoryStep({FactDirection::Rewind,*db.GetPgnFactForward(done.tip)});undo.Flush();
        assert(db.GetPgnIssuanceState()==after&&!db.HaveCoin({tx2->GetHash(),0}));
        auto replay=after;replay.tip=done.tip;const auto again=ApplyAuditSettlementPage(ledger,definition,2,10,10,Id(110),replay);assert(again&&again->outputs==page2->outputs&&replay==done);Publish(db,after,replay,*tx2,12);
        auto next=done;next.tip=Id(902);next.epochs->golds=20;next.epochs->last_gold_height=20;next.promised+=125*coin;next.partition->ordinary_promised+=125*coin;next.pools->audit+=125*coin;
        const auto late=ApplyAuditSettlementPage(ledger,definition,2,20,20,Id(120),next);assert(late&&late->count==1&&late->paid==125*coin&&late->outputs[0].scriptPubKey==second.script&&next.pools->audit==0);
    }
    { // Exact old financial amounts, one output per beneficiary for this period.
        auto grouped_before=before;grouped_before.audit_settlement->grouped=true;assert(grouped_before.Valid());
        const auto book=GroupAuditEntitlements(ledger,0,10,125*coin);assert(book.tasks==3&&book.recipients.size()==2);
        std::map<CScript,CAmount> old_due;
        auto legacy=before;
        const auto whole=ApplyAuditSettlementPage(ledger,definition,128,10,10,Id(110),legacy);assert(whole);
        for(const auto& output:whole->outputs)old_due[output.scriptPubKey]+=output.nValue;
        for(const auto& output:book.recipients)assert(old_due.at(output.scriptPubKey)==output.nValue);
        auto grouped=grouped_before;grouped.tip=Id(920);
        const auto first=ApplyAuditSettlementPage(ledger,definition,1,10,10,Id(110),grouped);
        assert(first&&first->outputs.size()==1&&first->count==1&&grouped.audit_settlement->active);
        assert(first->outputs[0]==book.recipients[0]);
        DataStream wire;wire<<grouped;IssuanceSnapshot cold;wire>>cold;assert(cold==grouped&&cold.audit_settlement->grouped);
        auto bad_remaining=grouped;--bad_remaining.audit_settlement->active->grouped_remaining;const auto bad_before=bad_remaining;
        assert(!ApplyAuditSettlementPage(ledger,definition,1,10,10,Id(110),bad_remaining)&&bad_remaining==bad_before);
        const auto grouped_path=fs::PathFromString(std::string(argv[1])+"-grouped");
        const auto first_tx=MakeTransactionRef(Coinbase(*first,21));
        assert(VerifyAuditSettlementOutputs(*first,*first_tx,{},0));
        {CCoinsViewDB db{{.path=grouped_path,.cache_bytes=1U<<20},{}};Initialize(db,grouped_before);Publish(db,grouped_before,grouped,*first_tx,21);}
        {CCoinsViewDB db{{.path=grouped_path,.cache_bytes=1U<<20},{}};assert(db.GetPgnIssuanceState()==grouped);
            auto done=grouped;done.tip=Id(921);const auto second=ApplyAuditSettlementPage(ledger,definition,1,10,10,Id(110),done);
            assert(second&&second->outputs.size()==1&&second->outputs[0]==book.recipients[1]);
            assert(done.audit_settlement->grouped&&!done.audit_settlement->active&&done.audit_settlement->total_paid==125*coin&&done.pools->audit==0);
            assert(first->paid+second->paid==125*coin);
            const auto second_tx=MakeTransactionRef(Coinbase(*second,22));Publish(db,grouped,done,*second_tx,22);
            CCoinsViewCache undo{&db};for(unsigned i=0;i<second->outputs.size();++i)assert(undo.SpendCoin({second_tx->GetHash(),i}));undo.SetBestBlock(grouped.tip);
            undo.StagePgnIssuanceHistoryStep({IssuanceDirection::Rewind,*db.GetPgnIssuanceForward(done.tip)});
            undo.StagePgnFactHistoryStep({FactDirection::Rewind,*db.GetPgnFactForward(done.tip)});undo.Flush();
            assert(db.GetPgnIssuanceState()==grouped&&!db.HaveCoin({second_tx->GetHash(),0}));
            auto replay=grouped;replay.tip=done.tip;const auto same=ApplyAuditSettlementPage(ledger,definition,1,10,10,Id(110),replay);
            assert(same&&same->outputs==second->outputs&&same->receipt==second->receipt&&replay==done);
            Publish(db,grouped,replay,*second_tx,22);
            const auto once=done;const auto no_repeat=ApplyAuditSettlementPage(ledger,definition,128,10,10,Id(110),done);
            assert(no_repeat&&no_repeat->outputs.empty()&&!no_repeat->paid&&done==once);
        }
        // Same display name has no role in aggregation: exact scripts remain distinct.
        assert(book.recipients[0].scriptPubKey!=book.recipients[1].scriptPubKey);
        auto missing=ledger;missing.begin()->second.jobs[0]->stage=NativeAuditJob::Committed;
        auto attempt=grouped;assert(!ApplyAuditSettlementPage(missing,definition,1,10,10,Id(110),attempt)&&attempt==grouped);
        assert(BuildCandidateAuditSettlementPolicy(128,true)!=BuildCandidateAuditSettlementPolicy(128,false));
        NativeAuditLedger bulk;
        for(unsigned i=0;i<25000;++i){auto report=ledger.begin()->second;
            report.jobs[0]->stage=NativeAuditJob::Passed;report.jobs[0]->passed_height=8;
            report.jobs[1]->stage=NativeAuditJob::Passed;report.jobs[1]->passed_height=8;
            bulk.emplace(Id(100000+i),std::move(report));
        }
        const auto large=GroupAuditEntitlements(bulk,0,10,125*coin+1);
        assert(large.tasks==50000&&large.recipients.size()==2);
        assert(large.recipients[0].nValue+large.recipients[1].nValue==125*coin+1);
        const auto tiny=GroupAuditEntitlements(bulk,0,10,1);
        assert(tiny.recipients.size()==2&&tiny.recipients[0].nValue+tiny.recipients[1].nValue==1);
        const auto maximum=GroupAuditEntitlements(bulk,0,10,PGN_MAX_SUPPLY_ATOMIC);
        assert(maximum.recipients[0].nValue+maximum.recipients[1].nValue==PGN_MAX_SUPPLY_ATOMIC);
        std::printf("{\"grouped_native\":true,\"original_tasks\":3,\"beneficiaries\":2,\"exact_rounding\":true,\"cold_DB\":true,\"reorg_replay\":true,\"duplicate_payment_rejected\":true}\n");
    }

    auto tampered=after;tampered.audit_settlement->active->root=Id(999);assert(!ApplyAuditSettlementPage(ledger,definition,2,10,10,Id(110),tampered));
    auto missing=before;missing.pools->audit-=1;missing.promised-=1;missing.partition->ordinary_promised-=1;assert(!ApplyAuditSettlementPage(ledger,definition,2,10,10,Id(110),missing));
    auto disabled=before;disabled.audit_settlement.reset();assert(!ApplyAuditSettlementPage(ledger,definition,2,10,10,Id(110),disabled));
    auto invalid_definition=definition;invalid_definition.audit_period=0;auto untouched=before;
    assert(!ApplyAuditSettlementPage(ledger,invalid_definition,2,10,10,Id(110),untouched)&&untouched==before);
    DataStream current;current<<after;IssuanceSnapshot restored;current>>restored;assert(restored==after);
    DataStream legacy;legacy<<disabled;legacy>>restored;assert(restored==disabled&&!restored.audit_settlement);
    assert(!BuildCandidateAuditSettlementPolicy(0)&&!BuildCandidateAuditSettlementPolicy(129)&&BuildCandidateAuditSettlementPolicy(2));
    std::printf("{\"PASS\":true,\"evidence_layer\":\"native signed-receipt + LevelDB UTXO/issuance + SQLite wallet-credit seam\",\"tasks_at_cutoff\":3,\"late_tasks\":1,\"pages\":2,\"paid_atomic\":\"1250000000000\",\"wallet_credit_first_page\":\"%lld\",\"cold_restart\":true,\"duplicate_rejected\":true,\"reorg_restored\":true,\"reapply_exact\":true,\"full_ConnectBlock_qualification\":false,\"independent_math_qualification\":false}\n",static_cast<long long>(credit));
}
