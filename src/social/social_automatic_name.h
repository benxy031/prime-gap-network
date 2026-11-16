#pragma once
#include "social/social_display_verifier.h"
#include "social/social_link_flow.h"
#include <QElapsedTimer>

namespace pgn::social {
// Reuse the configured public mining name and the existing two-key proof.
// The label grants no role, and neither another link nor a withdrawal is replaced.
class AutomaticPublicName final:public QObject {
 PublicDisplayRpc rpc_;Identity identity_;NameLinkFlow flow_;NameLinkPacketStore store_;
 QElapsedTimer clock_;QString address_,native_,settled_;QByteArray saved_;
 bool busy_{},queued_{};qint64 nextAttempt_{};quint64 attempt_{};
 void finish(QString state,QString detail={}){
  busy_=false;nextAttempt_=clock_.elapsed()+60000;
  if(changed)changed(std::move(state),std::move(detail));
 }
 void drive(){
  if(!busy_)return;
  const auto phase=flow_.phase();
  switch(phase){
   case NameLinkPhase::NeedsNativeConsent:
   case NameLinkPhase::ReadyForSocialConsent:
   case NameLinkPhase::NativeConsentReady:
    if(flow_.prepared().displayName.isEmpty()){finish("unavailable","The configured mining account has no verified public name.");return;}
    if(phase==NameLinkPhase::NeedsNativeConsent)flow_.sign(NameLinkSide::Native,true);
    else if(phase==NameLinkPhase::ReadyForSocialConsent)flow_.sign(NameLinkSide::Social,true);
    else flow_.submit(true);
    break;
   case NameLinkPhase::VerifiedLocal:case NameLinkPhase::Included:
    settled_=address_;finish("active");break;
   case NameLinkPhase::Blocked:case NameLinkPhase::Uncertain:
    finish("unavailable",flow_.reason());break;
   case NameLinkPhase::Pending:
    finish("pending");break;
   default:break;
  }
 }
 void flowChanged(){
  if(!busy_)return;
  const auto packet=flow_.packet();
  if(!packet.isEmpty()&&packet!=saved_){const auto error=store_.save(packet);if(!error.isEmpty()){finish("unavailable",error);return;}saved_=packet;}
  if(queued_)return;queued_=true;
  QTimer::singleShot(0,this,[this]{queued_=false;drive();});
 }
 void currentLink(quint64 attempt){
  const QPointer<AutomaticPublicName> weak(this);
  rpc_("getsocialdisplaylinks",{QJsonValue(QJsonArray{identity_.subject})},[weak,attempt](QJsonValue value,QString error){
   if(!weak||!weak->busy_||weak->attempt_!=attempt)return;
   const auto root=value.toObject();const auto rows=root["results"].toArray();
   if(error.isEmpty()&&root["state"]=="loading"){
    weak->finish("checking");weak->nextAttempt_=weak->clock_.elapsed()+3000;return;
   }
   if(!error.isEmpty()||root["schema"]!="PGN_SOCIAL_CURRENT_DISPLAY_1"||root["genesis"]!=weak->identity_.genesis||rows.size()!=1||!root["current_display_only"].toBool()||root["permissions_granted"]!=false){weak->finish("unavailable",error);return;}
   const auto row=rows[0].toObject();
   if(row["social_script"]!=weak->identity_.subject||row["complete"]!=true){weak->finish("unavailable","The current name association is incomplete.");return;}
   if(row["state"]=="linked"||row["state"]=="withdrawn"){
    weak->settled_=weak->address_;weak->finish(row["state"]=="linked"?QString("existing"):QString("withdrawn"));return;
   }
   if(row["state"]!="unlinked"){weak->finish("unavailable","The existing association is preserved.");return;}
   QByteArray saved;const auto invalid=weak->store_.load(saved);
   if(!invalid.isEmpty()){weak->finish("unavailable",invalid);return;}
   if(!saved.isEmpty()){
    NameLinkPacket packet;const auto error=decodeNameLinkPacket(saved,weak->identity_.genesis,packet);
    if(!error.isEmpty()||packet.action!="bind"||packet.socialScript!=weak->identity_.subject||packet.nativeScript!=weak->native_||packet.previous!=QString(64,'0')){weak->finish("unavailable","A different saved name association is preserved.");return;}
   }
   weak->saved_=saved;
   weak->flow_.setIntent({weak->identity_,weak->identity_.subject,weak->native_,"bind",QString(64,'0')});
   if(saved.isEmpty())weak->flow_.prepare();else weak->flow_.importPacket(saved);
  });
 }
public:
 std::function<void(QString,QString)> changed;
 AutomaticPublicName(PublicDisplayRpc rpc,NameLinkServices services,Identity identity,QString recovery,QObject* parent=nullptr):
  QObject(parent),rpc_(std::move(rpc)),identity_(std::move(identity)),flow_(std::move(services)),store_(std::move(recovery),identity_){
  clock_.start();flow_.changed=[this]{flowChanged();};
 }
 void ensure(const QString& configuredAddress){
  if(configuredAddress.isEmpty()||configuredAddress.size()>160||!identity_.valid()||busy_||settled_==configuredAddress||clock_.elapsed()<nextAttempt_)return;
  address_=configuredAddress;busy_=true;const auto attempt=++attempt_;const QPointer<AutomaticPublicName> weak(this);
  if(changed)changed("checking",{});
  rpc_("validateaddress",{address_},[weak,attempt](QJsonValue value,QString error){
   if(!weak||!weak->busy_||weak->attempt_!=attempt)return;
   const auto address=value.toObject();const auto script=address["scriptPubKey"].toString();
   if(!error.isEmpty()||address["isvalid"]!=true||!QRegularExpression("^0014[0-9a-f]{40}$").match(script).hasMatch()){weak->finish("unavailable","The configured mining address could not be verified.");return;}
   weak->native_=script;weak->currentLink(attempt);
  });
 }
 bool busy()const{return busy_;}
};
}
