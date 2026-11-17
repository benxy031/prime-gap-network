#include "leaderboard_rosettes.h"
#include <iostream>
#include <fstream>
#include <stdexcept>

static unsigned checks{};
static void need(bool ok,const char* text){++checks;if(!ok)throw std::runtime_error(text);}
static std::string body(std::string prime,int64_t gap,std::string n="275",std::string d="10"){
    using namespace pgn::core;
    UniValue out{UniValue::VOBJ},computed{UniValue::VOBJ},best{UniValue::VARR},item{UniValue::VOBJ};
    item.pushKV("lower_prime",prime);item.pushKV("upper_prime",(Integer{prime}+gap).convert_to<std::string>());item.pushKV("gap",gap);best.push_back(item);
    computed.pushKV("best",best);out.pushKV("schema","PGN_NETWORK_RANGE_REPORT_V1");out.pushKV("computed",computed);
    if(!n.empty()){out.pushKV("target_numerator",n);out.pushKV("target_denominator",d);}return out.write();
}
int main(int argc,char** argv){try{
    using namespace pgn::core;need(argc==2||argc==3,"oracle path");
    // A separate Python Decimal(100 digits) oracle supplies boundary neighbours.
    std::ifstream f(argv[1]);std::string p,n,d;int64_t gap;int tier,q;unsigned rows{};
    const std::string root(64,'a');const auto hash=[&](std::string_view){return root;};
    while(f>>p>>gap>>n>>d>>tier>>q){
        LeaderboardRosettes value;value.Observe(body(p,gap,n,d),root,true,hash);++rows;
        for(size_t i=0;i<5;++i)need(value.complete[i],"exact boundary classification");
        need(value.counts[0]==q,"independent Q oracle");
        for(size_t i=1;i<5;++i)need(value.counts[i]==(int(i)==tier?1:0),"exclusive tier oracle");
    }
    need(rows>=100,"independent coverage");
    if(argc==3){
        std::ifstream batches(argv[2]);int group;int active=-1;UniValue records{UniValue::VARR};
        std::array<uint64_t,5> expected{};std::string target_n,target_d;
        const auto finish=[&](){
            if(active<0)return;
            UniValue report{UniValue::VOBJ},computed{UniValue::VOBJ};computed.pushKV("best",records);
            report.pushKV("schema","PGN_NETWORK_RANGE_REPORT_V1");report.pushKV("computed",computed);
            report.pushKV("target_numerator",target_n);report.pushKV("target_denominator",target_d);
            LeaderboardRosettes actual;actual.Observe(report.write(),root,true,hash);
            for(size_t i=0;i<5;++i)need(actual.complete[i]&&actual.counts[i]==expected[i],"multi-record monotone enclosure/fallback oracle");
        };
        while(batches>>group>>p>>gap>>n>>d>>tier>>q){
            if(active!=group){finish();active=group;records=UniValue{UniValue::VARR};expected={};target_n=n;target_d=d;}
            need(n==target_n&&d==target_d,"one target per batch");
            UniValue one{UniValue::VOBJ};one.pushKV("lower_prime",p);one.pushKV("upper_prime",(Integer{p}+gap).convert_to<std::string>());one.pushKV("gap",gap);records.push_back(one);
            expected[0]+=q;if(tier)++expected[size_t(tier)];
        }
        finish();need(active>=3,"narrow, broad and exact boundary batches");
    }
    LeaderboardRosettes replay;const auto example=body("150000000000000000001",1300);
    replay.Observe(example,root,true,hash);const auto initial=replay.Json().write();replay.Observe(example,root,true,hash);
    need(replay.Json().write()==initial,"replayed report not counted twice");
    LeaderboardRosettes today;today.Observe(example,root,false,hash);today.Observe(example,root,true,hash);
    for(const auto& count:today.counts)need(count==0,"earlier record not recounted today");
    LeaderboardRosettes missing;missing.Observe(body("150000000000000000001",1900,""),root,true,hash);
    need(missing.complete[0]&&missing.counts[0]==1,"Q does not require moving target");
    need(!missing.complete[1]&&!missing.Json()["g"].exists("count"),"missing target never fake zero");
    LeaderboardRosettes bad;bad.Observe(example,std::string(64,'b'),true,hash);
    need(!bad.complete[0]&&!bad.Json()["q"].exists("count"),"changed root rejected");
    need(!bad.largest_gap_complete&&!bad.LargestGapJson().exists("gap"),"bad commitment cannot invent largest gap");
    need(missing.largest_gap_complete&&missing.largest_gap==1900,"largest gap does not depend on target classification");
    need(replay.largest_gap_complete&&replay.largest_gap==1300,"largest gap exact after replay");
    need(today.largest_gap_complete&&today.largest_gap==0,"earlier disclosure is not recounted in a new calendar window");
    LeaderboardRosettes separate;
    separate.Observe(example,root,true,hash,false);
    need(separate.largest_gap_complete&&separate.largest_gap==0,"qualification date does not replace first disclosure date");
    separate.Observe(body("150000000000000010001",1400),root,false,hash,true);
    need(separate.largest_gap_complete&&separate.largest_gap==1400,"disclosure-window largest gap independent of qualification-window rosettes");
    LeaderboardRosettes unrelated;unrelated.Observe(example,std::string(64,'b'),true,hash,false);
    need(!unrelated.complete[0]&&unrelated.largest_gap_complete,"unavailable disclosure outside gap interval does not poison that interval");
    LeaderboardRosettes empty;for(size_t i=0;i<5;++i)need(empty.complete[i]&&empty.counts[i]==0,"known empty remains zero");
    need(empty.Json()["g"]["count"].get_str()=="0","RPC decimal string");
    need(sizeof(LeaderboardRosetteTotals)==48,"fifty thousand retained row summaries occupy 2.4MB");
    LeaderboardRosettes overflow;overflow.counts[1]=std::numeric_limits<uint64_t>::max();overflow.Add(1);
    need(!overflow.complete[1]&&!overflow.Json()["g"].exists("count"),"overflow cannot wrap to zero");
    std::cout<<"PASS "<<checks<<" checks; "<<rows<<" independent Decimal boundary cases\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
