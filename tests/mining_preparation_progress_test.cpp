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
 QPalette theme=app.palette();theme.setColor(QPalette::Window,QColor("#03151b"));theme.setColor(QPalette::WindowText,QColor("#c8f7f0"));app.setPalette(theme);
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
   image.fill(QColor("#03151b"));rail.render(&image);check(image.save(name),"Widget capture failed");
  };
  QDir().mkpath("captures");
  render(first,"captures/preparation-first.png");
  check(rail.property("pgn_ticket_progress_scope").toString()=="pre_ticket_audit","Actual widget did not select preparation state");
  check(!rail.property("pgn_ticket_cycle_fraction").isValid(),"Preparation published an overall ticket fraction");

  for(const auto& state:QStringList{"Paused","Stopping","Completing audits","Report ready","Waiting for connection","Waiting for next ticket"}){
   auto controlled=first;controlled["phase"]=state;rail.present(controlled);app.processEvents();
   QImage picture(rail.size(),QImage::Format_ARGB32_Premultiplied);picture.fill(QColor("#03151b"));rail.render(&picture);
   const auto expected=state=="Paused"?QString("Audit paused"):state=="Stopping"?QString("Stopping"):state=="Report ready"?QString("Audit computed"):state;
   check(rail.property("pgn_ticket_display_phase").toString()==expected,"Cycle caption concealed the real stop/wait/publication state");
  }
  render(second,"captures/preparation-second.png");
  render(search,"captures/assigned-search.png");
  check(rail.property("pgn_ticket_progress_scope").toString()=="ticket_cycle","Actual widget did not switch to assigned cycle");
  auto paused=search;paused["phase"]="Paused";rail.present(paused);app.processEvents();
  QImage pausedImage(rail.size(),QImage::Format_ARGB32_Premultiplied);pausedImage.fill(QColor("#03151b"));rail.render(&pausedImage);
  check(rail.property("pgn_ticket_display_phase").toString()=="Paused","Assigned search concealed its paused state");
  check(!rail.property("pgn_ticket_cycle_eta_seconds").isValid(),"Paused work advertised an active cycle ETA");

  auto retained=search;retained["phase"]="Synchronizing";retained["positions"]="1224065679360";retained["source_age_ns"]="129666390000000";const auto saved=retained;
  const auto waiting=currentAssignmentPresentation(retained);
  check(retained==saved,"Presentation rewrite mutated retained computation evidence");
  check(!waiting.contains("ticket_id")&&!waiting.contains("positions")&&!waiting.contains("audit_service"),"Unverified prior assignment leaked into current synchronization presentation");
  render(waiting,"captures/waiting-assignment.png");
  check(rail.property("pgn_ticket_display_phase").toString()=="Synchronizing"&&!rail.property("pgn_ticket_progress_fraction").isValid(),"Actual widget displayed prior ticket progress before current assignment verification");
  check(currentAssignmentPresentation(search)==search&&currentAssignmentPresentation(paused)==paused,"Valid searching or paused checkpoint changed");
  render(search,"captures/assignment-restored.png");check(rail.property("pgn_ticket_progress_fraction").isValid(),"Fresh verified assignment could not restore progress");
  std::cout<<"PASS "<<checks<<" checks; real MiningTicketProgress rendered; no network or GPU job\n";
  return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
