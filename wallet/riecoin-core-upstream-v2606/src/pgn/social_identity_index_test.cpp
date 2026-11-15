// Candidate-index test: authenticated Native positions, unverified certificate
// bytes. No name or permission may be inferred from this index.
#define main ExistingNativeLeaderboardReferenceMain
#include <pgn/native_leaderboard_test.cpp>
#undef main
#include <pgn/social_identity_index.h>
#include <random.h>
namespace committed=pgn::social::identity::committed;
static std::string CandidateBytes(const uint256& chain,const std::string& social){
 UniValue statement{UniValue::VOBJ};statement.pushKV("v",2);statement.pushKV("genesis",chain.GetHex());statement.pushKV("scope",pgn::social::identity::Scope);statement.pushKV("social_script",social);
 UniValue value{UniValue::VOBJ};value.pushKV("statement",statement);return value.write();
}
int main(int argc,char** argv){try{
 need(argc==2,"Private candidate CDB path required");LOCK(cs_main);const auto path=fs::PathFromString(argv[1]);need(!fs::exists(path),"New candidate fixture path required");Fixture f(path/"native");f.Append({CTxOut{0,CScript{}<<OP_RETURN}});
 const std::string a="0014"+std::string(40,'a'),b="0014"+std::string(40,'b');
 const auto frame_a=committed::Envelope(CandidateBytes(f.chain,a)),frame_b=committed::Envelope(CandidateBytes(f.chain,b));
 auto canonical=[&](const uint256& hash,int32_t height){const auto* block=f.tip->GetAncestor(height);return block&&block->GetBlockHash()==hash;};
 auto store=std::make_unique<committed::CandidateIndex>(path/"index",f.chain);
 auto append=[&]{const auto block=f.archive.Load(*f.view,*f.tip,f.tip->nHeight,f.schedule.archive);need(bool(block.block),"Verified Native archive block");return store->Append(*block.block,canonical);};
 check(append().state=="indexed","Genesis cursor committed atomically");
 f.Append({CTxOut{0,frame_a},CTxOut{0,frame_a},CTxOut{0,frame_a},CTxOut{0,frame_b}});
 check(append().candidates==4,"Unverified candidates retain exact Native positions without granting names");
 const committed::CandidateAnchor anchor{f.tip->nHeight,f.tip->GetBlockHash()};
 const auto first=store->Read(a,anchor,"",2,canonical);const auto second=store->Read(a,anchor,first.next,2,canonical);
 check(first.refs.size()==2&&!first.complete&&first.next.size()==20&&second.refs.size()==1&&second.complete,"Bounded per-S pagination is complete without duplicate positions");
 for(unsigned height=0;height<100;++height){std::vector<CTxOut> pressure;for(unsigned i=0;i<10;++i)pressure.emplace_back(0,frame_b);f.Append(std::move(pressure));need(append().state=="indexed","Targeted pressure block indexed");}
 const committed::CandidateAnchor current{f.tip->nHeight,f.tip->GetBlockHash()};
 const auto continued=store->Read(a,anchor,first.next,2,canonical);
 check(continued.refs.size()==1&&continued.refs[0].block==second.refs[0].block,"Extension preserves older canonical anchor and continuation");
 const auto independent=store->Read(a,current,"",100,canonical),pressured=store->Read(b,current,"",100,canonical);
 std::cout<<"OBSERVED A "<<independent.refs.size()<<" complete "<<independent.complete<<" B "<<pressured.refs.size()<<" complete "<<pressured.complete<<"\n";
 check(independent.refs.size()==3&&independent.complete&&pressured.refs.size()==100&&!pressured.complete,"1000 candidates targeting B neither hold nor truncate A");
 bool oversized=false;try{store->Read(a,current,"",101,canonical);}catch(const std::exception&){oversized=true;}check(oversized,"Page bound100 enforced");
 store.reset();store=std::make_unique<committed::CandidateIndex>(path/"index",f.chain);
 check(store->Tip().hash==current.hash&&store->Read(a,current,"",100,canonical).refs.size()==3,"CDB restart preserves cursor and per-S refs");
 f.Append({CTxOut{0,CScript{}<<OP_RETURN}},f.indices.front().get());
 bool old_anchor=false;try{store->Read(a,current,"",100,canonical);}catch(const std::exception&){old_anchor=true;}check(old_anchor,"Reorganized page anchor refused");
 bool stale_index=false;try{append();}catch(const std::exception&){stale_index=true;}check(stale_index,"Reorg cannot be mislabeled already-indexed");
 while(store->Tip().height>0)need(store->RollbackOne().state=="rolled_back","Native candidate undo");
 check(append().state=="indexed","New canonical fork appends after CDB rollback");
 const committed::CandidateAnchor fork{f.tip->nHeight,f.tip->GetBlockHash()};const auto clean=store->Read(a,fork,"",100,canonical);
 check(clean.refs.empty()&&clean.complete,"Orphaned references do not reappear on replacement branch");
 store.reset();store=std::make_unique<committed::CandidateIndex>(path/"index",f.chain);
 check(store->Read(a,fork,"",100,canonical).refs.empty(),"Restart after reorg preserves rollback");
 std::cout<<"CHECKS "<<checks<<"\nLIMIT Candidate refs only, no signature/name/role admission, no RPC/P2P/GC or scale claim.\n";return 0;
}catch(const std::exception& error){std::cerr<<"FAIL "<<error.what()<<'\n';return 1;}}
