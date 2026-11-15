#include "ui/mining_ticket_progress.h"
#include "ui/mining_cycle_progress.h"
#include <QApplication>
#include <QImage>
#include <QJsonArray>
#include <QDir>
#include <iostream>
#include <stdexcept>
using namespace pgn::ui;
static int checks=0;
void check(bool value,const char* text){if(!value)throw std::runtime_error(text);++checks;}
QJsonObject preparation(int count,int active){
 return {{"schema","PGN_PRE_TICKET_AUDIT_VIEW_V1"},{"active_audit_id",QString(64,'a')},
         {"search_assigned",false},{"confirmed_service_answers",count<0?QJsonValue(QJsonValue::Null):QJsonValue(count)},
         {"active_index",active<0?QJsonValue(QJsonValue::Null):QJsonValue(active)}};
}
QJsonObject frame(QJsonObject view){
 return {{"ticket_id",QString(64,'a')},{"title","Audit 1"},{"bits",68},{"phase","Auditing"},
         {"work_role","audit"},{"positions_total","10000000000000"},{"positions","5000000000000"},
         {"eta_rate",1e12},{"audit_service",QJsonObject{{"mode","two_regular_one_catchup"},{"active_role","audit"},{"preparation",view}}}};
}
int main(int argc,char** argv){
 QApplication app(argc,argv);
 try{
  auto first=frame(preparation(0,0));auto p=miningCycleProgress(first);
  check(p.known&&p.preparing,"First service audit has no preparation layout");
  check(p.slices.size()==3&&p.slices[0].fraction==.5&&p.slices[1].fraction==0.,"Uncompleted service audit was fabricated");
  check(p.slices[2].pendingAssignment&&p.slices[2].fraction==0.,"Unassigned search appears complete");
  check(!p.cycleEta&&p.taskEta&&*p.taskEta==5.,"Preparation invented a cycle ETA");
  auto second=frame(preparation(1,1));p=miningCycleProgress(second);
  check(p.slices[0].fraction==1.&&p.slices[1].fraction==.5,"Core-confirmed first answer lost");
  p=miningCycleProgress(frame(preparation(-1,-1)));
  check(p.known&&p.preparing&&p.slices[0].pendingAssignment&&p.slices[1].pendingAssignment,"Missing Core observation became completed work");
  auto wrong=preparation(0,0);wrong["active_audit_id"]=QString(64,'b');
  check(!miningCycleProgress(frame(wrong)).known,"Preparation attached to another active audit");
  wrong=preparation(0,1);check(!miningCycleProgress(frame(wrong)).known,"Impossible service order was accepted");
  wrong=preparation(1,1);wrong["confirmed_service_answers"]=true;
  check(!miningCycleProgress(frame(wrong)).known,"Boolean service count accepted");
  auto search=second;search["work_role"]="search";search["phase"]="Searching";
  auto service=search["audit_service"].toObject();
  service["cycle"]=QJsonObject{{"schema","PGN_MINING_CYCLE_VIEW_V1"},{"request_id",QString(64,'c')},
   {"ticket_number","42"},{"active_index",2},{"search_positions","10000000000000"},
   {"audits",QJsonArray{QJsonObject{{"state","committed"}},QJsonObject{{"state","committed"}}}}};
  search["audit_service"]=service;p=miningCycleProgress(search);
  check(p.known&&!p.preparing&&p.title=="Ticket 42","Assigned ticket retained stale preparation identity");
  MiningTicketProgress rail;rail.resize(1366,120);
  const auto render=[&](const QJsonObject& input,const QString& name){
   rail.present(input);app.processEvents();QImage image(rail.size(),QImage::Format_ARGB32_Premultiplied);
   image.fill(Qt::transparent);rail.render(&image);check(image.save(name),"Widget capture failed");
  };
  QDir().mkpath("captures");
  render(first,"captures/preparation-first.png");
  check(rail.property("pgn_ticket_progress_scope").toString()=="pre_ticket_audit","Actual widget did not select preparation state");
  check(!rail.property("pgn_ticket_cycle_fraction").isValid(),"Preparation published an overall ticket fraction");
  render(second,"captures/preparation-second.png");
  render(search,"captures/assigned-search.png");
  check(rail.property("pgn_ticket_progress_scope").toString()=="ticket_cycle","Actual widget did not switch to assigned cycle");
  std::cout<<"PASS "<<checks<<" checks; real MiningTicketProgress rendered; no network or GPU job\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
