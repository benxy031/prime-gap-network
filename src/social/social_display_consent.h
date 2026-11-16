#pragma once
#include "social/social_display_verifier.h"
#include "social/social_display_resolver.h"
#include "social/social_link_flow.h"
#include <QRandomGenerator>
namespace pgn::social {
using NameLinkWalletSigner=std::function<void(Identity,NameLinkSide,QString,QByteArray,std::function<void(QString,QString)>)>;
class CurrentDisplayConsent:public QObject{
 PublicDisplayRpc rpc_;NameLinkWalletSigner signer_;Identity identity_;PublicDisplayVerifier verifier_;NativeDisplayResolver resolver_;NameLinkPacket current_;
 void project(NameLinkIntent intent,QByteArray source,NameLinkPreparedReply done){
  verifier_.verify(source,[this,intent,done](DisplayProjection p,QString error){
   if(!error.isEmpty()){done({},error);return;}
   if(p.packet.socialScript!=intent.socialScript||p.packet.nativeScript!=intent.nativeScript||p.packet.previous!=intent.previousHead||p.packet.action!=intent.action){done({},"Current display packet scope changed.");return;}
   current_=p.packet;
   NameLinkPrepared prepared{p.packet,p.label,p.anchor,p.height,!p.packet.certificate["social_proof"].toString().isEmpty(),!p.packet.certificate["operator_proof"].toString().isEmpty(),!p.packet.certificate["authorization_proof"].toString().isEmpty(),false};
   const QPointer<CurrentDisplayConsent> weak(this);
   rpc_("getsocialdisplaylinks",{QJsonValue(QJsonArray{p.packet.socialScript})},[weak,prepared,done](QJsonValue value,QString error)mutable{
    if(!weak)return;const auto root=value.toObject();const auto rows=root["results"].toArray();
    if(!error.isEmpty()||root["state"]=="loading"||root["schema"]!="PGN_SOCIAL_CURRENT_DISPLAY_1"||root["genesis"]!=weak->identity_.genesis||rows.size()!=1){done({},error.isEmpty()?QString("Native display registry unavailable; Chat is still usable."):error);return;}
    const auto row=rows[0].toObject();if(row["social_script"]!=prepared.packet.socialScript){done({},"Display registry returned another identity.");return;}
    const auto state=row["state"].toString();const auto currentHead=state=="unlinked"?QString(64,'0'):row["certificate_id"].toString();
    if((state!="unlinked"&&state!="linked"&&state!="withdrawn")||!hexField(currentHead,64)||(prepared.packet.previous!=currentHead&&prepared.packet.id!=currentHead)){done({},"The Native association head changed or is conflicting. Keep the public packet; no replacement or fork is chosen automatically.");return;}
    prepared.displayVerified=row["certificate_id"]==prepared.packet.id&&(row["state"]=="linked"||row["state"]=="withdrawn");done(std::move(prepared),{});
   });
  });
 }

public:
 std::function<void()> changed;
 CurrentDisplayConsent(PublicDisplayRpc rpc,NameLinkWalletSigner signer,Identity identity,QObject* parent=nullptr):
 QObject(parent),rpc_(std::move(rpc)),signer_(std::move(signer)),identity_(std::move(identity)),verifier_(rpc_,identity_,this),resolver_(rpc_,identity_,this){}
 NameLinkServices services(){
  NameLinkServices result;result.currentDisplayOnly=true;const QPointer<CurrentDisplayConsent> weak(this);
  result.prepare=[weak](NameLinkIntent intent,QByteArray source,NameLinkPreparedReply done){
   if(!weak||intent.session!=weak->identity_){done({},"Display context changed.");return;}
   if(!source.isEmpty()){weak->project(intent,source,done);return;}
   if(!QRegularExpression("^0014[0-9a-f]{40}$").match(intent.nativeScript).hasMatch()){done({},"Native public signing script required.");return;}
   weak->rpc_("getdescriptorinfo",{"raw("+intent.nativeScript+")"},[weak,intent,done](QJsonValue v,QString error){
    if(!weak)return;if(!error.isEmpty()){done({},error);return;}
    weak->rpc_("deriveaddresses",{v.toObject()["descriptor"]},[weak,intent,done](QJsonValue v,QString error){
     if(!weak)return;const auto addresses=v.toArray();if(!error.isEmpty()||addresses.size()!=1){done({},"Native address unavailable.");return;}
     weak->rpc_("getpgnparticipants",{0,1,addresses[0]},[weak,intent,done](QJsonValue v,QString error){
      if(!weak)return;const auto root=v.toObject();const auto rows=root["participants"].toArray();if(!error.isEmpty()||root["genesis"]!=intent.session.genesis||rows.size()>1||!hexField(root["bestblockhash"].toString(),64)){done({},"Native identity snapshot unavailable.");return;}
      QString op=QString::fromLatin1(QCryptographicHash::hash(QByteArray::fromHex(intent.nativeScript.toLatin1()),QCryptographicHash::Sha256).toHex());
      if(!rows.isEmpty()){const auto row=rows[0].toObject();if(row["signing_script"]!=intent.nativeScript){done({},"Native lookup returned another key.");return;}op=row["operator_id"].toString();}
      QString nonce;for(int i=0;i<4;++i)nonce+=QString("%1").arg(QRandomGenerator::system()->generate64(),16,16,QLatin1Char('0'));
      const QJsonObject statement{{"v",2},{"genesis",intent.session.genesis},{"origin","pgn-social://"+intent.session.genesis},{"scope","display_link_for_script_history"},{"action",intent.action},{"social_script",intent.socialScript},{"operator_script",intent.nativeScript},{"operator_id",op},{"anchor_hash",root["bestblockhash"]},{"anchor_height",root["height"]},{"previous_link",intent.previousHead},{"nonce",nonce}};
      const auto source=canonical(QJsonObject{{"statement",statement},{"social_proof",""},{"operator_proof",""},{"authorizer_script",intent.action=="withdraw"?intent.nativeScript:QString{}},{"authorization_proof",""}});
      weak->project(intent,source,done);
     });
    });
   });
  };
  result.sign=[weak](NameLinkIntent intent,NameLinkSide side,QString script,QByteArray challenge,auto done){
   if(!weak||intent.session!=weak->identity_||!weak->signer_){done({},"Dedicated consent signer unavailable.");return;}
   const auto p=weak->current_;const auto expected=side==NameLinkSide::Native?p.nativeChallenge:side==NameLinkSide::Social?p.socialChallenge:p.transitionChallenge;
   const auto owner=side==NameLinkSide::Native?p.nativeScript:side==NameLinkSide::Social?p.socialScript:p.certificate["authorizer_script"].toString();
   if(script!=owner||challenge!=expected||(side==NameLinkSide::Social&&script!=intent.session.subject)){done({},"Consent signing scope changed.");return;}
   weak->signer_(intent.session,side,script,p.source,done);
  };
  result.submit=[weak](NameLinkIntent intent,QByteArray source,auto done){
   if(!weak||intent.session!=weak->identity_){done({},"Display context changed.");return;}
   NameLinkPacket p;auto error=decodeNameLinkPacket(source,intent.session.genesis,p);if(!error.isEmpty()){done({},error);return;}const auto id=p.id,s=p.socialScript;
   weak->resolver_.submit(source,[weak,intent,id,done](QJsonObject answer,QString error){
    if(!weak)return;if(!error.isEmpty()){done(answer,error);return;}
    // Native performs durable proof admission and P2P relay. No second bounded
    // history cache can erase or prevent a legitimate future transition.
    answer.insert("state","verified_local");if(weak->changed)weak->changed();done(answer,{});
   });
  };
  return result;
 }
};
}

