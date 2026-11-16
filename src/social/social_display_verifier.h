#pragma once
#include "social/social_link_packet_store.h"
#include "social/social_leaderboard.h"
#include <QObject>
#include <QPointer>
#include <QTimer>
namespace pgn::social {
struct DisplayProjection {NameLinkPacket packet;QString label,anchor;int height{-1};};
using PublicDisplayRpc=std::function<void(QString,QJsonArray,std::function<void(QJsonValue,QString)>)>;
// Uses ONLY existing public Core21 RPC. O is verified at CURRENT snapshot.
// Source anchor is checked canonical, NOT used to assert historic K->O.
class PublicDisplayVerifier:public QObject {
 PublicDisplayRpc rpc_;Identity identity_;quint64 generation_{};
 struct Job{QPointer<PublicDisplayVerifier> owner;quint64 generation{};NameLinkPacket packet;QString label,tip,address;int height{-1};std::function<void(DisplayProjection,QString)> done;bool finished{};};
 using Work=std::shared_ptr<Job>;
 void complete(Work j,QString error={}){
  if(j->finished)return;j->finished=true;if(!j->owner||j->generation!=generation_)return;
  j->done({j->packet,j->label,j->tip,j->height},std::move(error));
 }
 void call(Work j,QString method,QJsonArray args,std::function<void(QJsonValue)> next){
  if(j->finished||!j->owner||j->generation!=generation_)return;
  const auto once=std::make_shared<bool>(false);const QPointer<PublicDisplayVerifier> weak(this);
  rpc_(method,args,[weak,j,once,next](QJsonValue value,QString error){if(!weak||j->finished||j->generation!=weak->generation_||*once)return;*once=true;if(!error.isEmpty()){weak->complete(j,error);return;}next(value);});
 }
 void address(Work j,QString script,std::function<void(QString)> next){
  call(j,"getdescriptorinfo",{"raw("+script+")"},[this,j,script,next](QJsonValue v){
   const auto d=v.toObject()["descriptor"].toString();if(d.isEmpty()){complete(j,"Native public descriptor unavailable.");return;}
   call(j,"deriveaddresses",{d},[this,j,script,next](QJsonValue v){
    const auto a=v.toArray();if(a.size()!=1||!a[0].isString()){complete(j,"Native address derivation unavailable.");return;}const auto address=a[0].toString();
    call(j,"validateaddress",{address},[this,j,script,next,address](QJsonValue v){if(v.toObject()["scriptPubKey"]!=script){complete(j,"Native script/address binding mismatch.");return;}next(address);});
   });
  });
 }
 void proof(Work j,QString script,QString signature,QByteArray challenge,std::function<void()> next){
  if(signature.isEmpty()){next();return;} // partial prepare; accept requires complete below
  address(j,script,[this,j,signature,challenge,next](QString a){
   call(j,"verifymessage",{a,signature,QString::fromUtf8(challenge)},[this,j,next](QJsonValue valid){if(valid!=QJsonValue(true)){complete(j,"Public Core rejected BIP322 consent.");return;}next();});
  });
 }
 void signatures(Work j){
  const auto& c=j->packet.certificate;const auto finish=[this,j]{
   call(j,"getblockhash",{j->height},[this,j](QJsonValue value){if(value.toString()!=j->tip){complete(j,"Current Native projection reorganized; keep fallback and retry.");return;}
    call(j,"getblockhash",{j->packet.statement["anchor_height"]},[this,j](QJsonValue value){if(value!=j->packet.statement["anchor_hash"]){complete(j,"Consent anchor reorganized; no display name.");return;}complete(j);});
   });
  };
  if(j->packet.action=="withdraw"){proof(j,c["authorizer_script"].toString(),c["authorization_proof"].toString(),j->packet.transitionChallenge,finish);return;}
  proof(j,j->packet.socialScript,c["social_proof"].toString(),j->packet.socialChallenge,[this,j,finish]{proof(j,j->packet.nativeScript,j->packet.certificate["operator_proof"].toString(),j->packet.nativeChallenge,finish);});
 }
 void page(Work j,qint64 offset=0){
  QJsonArray args{offset,100};if(offset)args.append(j->tip);
  call(j,"getpgnleaderboard",args,[this,j,offset](QJsonValue v){
   const auto root=v.toObject();ReadRequest request{Section::Miners,identity_,Window::AllTime,offset?j->tip+":"+QString::number(offset):QString{},100};Snapshot parsed;
   const auto invalid=decodeLeaderboard(request,root,parsed);
   if(!invalid.isEmpty()||root["bestblockhash"]!=j->tip||root["height"].toInt(-1)!=j->height){complete(j,"Native name/membership snapshots differ; retry without publishing.");return;}
   for(const auto& row:parsed.miners)if(row.id==j->packet.operatorId){if(!row.publicIdentity||!publicLabel(row.publicIdentity->displayName)){complete(j,"Current Native operator has no authenticated public name.");return;}j->label=row.publicIdentity->displayName;signatures(j);return;}
   if(!parsed.nextCursor.isEmpty()){QTimer::singleShot(0,this,[this,j,offset,root]{page(j,root["next_offset"].toInteger());});return;}
   complete(j,"Signed operator is absent from the authenticated current name projection.");
  });
 }
public:
 explicit PublicDisplayVerifier(PublicDisplayRpc rpc,Identity identity,QObject* parent=nullptr):QObject(parent),rpc_(std::move(rpc)),identity_(std::move(identity)){}
 void invalidate(){++generation_;}
 void verify(QByteArray source,std::function<void(DisplayProjection,QString)> done){
  auto j=std::make_shared<Job>();j->owner=this;j->generation=generation_;j->done=std::move(done);
  const auto invalid=decodeNameLinkPacket(source,identity_.genesis,j->packet);if(!invalid.isEmpty()){complete(j,invalid);return;}
  call(j,"getblockhash",{0},[this,j](QJsonValue v){if(v.toString()!=identity_.genesis){complete(j,"Native network context changed.");return;}
   call(j,"getblockhash",{j->packet.statement["anchor_height"]},[this,j](QJsonValue v){if(v!=j->packet.statement["anchor_hash"]){complete(j,"Consent anchor is not canonical.");return;}
    address(j,j->packet.nativeScript,[this,j](QString address){
     j->address=address;call(j,"getpgnparticipants",{0,1,address},[this,j](QJsonValue v){
      const auto root=v.toObject();const auto rows=root["participants"].toArray();j->tip=root["bestblockhash"].toString();j->height=root["height"].toInt(-1);
      if(root["genesis"]!=identity_.genesis||!hexField(j->tip,64)||j->height<0||rows.size()>1){complete(j,"Authenticated current membership unavailable.");return;}
      // NativeAuditOperator exact public fallback; never username/alias matching.
      QString op=QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromHex(j->packet.nativeScript.toLatin1()),QCryptographicHash::Sha256).toHex());
      if(!rows.isEmpty()){const auto row=rows[0].toObject();if(row["signing_script"]!=j->packet.nativeScript||!hexField(row["operator_id"].toString(),64)){complete(j,"Current Native membership target changed.");return;}op=row["operator_id"].toString();}
      if(op!=j->packet.operatorId){complete(j,"Signed operator does not match Native K at the current snapshot.");return;}page(j);
     });
    });
   });
  });
 }
};
}

