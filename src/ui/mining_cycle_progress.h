#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <algorithm>
#include <cmath>
#include <optional>
namespace pgn::ui {
// The worker can still attest the active audit and its completed prefix
// while the next partition has no sample. That view has no source age or rate.
inline bool retainedAuditPartitionBoundary(const QJsonObject& frame){
 const auto phase=frame.value("phase");
 if(frame.value("schema")!="PGN_HORIZON_MINING_VIEW_V1"||frame.value("work_role")!="audit"||
    frame.value("progress_scope")!="audit"||(phase!="Searching"&&phase!="Paused")||
    !frame.value("source_age_ns").isNull())return false;
 const auto validId=[](const QString& id){
  return id.size()==64&&std::all_of(id.begin(),id.end(),[](QChar value){
   return (value>=u'0'&&value<=u'9')||(value>=u'a'&&value<=u'f');
  });
 };
 const auto unsignedDecimal=[](const QString& number){
  return !number.isEmpty()&&number.size()<=64&&(number.size()==1||number.front()!=u'0')&&
   std::all_of(number.begin(),number.end(),[](QChar value){return value>=u'0'&&value<=u'9';});
 };
 const auto less=[](const QString& left,const QString& right){
  return left.size()==right.size()?left<right:left.size()<right.size();
 };
 const auto range=frame.value("audit_active_range").toObject();
 const auto start=range.value("start").toString(),end=range.value("end").toString();
 const auto done=frame.value("positions").toString(),total=frame.value("positions_total").toString();
 return validId(frame.value("ticket_id").toString())&&validId(frame.value("request_id").toString())&&
        unsignedDecimal(start)&&unsignedDecimal(end)&&less(start,end)&&
        unsignedDecimal(done)&&unsignedDecimal(total)&&total!="0"&&!less(total,done);
}

// Disconnection and synchronization still remove expired current assignments.
inline QJsonObject currentAssignmentPresentation(const QJsonObject& frame){
 if(frame.value("phase")!="Synchronizing"&&!frame.value("source_stale").toBool())return frame;
 if(retainedAuditPartitionBoundary(frame)){
  auto retained=frame;
  for(const auto* key:{"speed_rate","average_rate","compute_tests_rate","eta_rate"})
   retained.insert(key,QJsonValue::Null);
  return retained;
 }
 QJsonObject waiting;
 for(const auto* key:{"schema","phase","message","source_device","stream_id","local_sequence","observed_utc"})
  if(frame.contains(key))waiting.insert(key,frame.value(key));
 waiting.insert("title","Waiting for current assignment");waiting.insert("source_stale",true);
 return waiting;
}
struct CycleSlice { double weight{},fraction{};QString role;bool skipped{},pendingAssignment{}; };
struct CycleProgress {
 bool known{},preparing{};double fraction{};QString title,phase;QVector<CycleSlice> slices;
 std::optional<double> taskEta,cycleEta;
};
inline bool coherentMiningCycleFrame(const QJsonObject& frame){
 const auto cycle=frame.value("audit_service").toObject().value("cycle").toObject();
 const auto active=cycle.value("active_audit_id").toString();
 return frame.value("work_role")!="audit"||active.isEmpty()||active==frame.value("ticket_id").toString();
}
// These are observed physical audits, independent of server service credits.
class MiningCycleHistory {
 QString request_;QJsonArray audits_;QJsonObject search_;
public:
 void observe(const QJsonObject& frame){
  const auto cycle=frame.value("audit_service").toObject().value("cycle").toObject();const auto request=cycle.value("request_id").toString();
  if(request.isEmpty()||!coherentMiningCycleFrame(frame))return;
  if(request!=request_){request_=request;audits_={};search_={};}
  const double total=frame.value("positions_total").toString().toDouble(),done=frame.value("positions").toString().toDouble();
  if(frame.value("work_role")=="search"){
   if(total>0&&done>=0&&done<=total&&!frame.value("source_stale").toBool())
    search_={{"total",total},{"fraction",done/total},{"remaining",total-done}};
   return;
  }
  if(frame.value("work_role")!="audit")return;
  const auto identity=frame.value("ticket_id").toString();
  if(identity.isEmpty()||total<=0||done<0||done>total)return;
  const auto stages=cycle.value("audits").toArray();
  const bool regularCommitted=stages.size()==2&&stages[0].toObject().value("state")=="committed"&&stages[1].toObject().value("state")=="committed";
  QJsonObject item{{"identity",identity},{"fraction",done/total},{"remaining",total-done},{"catch_up",cycle.value("catch_up").toBool()&&regularCommitted}};
  for(int i=0;i<audits_.size();++i)if(audits_[i].toObject().value("identity")==identity){audits_[i]=item;return;}
  audits_.append(item);
 }
 const QJsonArray& audits()const{return audits_;}
 const QJsonObject& search()const{return search_;}
};
inline CycleProgress miningCycleProgress(const QJsonObject& frame,QJsonArray observed={},QJsonObject observedSearch={}){
 CycleProgress out;const auto cycle=frame.value("audit_service").toObject().value("cycle").toObject();
 const auto audits=cycle.value("audits").toArray();const int index=cycle.value("active_index").toInt(-1);
 const double total=frame.value("positions_total").toString().toDouble(),done=frame.value("positions").toString().toDouble(),rate=frame.value("eta_rate").toDouble();

 // A service audit can precede request issuance. Its observed task progress
 // must not look like a completed search or borrow the preceding ticket.
 if(cycle.isEmpty()){
  const auto service=frame.value("audit_service").toObject();
  const auto view=service.value("preparation").toObject();
  if(frame.value("work_role")!="audit"||service.value("mode")!="two_regular_one_catchup"
     ||view.value("schema")!="PGN_PRE_TICKET_AUDIT_VIEW_V1"
     ||view.value("search_assigned")!=QJsonValue(false)
     ||view.value("active_audit_id").toString().isEmpty()
     ||view.value("active_audit_id")!=frame.value("ticket_id")
     ||total<=0||done<0||done>total)return out;
  const auto confirmedValue=view.value("confirmed_service_answers");
  const auto activeValue=view.value("active_index");
  const bool countKnown=confirmedValue.isDouble();
  const int confirmed=countKnown?confirmedValue.toInt(-1):-1;
  const int active=activeValue.isDouble()?activeValue.toInt(-1):-1;
  if((countKnown&&(confirmed<0||confirmed>2||confirmedValue.toDouble()!=confirmed))
     ||(!countKnown&&!confirmedValue.isNull())
     ||(!activeValue.isNull()&&(!activeValue.isDouble()||active<0||active>1||activeValue.toDouble()!=active))
     ||(countKnown&&(active<0||active>confirmed))||(!countKnown&&active>=0))return out;
  out.known=true;out.preparing=true;
  out.title=active>=0?QString("Next ticket · Audit %1/2").arg(active+1):QString("Next ticket · audits");out.phase="Auditing";
  const double local=std::clamp(done/total,0.,1.);
  for(int i=0;i<2;++i){
   const bool committed=countKnown&&i<confirmed;
   const double fraction=committed?1.:i==active?local:0.;
   out.slices.append({.05,fraction,"audit",false,!countKnown});
   out.fraction+=.05*fraction;
  }
  // There is no assigned search range yet. A dashed placeholder is a phase,
  // never a completed job, skipped audit, admission receipt or measured ETA.
  out.slices.append({.9,0.,"search",false,true});
  if(rate>0&&std::isfinite(rate))out.taskEta=std::max(0.,total-done)/rate;
  return out;
 }
 if(cycle.value("schema")!="PGN_MINING_CYCLE_VIEW_V1"||audits.size()!=2||index<0||index>2||total<=0||done<0||done>total)return out;
 const bool auditing=frame.value("work_role")=="audit";const double local=std::clamp(done/total,0.,1.);
 int active=-1;const auto identity=frame.value("ticket_id").toString();
 const bool regularCommitted=audits[0].toObject().value("state")=="committed"&&audits[1].toObject().value("state")=="committed";
 QJsonArray catchups;
 if(cycle.value("extra_audit_ids").isArray()){
  // The execution journal owns admission. Frame arrival order cannot create
  // an extra audit or reclassify a regular audit during a queue handoff.
  for(const auto& id:cycle.value("extra_audit_ids").toArray()){
   QJsonObject match{{"identity",id.toString()},{"fraction",0.}};
   for(const auto& item:observed)if(item.toObject().value("identity")==id){match=item.toObject();break;}
   if(cycle.value("completed_extra_audit_ids").toArray().contains(id)){match.insert("fraction",1.);match.insert("remaining",0.);}
   match.insert("catch_up",true);catchups.append(match);
  }
 }else for(const auto& item:observed)if(item.toObject().value("catch_up").toBool())catchups.append(item);
 if(regularCommitted&&(!catchups.isEmpty()||(auditing&&cycle.value("catch_up").toBool()))){
  observed=QJsonArray{QJsonObject{{"fraction",1.},{"remaining",0.}},QJsonObject{{"fraction",1.},{"remaining",0.}}};
  for(const auto& item:catchups)observed.append(item);
 }
 if(observed.isEmpty()){
  // On attachment mid-cycle, only confirmed completed work is reconstructible.
  for(int i=0;i<2;++i){const auto a=audits[i].toObject();if(a.value("state")=="committed")observed.append(QJsonObject{{"fraction",1.},{"remaining",0.}});else break;}
 }
 if(auditing){
  for(int i=0;i<observed.size();++i)if(observed[i].toObject().value("identity")==identity){active=i;break;}
  if(active<0){active=observed.size();observed.append(QJsonObject{{"identity",identity},{"fraction",local},{"remaining",total-done}});}
 }
 const int count=auditing?std::max(2,int(observed.size())):int(observed.size());
 const double scale=.9+.05*count;double remaining=0;bool etaKnown=true;
 out.known=true;out.title="Ticket "+cycle.value("ticket_number").toString();
 out.phase=auditing?(active<2?QString("Audit %1/2").arg(active+1):QString("Catch-up audit %1").arg(active-1)):QString("Searching");
 for(int i=0;i<count;++i){
  const auto item=i<observed.size()?observed[i].toObject():QJsonObject{};
  const double fraction=i==active?local:std::clamp(item.value("fraction").toDouble(),0.,1.);
  out.slices.append({.05/scale,fraction,i<2?"audit":"catch_up",false});
  if(i==active)remaining+=total-done;else if(item.value("remaining").isDouble())remaining+=item.value("remaining").toDouble();
  else if(i<audits.size()&&audits[i].toObject().value("positions").isString())remaining+=audits[i].toObject().value("positions").toString().toDouble();else etaKnown=false;
 }
 const double search=cycle.value("search_positions").toString().toDouble();
 const bool retainedSearch=search>0&&observedSearch.value("total").toDouble()==search;
 const double searchFraction=auditing?(retainedSearch?std::clamp(observedSearch.value("fraction").toDouble(),0.,1.):0.):local;
 out.slices.append({.9/scale,searchFraction,"search",false});
 if(!auditing)remaining+=total-done;
 else if(retainedSearch)remaining+=observedSearch.value("remaining").toDouble();
 else if(search>0&&active<2)remaining+=search;
 else etaKnown=false; // A mid-search attachment cannot reconstruct its prefix.
 for(const auto& slice:out.slices)out.fraction+=slice.weight*slice.fraction;
 if(rate>0&&std::isfinite(rate)){out.taskEta=(total-done)/rate;if(etaKnown)out.cycleEta=remaining/rate;}
 return out;
}
}
