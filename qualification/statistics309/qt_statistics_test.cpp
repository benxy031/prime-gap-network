#include "social/social_leaderboard.h"
#include "ui/social_view.h"
#include <pgn/leaderboard_utc_period.h>
#include <QApplication>
#include <QComboBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QTableWidget>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using namespace pgn;
static int checks{};
static void require(bool value,const char* why){++checks;if(!value)throw std::runtime_error(why);}
static QJsonObject page(const social::ReadRequest& request,bool modern=true,bool expired=false){
 const auto tip=QString(64,'b');QJsonArray rows;
 for(int i=0;i<3;++i){QJsonObject counts;for(const auto* tier:{"q","g","s","b","r"})counts[tier]=QJsonObject{{"complete",true},{"count",QString::number(i+1)}};
  rows.append(QJsonObject{{"operator_id",QString(64,QChar('c'+i))},{"display_name",QStringList{"Wizz fixture","Robert fixture","Guest fixture"}[i]},
   {"tickets_reported","2"},{"tickets_validated","1"},{"audits_finalized_valid","2"},{"validated_search_positions",QString::number((i+1)*200000000000000LL)},
   {"last_activity_height",300},{"rosettes",counts},{"largest_recorded_gap",i==2?QJsonObject{{"complete",true},{"reason","No disclosed gap in this UTC interval"}}:QJsonObject{{"complete",true},{"gap",i?"1300":"1600"}}}});
 }
 QJsonObject value{{"schema","PGN_LEADERBOARD_V1"},{"sort","validated_search_positions_desc_then_operator_id_asc"},{"genesis",request.identity.genesis},
  {"bestblockhash",tip},{"height",300},{"offset",0},{"next_offset",3},{"total",3},{"more",false},{"rows",rows},
  {"rosette_counting_unit","unique_disclosed_gap_records_per_operator"},{"rosette_history_scope","qualified_report_disclosures"}};
 if(modern){value["supported_periods"]=QJsonArray{"total","today_utc","week_utc","month_utc","hour_utc"};value["largest_gap_scope"]="unique_disclosed_gaps_per_operator_from_current_qualified_reports";value["largest_gap_time_source"]="first_qualified_disclosure_canonical_header_time";}
 if(request.window!=social::Window::AllTime){
  const auto native=core::ParseLeaderboardPeriod(social::leaderboardPeriodName(request.window).toStdString());require(bool(native),"Unknown requested period");
  const auto interval=core::LeaderboardUTCWindow::At(*native,QDateTime::currentSecsSinceEpoch()-(expired?3600:0));
  value["period"]=social::leaderboardPeriodName(request.window);value["utc_start"]=qint64(*interval.utc_start);value["utc_end"]=qint64(interval.End());
  value["history_complete"]=true;value["event_time_source"]="canonical_block_header_time";value["validation_date_scope"]="latest_current_qualification_episode";
 }
 return value;
}
int main(int argc,char** argv){qputenv("QT_QPA_PLATFORM","offscreen");QApplication app(argc,argv);try{
 const social::Identity identity{QString(64,'a'),"0014"+QString(40,'1')};social::ReadRequest request{social::Section::Miners,identity,social::Window::AllTime,{},100,{}};social::Snapshot data;
 require(social::decodeLeaderboard(request,page(request),data).isEmpty(),"Modern recorded gap refused");require(social::validate(data,request).isEmpty(),"Decoded snapshot provenance refused");
 require(data.largestGapSupported&&data.miners[0].largestRecordedGap.value==QString("1600")&&!data.miners[2].largestRecordedGap.value,"Exact gap or known absence lost");require(data.leaderboardWindows.size()==5,"Calendar capability lost");
 for(const auto& bad:QJsonArray{0,1300,"0","1301","4294967298","-2","999999999999999999999999999"}){
  auto value=page(request);auto rows=value["rows"].toArray();auto row=rows[0].toObject();row["largest_recorded_gap"]=QJsonObject{{"complete",true},{"gap",bad}};rows[0]=row;value["rows"]=rows;require(!social::decodeLeaderboard(request,value,data).isEmpty(),"Malformed gap was promoted to exact data");
 }
 require(social::decodeLeaderboard(request,page(request,false),data).isEmpty()&&!data.largestGapSupported&&data.leaderboardWindows.size()==2,"Old Core lost Total/Today compatibility");
 for(const auto window:{social::Window::Day,social::Window::Week,social::Window::Month,social::Window::Hour}){
  request.window=window;auto value=page(request);require(social::decodeLeaderboard(request,value,data).isEmpty(),"Valid calendar page denied");QJsonArray args;require(social::leaderboardArguments(request,args)&&social::validLeaderboardRpcArguments(args),"Named calendar grammar denied");
  value["utc_end"]=value["utc_end"].toInteger()+1;require(!social::decodeLeaderboard(request,value,data).isEmpty(),"Altered calendar endpoint accepted");
  value=page(request);request.cursor=QString(64,'b')+":0:"+QString::number(value["utc_start"].toInteger());require(social::leaderboardArguments(request,args)&&args.size()==5&&args[4].toInteger()==value["utc_start"].toInteger(),"Fixed calendar continuation lost");request.cursor.clear();
 }
 request.window=social::Window::Week;require(!social::decodeLeaderboard(request,page(request,false),data).isEmpty(),"Unadvertised period silently enabled");
 bool modern=false,expired=false;int reads=0;social::Window last{};social::Services services;
 services.read=[&](social::ReadRequest r,auto done){if(r.section!=social::Section::Miners){social::Snapshot s;s.identity=r.identity;s.section=r.section;s.authenticated=true;s.source="SYNTHETIC UI FIXTURE";s.observedUtc=QDateTime::currentDateTimeUtc();done(s,{});return;}++reads;last=r.window;social::Snapshot s;const auto error=social::decodeLeaderboard(r,page(r,modern,expired),s);done(s,error);};
 QTemporaryDir dir;ui::SocialView view(dir.path()+"/drafts.json",services);view.resize(1360,800);view.show();view.setIdentity(identity);view.selectSection(social::Section::Miners);app.processEvents();
 auto* table=view.findChild<QTableWidget*>("pgn-social-miners");auto* week=view.findChild<QPushButton*>("pgn-social-period-week");auto* hour=view.findChild<QPushButton*>("pgn-social-period-hour");require(table&&week&&hour,"Actual view controls missing");require(table->isColumnHidden(11)&&week->isHidden()&&hour->isHidden(),"Old Core exposes unsupported UI");
 modern=true;view.refresh();app.processEvents();require(!table->isColumnHidden(11)&&week->isVisible()&&hour->isVisible(),"Advertised controls hidden");require(table->horizontalHeader()->visualIndex(11)==7,"Gap not beside positions");
 require(table->horizontalScrollBar()->maximum()==0,"1360px layout requires horizontal scroll");QMetaObject::invokeMethod(table->horizontalHeader(),"sectionClicked",Qt::DirectConnection,Q_ARG(int,11));app.processEvents();require(table->item(0,0)->data(Qt::UserRole)==QString(64,'c'),"Largest-gap sorting uses the wrong column");
 week->click();app.processEvents();require(last==social::Window::Week,"Week button did not request calendar week");hour->click();app.processEvents();require(last==social::Window::Hour&&view.findChild<QLabel*>("pgn-social-miner-interval")->text().contains(":"),"Hour caption omits precise UTC endpoints");
 expired=true;view.refresh();const auto before=reads;view.refreshExpiredLeaderboardWindow();view.refreshExpiredLeaderboardWindow();require(reads==before+1,"UTC rollover failed or caused repeated RPC polling");view.hide();view.refreshExpiredLeaderboardWindow();require(reads==before+1,"Hidden page performs rollover RPC");
 view.show();modern=false;view.setIdentity({QString(64,'f'),identity.subject});app.processEvents();require(last==social::Window::AllTime&&week->isHidden()&&table->isColumnHidden(11),"Capabilities leaked across wallet binding");
 std::cout<<"PASS "<<checks<<" checks: exact gap/provenance, four UTC calendar periods, fixed continuation, old-Core controls, real view sorting/layout, one rollover RPC and binding reset; synthetic UI fixture only\n";return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
