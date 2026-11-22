
#pragma once
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <optional>

namespace pgn::wallet {
inline bool legacyCycleWorkMatches(const QJsonObject& work,const QString& request,
                                  const QString& ticket,const QString& genesis) {
 return work.value("request_id")==request&&work.value("ticket")==ticket&&work.value("genesis")==genesis;
}
inline QString projectedAuditId(const QString& request,qint64 slot,qint64 attempt) {
 static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
 if(!digest.match(request).hasMatch()||request==QString(64,'0')||slot<0||slot>1||attempt<0||attempt>4294967295LL)return {};
 const QByteArray bytes="{\"attempt\":"+QByteArray::number(attempt)+
     ",\"domain\":\"PGN_HORIZON_AUDIT_VIEW_V1\",\"request\":\""+request.toLatin1()+
     "\",\"slot\":"+QByteArray::number(slot)+"}";
 return QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
}
inline std::optional<QJsonObject> resolveLegacyCycleAudits(const QString& request,const QJsonArray& extraIds,
    const QString& genesis,const QJsonObject& service,const QJsonObject& activity,const QString& verifiedAncestor={}) {
 static const QRegularExpression digest(QStringLiteral("^[0-9a-f]{64}$"));
 const auto state=activity.value("state").toObject();
 const auto anchor=state.value("bestblockhash").toString();
 if(service.value("schema")!="PGN_NATIVE_ASSIGNED_AUDIT_SERVICE_V1"||service.value("enforced")!=QJsonValue(true)||
    service.value("request_id")!=request||activity.value("schema")!="PGN_AUDIT_ACTIVITY_V1"||
    !digest.match(anchor).hasMatch()||anchor==QString(64,'0')||state.value("genesis")!=genesis||
    (service.value("view_parent")!=anchor&&(verifiedAncestor.isEmpty()||service.value("view_parent")!=verifiedAncestor))||
    !service.value("credits").isArray()||!activity.value("audits").isArray())return {};
 const auto credits=service.value("credits").toArray();
 const auto audits=activity.value("audits").toArray();
 if(credits.size()>2||extraIds.size()>4||audits.size()>64)return {};
 QJsonArray regular,extras;
 for(const auto& value:credits){
  const auto entry=value.toObject();
  const auto id=projectedAuditId(entry.value("request_id").toString(),entry.value("slot").toInteger(-1),entry.value("attempt").toInteger(-1));
  const auto commitment=entry.value("commitment").toString();
  if(id.isEmpty()||!digest.match(commitment).hasMatch()||commitment==QString(64,'0'))return {};
  regular.append(QJsonObject{{"audit_id",id},{"request",entry.value("request_id")},{"slot",entry.value("slot")},
                              {"attempt",entry.value("attempt")},{"commitment",commitment}});
 }
 for(const auto& value:extraIds){
  const auto wanted=value.toString();
  if(!digest.match(wanted).hasMatch())return {};
  QJsonObject match;
  for(const auto& element:audits){
   const auto report=element.toObject();
   const auto parent=report.value("request_id").toString();
   for(const auto& item:report.value("jobs").toArray()){
    const auto job=item.toObject();
    const auto slot=job.value("slot").toInteger(-1),attempt=job.value("attempt").toInteger(-1);
    if(projectedAuditId(parent,slot,attempt)!=wanted||job.value("expired").toBool())continue;
    if(!match.isEmpty())return {};
    const auto commitment=job.value("commitment").toString();
    if(!digest.match(commitment).hasMatch()||commitment==QString(64,'0'))continue;
    match={{"audit_id",wanted},{"request",parent},{"slot",slot},{"attempt",attempt},{"commitment",commitment}};
    if(job.value("actor").isString())match.insert("actor",job.value("actor"));
   }
  }
  if(!match.isEmpty())extras.append(match);
 }
 return QJsonObject{{"anchor",anchor},{"audits",audits},{"regular_refs",regular},{"extra_refs",extras}};
}
}
