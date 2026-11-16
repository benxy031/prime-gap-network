#pragma once
#include "social/social_link_flow.h"
#include <QJsonArray>
namespace pgn::social {
enum class NameLinkCall {Prepare,Submit,ReadCurrent};
using NameLinkRpc=std::function<void(NameLinkCall,QJsonArray,std::function<void(QJsonValue,QString)>)>;
using NameLinkWalletSigner=std::function<void(Identity,NameLinkSide,QString,QByteArray,std::function<void(QString,QString)>)>;
inline bool validNameLinkRpcArguments(const QString& method,const QJsonArray& args,const QString& genesis){
 const auto script=[](QJsonValue v){return v.isString()&&QRegularExpression("^0014[0-9a-f]{40}$").match(v.toString()).hasMatch();};
 if(method=="preparesocialassociation"){
  if((args.size()!=4&&args.size()!=5)||!script(args[0])||!script(args[1])||!args[2].isString()||(args[2].toString()!="bind"&&args[2].toString()!="withdraw")||!args[3].isString()||!hexField(args[3].toString(),64))return false;
  if(args.size()==4)return true;
  if(!args[4].isString())return false;NameLinkPacket packet;
  return decodeNameLinkPacket(args[4].toString().toUtf8(),genesis,packet).isEmpty()&&packet.socialScript==args[0].toString()&&packet.nativeScript==args[1].toString()&&packet.action==args[2].toString()&&packet.previous==args[3].toString();
 }
 if(method=="submitsocialassociation"&&args.size()==1&&args[0].isString()){
  NameLinkPacket packet;if(!decodeNameLinkPacket(args[0].toString().toUtf8(),genesis,packet).isEmpty())return false;
  const auto& c=packet.certificate;return packet.action=="bind"?(!c["social_proof"].toString().isEmpty()&&!c["operator_proof"].toString().isEmpty()):!c["authorization_proof"].toString().isEmpty();
 }
 return false;
}
inline QString decodePreparedNameLink(const NameLinkIntent& intent,const QJsonObject& object,NameLinkPrepared& prepared){
 if(object["schema"].toString()!="PGN_SOCIAL_CONSENT_PREPARE_2"||object["genesis"].toString()!=intent.session.genesis||!hexField(object["bestblockhash"].toString(),64)||!object["height"].isDouble()||object["height"].toInteger(-1)<0||object["height"].toInteger()>2147483647||object["height"].toDouble()!=double(object["height"].toInteger())||!object["certificate"].isString())return "Native consent response has invalid network or anchor.";
 for(const auto key:{"social_verified","operator_verified","transition_verified","included"})if(!object[key].isBool())return "Native consent coverage is missing.";
 NameLinkPacket packet;const auto invalid=decodeNameLinkPacket(object["certificate"].toString().toUtf8(),intent.session.genesis,packet);if(!invalid.isEmpty())return invalid;
 if(packet.id!=object["event_id"].toString()||packet.socialScript!=intent.socialScript||packet.nativeScript!=intent.nativeScript||packet.action!=intent.action||packet.previous!=intent.previousHead||packet.statement["anchor_height"].toInteger()>object["height"].toInteger())return "Native consent response changed keys, head or content ID.";
 const auto social=intent.action=="bind"?packet.socialChallenge:QByteArray{},native=intent.action=="bind"?packet.nativeChallenge:QByteArray{},transition=intent.action=="withdraw"?packet.transitionChallenge:QByteArray{};
 if(object["social_challenge"].toString().toUtf8()!=social||object["operator_challenge"].toString().toUtf8()!=native||object["transition_challenge"].toString().toUtf8()!=transition)return "Native consent challenge differs from canonical signed data.";
 const auto& cert=packet.certificate;
 if(object["social_verified"].toBool()!=!cert["social_proof"].toString().isEmpty()||object["operator_verified"].toBool()!=!cert["operator_proof"].toString().isEmpty()||object["transition_verified"].toBool()!=!cert["authorization_proof"].toString().isEmpty())return "Native proof coverage differs from public packet.";
 QString name;if(object.contains("display_name")){if(!object["display_name"].isString()||!publicLabel(object["display_name"].toString()))return "Native public attribution is malformed.";name=object["display_name"].toString();}
 prepared={packet,name,object["bestblockhash"].toString(),object["height"].toInt(),object["social_verified"].toBool(),object["operator_verified"].toBool(),object["transition_verified"].toBool(),object["included"].toBool()};return {};
}
class NameLinkCoreService final:public QObject {
 NameLinkRpc rpc_;NameLinkWalletSigner signer_;Identity context_;quint64 epoch_{},lookup_{};NameLinkPacket current_;
public:
 NameLinkCoreService(NameLinkRpc rpc,NameLinkWalletSigner signer,Identity context,QObject* parent=nullptr):QObject(parent),rpc_(std::move(rpc)),signer_(std::move(signer)),context_(std::move(context)){}
 void setContext(Identity context){context_=std::move(context);++epoch_;current_={};}
 NameLinkServices services(){
  NameLinkServices port;const QPointer<NameLinkCoreService> weak(this);
  port.prepare=[weak](NameLinkIntent intent,QByteArray existing,NameLinkPreparedReply done){
   if(!weak||intent.session!=weak->context_||!weak->rpc_){done({},"Current native consent connection unavailable.");return;}
   const auto epoch=weak->epoch_,lookup=++weak->lookup_;QJsonArray args{intent.socialScript,intent.nativeScript,intent.action,intent.previousHead};if(!existing.isEmpty())args.append(QString::fromUtf8(existing));
   if(!validNameLinkRpcArguments("preparesocialassociation",args,intent.session.genesis)){done({},"Invalid bounded consent request.");return;}
   weak->rpc_(NameLinkCall::Prepare,args,[weak,intent,epoch,lookup,done](QJsonValue value,QString error){
    if(!weak||weak->epoch_!=epoch||weak->lookup_!=lookup||weak->context_!=intent.session){done({},"Native consent context changed.");return;}
    NameLinkPrepared prepared;if(error.isEmpty())error=value.isObject()?decodePreparedNameLink(intent,value.toObject(),prepared):QString("Native consent response is missing.");
    if(error.isEmpty())weak->current_=prepared.packet;else weak->current_={};done(std::move(prepared),error);
   });
  };
  port.sign=[weak](NameLinkIntent intent,NameLinkSide side,QString script,QByteArray challenge,auto done){
   if(!weak||intent.session!=weak->context_||!weak->signer_||weak->current_.source.isEmpty()){done({},"Dedicated consent signer unavailable.");return;}
   const auto& packet=weak->current_;const auto expected=side==NameLinkSide::Native?packet.nativeChallenge:side==NameLinkSide::Social?packet.socialChallenge:packet.transitionChallenge;
   const auto owner=side==NameLinkSide::Native?packet.nativeScript:side==NameLinkSide::Social?packet.socialScript:packet.certificate["authorizer_script"].toString();
   if(script!=owner||challenge!=expected||packet.socialScript!=intent.socialScript||packet.nativeScript!=intent.nativeScript||packet.previous!=intent.previousHead||packet.action!=intent.action||(side==NameLinkSide::Social&&script!=intent.session.subject)){done({},"Consent signer scope changed.");return;}
   const auto epoch=weak->epoch_;const auto event=packet.id;
   weak->signer_(intent.session,side,script,packet.source,[weak,epoch,event,done](QString signature,QString error){
    if(!weak||weak->epoch_!=epoch||weak->current_.id!=event){done({},"Consent wallet context changed; signature discarded.");return;}done(signature,error);
   });
  };
  port.submit=[weak](NameLinkIntent intent,QByteArray source,auto done){
   if(!weak||intent.session!=weak->context_||!weak->rpc_){done({},"Current Native consent connection unavailable.");return;}
   QJsonArray args{QString::fromUtf8(source)};if(!validNameLinkRpcArguments("submitsocialassociation",args,intent.session.genesis)){done({},"Complete bounded consent required.");return;}
   const auto epoch=weak->epoch_;weak->rpc_(NameLinkCall::Submit,args,[weak,epoch,intent,done](QJsonValue value,QString error){if(!weak||weak->epoch_!=epoch||weak->context_!=intent.session){done({},"Native consent context changed; keep exact public packet.");return;}done(value.toObject(),error);});
  };return port;
 }
};
}
