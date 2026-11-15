#include "social/social_leaderboard.h"
#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonArray>
#include <iostream>
#include <stdexcept>
using namespace pgn::social;
int main(int argc,char**argv){
 QCoreApplication app(argc,argv);int checks=0;const auto check=[&](bool pass,const char* reason){if(!pass)throw std::runtime_error(reason);++checks;};
 const Identity identity{QString(64,'a'),"0014"+QString(40,'b')};ReadRequest request{Section::Miners,identity,Window::AllTime,{},100};
 const auto tier=[](QString n){return QJsonObject{{"count",n},{"complete",true}};};
 const QJsonObject goodCounts{{"q",tier("0")},{"g",tier("83")},{"s",tier("165")},{"b",tier("540")},{"r",tier("1597")}};
 QJsonObject row{{"operator_id",QString(64,'d')},{"display_name","Recorded QA label"},{"tickets_reported","3"},{"tickets_validated","2"},{"audits_finalized_valid","4"},{"validated_search_positions","100000"},{"last_activity_height",1},{"signing_scripts",QJsonArray{"0014"+QString(40,'e')}},{"signing_scripts_total",1},{"signing_scripts_more",false},{"rosettes",goodCounts}};
 QJsonObject base{{"schema","PGN_LEADERBOARD_V1"},{"genesis",identity.genesis},{"bestblockhash",QString(64,'c')},{"height",1},{"sort","validated_search_positions_desc_then_operator_id_asc"},{"offset",0},{"next_offset",1},{"total",1},{"more",false},{"rows",QJsonArray{row}},{"rosette_counting_unit","unique_disclosed_gap_records_per_operator"},{"rosette_history_scope","qualified_report_disclosures"}};
 const auto parse=[&](QJsonObject response,std::optional<ReadRequest> optional={}){const auto scope=optional.value_or(request);Snapshot result;check(decodeLeaderboard(scope,response,result).isEmpty(),"Existing Native page grammar");return result;};
 auto value=parse(base);const QStringList expected{"0","83","165","540","1597"};
 for(int i=0;i<5;++i){check(value.miners[0].rosettes[i].value==expected[i],"Exact recorded decimal count");check(value.miners[0].rosettes[i].provenance.source.contains("not coin payments"),"Scope in provenance");}
 check(validate(value,request).isEmpty(),"Same-genesis provenance validates");
 auto legacy=base;auto old=row;old.remove("rosettes");legacy["rows"]=QJsonArray{old};auto unavailable=parse(legacy);
 for(int i=0;i<5;++i)check(!unavailable.miners[0].rosettes[i].value,"Legacy Core does not infer zero");
 check(unavailable.miners[0].ticketsReported.value==QString("3"),"Missing rosettes preserve existing metrics");
 auto partial=base;auto counts=goodCounts;counts["g"]=QJsonObject{{"count","999"},{"complete",false},{"reason","Not disclosed"}};auto modified=row;modified["rosettes"]=counts;partial["rows"]=QJsonArray{modified};value=parse(partial);
 check(!value.miners[0].rosettes[1].value&&value.miners[0].rosetteReasons[1]=="Not disclosed","Incomplete count ignored with reason");check(value.miners[0].rosettes[2].value==QString("165"),"One unavailable tier does not suppress another");
 for(const QJsonValue& bad:QList<QJsonValue>{83,-1,QString("083"),QString("-1"),QString(257,'9'),QJsonValue{}}){
  modified=row;counts=goodCounts;counts["g"]=QJsonObject{{"count",bad},{"complete",true}};modified["rosettes"]=counts;auto response=base;response["rows"]=QJsonArray{modified};value=parse(response);
  check(!value.miners[0].rosettes[1].value,"Malformed non-canonical count stays unavailable");check(value.miners[0].rosettes[0].value==QString("0"),"Explicit complete zero independent");
 }
 auto wrong=base;wrong["rosette_history_scope"]="all_discoveries";value=parse(wrong);for(const auto& c:value.miners[0].rosettes)check(!c.value,"Unsupported scope cannot grant exact count");
 auto overlap=base;modified=row;counts=goodCounts;counts["q"]=tier("1");counts["g"]=tier("3");modified["rosettes"]=counts;overlap["rows"]=QJsonArray{modified};value=parse(overlap);check(value.miners[0].rosettes[0].value==QString("1")&&value.miners[0].rosettes[1].value==QString("3"),"Q independent of GSBR, no arithmetic invented");
 const auto start=QDateTime(QDateTime::currentDateTimeUtc().date(),QTime(0,0),Qt::UTC).toSecsSinceEpoch();
 auto today=base;today["period"]="today_utc";today["utc_start"]=start;today["utc_end"]=start+86400;today["history_complete"]=true;today["event_time_source"]="canonical_block_header_time";today["validation_date_scope"]="latest_current_qualification_episode";
 auto day=request;day.window=Window::Day;value=parse(today,day);check(value.window==Window::Day&&value.leaderboardPeriod->utcStart==start,"Recorded counts inherit fixed Today UTC interval");check(value.miners[0].rosettes[1].value==QString("83"),"No Total/daily recomputation in frontend");
 auto forged=base;forged["genesis"]=QString(64,'f');Snapshot rejected;check(!decodeLeaderboard(request,forged,rejected).isEmpty(),"Foreign genesis rejected");
 std::cout<<"PASS "<<checks<<" rosette decode checks; synthetic fixture only\n";return 0;
}

