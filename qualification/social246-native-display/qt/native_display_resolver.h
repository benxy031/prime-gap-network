#pragma once
#include "public_display_verifier.h"
#include <QSet>
namespace pgn::social {
// Typed consumer of the isolated Native display registry. A name grants no
// permissions; identity remains the complete Social signing script.
class NativeDisplayResolver final:public QObject {
 PublicDisplayRpc rpc_;Identity identity_;
 static bool script(const QString& value){return QRegularExpression("^0014[0-9a-f]{40}$").match(value).hasMatch();}
 static bool hash(const QString& value){return QRegularExpression("^[0-9a-f]{64}$").match(value).hasMatch();}
 void submitOne(QByteArray source,QString id,int attempts,std::function<void(QJsonObject,QString)> done){
  const QPointer<NativeDisplayResolver> weak(this);
  rpc_("submitsocialdisplaylink",{QString::fromUtf8(source)},[weak,source,id,attempts,done](QJsonValue value,QString error){
   if(!weak)return;const auto answer=value.toObject();
   if(!error.isEmpty()){done({},error);return;}
   const auto state=answer["state"].toString();
   if(state=="queued"){
    if(attempts<=0){done({},"Native display admission is still pending; keep the signed packet.");return;}
    QTimer::singleShot(100,weak,[weak,source,id,attempts,done]{if(weak)weak->submitOne(source,id,attempts-1,done);});return;
   }
   if(answer["schema"]!="PGN_SOCIAL_DISPLAY_SUBMIT_2"||answer["genesis"]!=weak->identity_.genesis||answer["event_id"]!=id||!answer["permissions_granted"].isBool()||answer["permissions_granted"].toBool()){
    done({},"Native display response changed identity, content or purpose.");return;
   }
   if(state!="accepted_local"){done(answer,answer["reason"].toString("Display proof not admitted; keep the signed packet."));return;}
   done(answer,{});
  });
 }
public:
 NativeDisplayResolver(PublicDisplayRpc rpc,Identity identity,QObject* parent=nullptr):QObject(parent),rpc_(std::move(rpc)),identity_(std::move(identity)){}
 void submit(QByteArray source,std::function<void(QJsonObject,QString)> done){
  NameLinkPacket packet;const auto error=decodeNameLinkPacket(source,identity_.genesis,packet);
  if(!error.isEmpty()){done({},error);return;}submitOne(std::move(source),packet.id,40,std::move(done));
 }
 void decorate(Snapshot snapshot,std::function<void(Snapshot,QString)> done){
  if(snapshot.identity!=identity_||snapshot.section!=Section::Chat){done(std::move(snapshot),{});return;}
  QJsonArray targets;QSet<QString> selected;
  const auto add=[&](const QString& value){if(script(value)&&!selected.contains(value)&&targets.size()<100){targets.append(value);selected.insert(value);}};
  for(const auto& entry:snapshot.entries)add(entry.authorId);
  if(snapshot.participants)for(const auto& member:*snapshot.participants)add(member.id);
  if(targets.isEmpty()){done(std::move(snapshot),{});return;}
  const QPointer<NativeDisplayResolver> weak(this);
  rpc_("getsocialdisplaylinks",{targets},[weak,selected,snapshot=std::move(snapshot),done](QJsonValue value,QString error)mutable{
   if(!weak)return;const auto root=value.toObject();const auto anchor=root["bestblockhash"].toString();const auto rawHeight=root["height"].toDouble(-1);
   // Older Core remains a fully usable Chat backend; absence/loading of this
   // independent capability leaves its honest script fallback in place.
   if(!error.isEmpty()||root["state"]=="loading"||root["schema"]!="PGN_SOCIAL_CURRENT_DISPLAY_1"||root["genesis"]!=weak->identity_.genesis||!root["current_display_only"].toBool()||!root["permissions_granted"].isBool()||root["permissions_granted"].toBool()||!hash(anchor)||rawHeight<0||rawHeight>2147483647||rawHeight!=int(rawHeight)){
    done(std::move(snapshot),{});return;
   }
   const int height=int(rawHeight);const auto rows=root["results"].toArray();QHash<QString,QString> names;QSet<QString> seen;
   if(rows.size()>100){done(std::move(snapshot),{});return;}
   for(const auto& value:rows){
    const auto row=value.toObject();const auto social=row["social_script"].toString(),label=row["display_name"].toString();
    if(!selected.contains(social))continue;
    if(seen.contains(social)){names.remove(social);continue;}seen.insert(social);
    if(row["state"]=="linked"&&row["complete"].isBool()&&row["complete"].toBool()&&hash(row["certificate_id"].toString())&&hash(row["operator_id"].toString())&&script(row["operator_script"].toString())&&!label.trimmed().isEmpty()&&label.size()<=120)
     names.insert(social,label+QString::fromUtf8(" · ")+social.left(8)+QString::fromUtf8("…")+social.right(6));
   }
   weak->rpc_("getblockhash",{height},[weak,anchor,names,snapshot=std::move(snapshot),done](QJsonValue current,QString error)mutable{
    if(!weak)return;
    if(error.isEmpty()&&current.toString()==anchor){
     for(auto& entry:snapshot.entries)if(names.contains(entry.authorId))entry.author=names[entry.authorId];
     if(snapshot.participants)for(auto& member:*snapshot.participants)if(names.contains(member.id))member.name=names[member.id];
    }
    done(std::move(snapshot),{});
   });
  });
 }
};
}
