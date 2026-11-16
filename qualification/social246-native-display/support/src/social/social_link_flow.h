#pragma once
#include "social/social_link_packet.h"
#include <QObject>
#include <QPointer>
#include <memory>
namespace pgn::social {
enum class NameLinkSide {Social,Native,Transition};
enum class NameLinkPhase {Idle,Preparing,NeedsNativeConsent,ReadyForSocialConsent,NativeConsentReady,Signing,Submitting,Pending,Included,Blocked,Uncertain,VerifiedLocal};
struct NameLinkIntent {Identity session;QString socialScript,nativeScript,action{"bind"},previousHead{QString(64,'0')};bool operator==(const NameLinkIntent&)const=default;};
struct NameLinkPrepared {NameLinkPacket packet;QString displayName,anchor;int height{-1};bool socialVerified{},nativeVerified{},transitionVerified{},included{},displayVerified{};};
using NameLinkPreparedReply=std::function<void(NameLinkPrepared,QString)>;
struct NameLinkServices {
 bool currentDisplayOnly{}; // Explicit projection, never historical V2 inclusion or roles
 // Existing certificate source is empty only for creation. Import revalidates
 // its original anchor/Native K->O plus any provided consent signatures.
 std::function<void(NameLinkIntent,QByteArray,NameLinkPreparedReply)> prepare;
 // Dedicated wallet signer: selected purpose/domain/script only. No audit
 // signer, wallet unlock or arbitrary RPC is exposed through this interface.
 std::function<void(NameLinkIntent,NameLinkSide,QString,QByteArray,std::function<void(QString,QString)>)> sign;
 std::function<void(NameLinkIntent,QByteArray,std::function<void(QJsonObject,QString)>)> submit;
};
class NameLinkFlow final:public QObject {
 NameLinkServices services_;NameLinkIntent intent_;NameLinkPrepared prepared_;quint64 generation_{},lookup_{};
 NameLinkPhase phase_{NameLinkPhase::Idle};QString reason_;bool submitted_{};
 void notify(){if(changed)changed();}
 void fail(QString reason,bool uncertain=false){reason_=std::move(reason);phase_=uncertain?NameLinkPhase::Uncertain:NameLinkPhase::Blocked;notify();}
 bool bound(const NameLinkPrepared& p)const{return p.packet.statement.value("genesis").toString()==intent_.session.genesis&&p.packet.socialScript==intent_.socialScript&&p.packet.nativeScript==intent_.nativeScript&&p.packet.action==intent_.action&&p.packet.previous==intent_.previousHead;}
 void preparedPhase(){reason_.clear();phase_=services_.currentDisplayOnly&&prepared_.displayVerified?NameLinkPhase::VerifiedLocal:prepared_.included?NameLinkPhase::Included:submitted_?NameLinkPhase::Pending:intent_.action=="withdraw"?(prepared_.transitionVerified?NameLinkPhase::NativeConsentReady:NameLinkPhase::ReadyForSocialConsent):(prepared_.nativeVerified?(prepared_.socialVerified?NameLinkPhase::NativeConsentReady:NameLinkPhase::ReadyForSocialConsent):NameLinkPhase::NeedsNativeConsent);notify();}
 void verifyPacket(QByteArray packet){
  const auto script=[](const QString& value){return QRegularExpression("^0014[0-9a-f]{40}$").match(value).hasMatch();};
  if(!intent_.session.valid()||!script(intent_.session.subject)||!script(intent_.socialScript)||!script(intent_.nativeScript)||!QRegularExpression("^[0-9a-f]{64}$").match(intent_.previousHead).hasMatch()||(intent_.action!="bind"&&intent_.action!="withdraw")){fail("Verified network, public keys, action and current head are required.");return;}
  if(!services_.prepare){fail("Public name links are not supported by this Core. Chat remains available.");return;}
  phase_=NameLinkPhase::Preparing;notify();const auto g=generation_,lookup=++lookup_;const QPointer<NameLinkFlow> weak(this);const auto once=std::make_shared<bool>(false);
  services_.prepare(intent_,packet,[weak,g,lookup,once](NameLinkPrepared answer,QString error){if(!weak||weak->generation_!=g||weak->lookup_!=lookup||*once)return;*once=true;
   if(!error.isEmpty()){weak->fail(error);return;}if(!weak->bound(answer)){weak->fail("The Native response does not match this network, keys, action and current head.");return;}
   NameLinkPacket parsed;const auto invalid=decodeNameLinkPacket(answer.packet.source,weak->intent_.session.genesis,parsed);if(!invalid.isEmpty()||parsed.id!=answer.packet.id){weak->fail(invalid.isEmpty()?QString("Consent content ID changed."):invalid);return;}
   answer.packet=std::move(parsed);if(!weak->bound(answer)){weak->fail("The canonical certificate differs from the requested Native keys or head.");return;}
   const auto& cert=answer.packet.certificate;if(answer.socialVerified!=!cert.value("social_proof").toString().isEmpty()||answer.nativeVerified!=!cert.value("operator_proof").toString().isEmpty()||answer.transitionVerified!=!cert.value("authorization_proof").toString().isEmpty()){weak->fail("Native consent coverage does not match the signed packet.");return;}
   if(answer.included&&!(weak->intent_.action=="bind"?answer.socialVerified&&answer.nativeVerified:answer.transitionVerified)){weak->fail("Incomplete consent cannot be included.");return;}
   weak->prepared_=std::move(answer);weak->preparedPhase();
  });
 }
public:
 std::function<void()> changed;
 explicit NameLinkFlow(NameLinkServices services,QObject* parent=nullptr):QObject(parent),services_(std::move(services)){}
 void setIntent(NameLinkIntent intent){++generation_;intent_=std::move(intent);prepared_={};submitted_=false;phase_=NameLinkPhase::Idle;reason_.clear();notify();}
 const auto& intent()const{return intent_;}const auto& prepared()const{return prepared_;}NameLinkPhase phase()const{return phase_;}QString reason()const{return reason_;}
 QByteArray packet()const{return prepared_.packet.source;}
 void prepare(){++generation_;prepared_={};submitted_=false;verifyPacket({});}
 void refresh(){if(prepared_.packet.source.isEmpty())return;++generation_;verifyPacket(prepared_.packet.source);}
 void importPacket(QByteArray source){NameLinkPacket packet;const auto invalid=decodeNameLinkPacket(source,intent_.session.genesis,packet);if(!invalid.isEmpty()){fail(invalid);return;}
  if(packet.socialScript!=intent_.socialScript||packet.nativeScript!=intent_.nativeScript||packet.action!=intent_.action||packet.previous!=intent_.previousHead){fail("Imported consent is for another key, action or current head.");return;}++generation_;submitted_=false;verifyPacket(std::move(source));
 }
 void chooseWithdrawalSigner(const QString& script){
  if(intent_.action!="withdraw"||prepared_.packet.source.isEmpty()||(script!=intent_.socialScript&&script!=intent_.nativeScript)){fail("Select one participant key from the current Native lineage.");return;}
  auto certificate=prepared_.packet.certificate;certificate.insert("authorizer_script",script);certificate.insert("authorization_proof",QString{});++generation_;verifyPacket(canonical(certificate));
 }
 void sign(NameLinkSide side,bool explicitConsent){
  if(!explicitConsent){fail("Explicit consent is required. No wallet signature was requested.");return;}
  if(prepared_.packet.source.isEmpty()||!services_.sign||phase_==NameLinkPhase::Idle||phase_==NameLinkPhase::Preparing||phase_==NameLinkPhase::Signing||phase_==NameLinkPhase::Submitting||phase_==NameLinkPhase::Pending||phase_==NameLinkPhase::Included||phase_==NameLinkPhase::VerifiedLocal||phase_==NameLinkPhase::Blocked||phase_==NameLinkPhase::Uncertain){fail("A current Native-checked consent packet is required.");return;}
  if(side==NameLinkSide::Social&&(intent_.session.subject!=intent_.socialScript||!prepared_.nativeVerified)){fail("Native consent must be verified before this device signs its own Social key.");return;}
  if(side==NameLinkSide::Transition&&intent_.action!="withdraw"){fail("This packet is not a withdrawal.");return;}
  if(side!=NameLinkSide::Transition&&intent_.action!="bind"){fail("This packet requires only the participant withdrawal domain.");return;}
  const auto script=side==NameLinkSide::Native?intent_.nativeScript:side==NameLinkSide::Social?intent_.socialScript:prepared_.packet.certificate.value("authorizer_script").toString();
  const auto challenge=side==NameLinkSide::Native?prepared_.packet.nativeChallenge:side==NameLinkSide::Social?prepared_.packet.socialChallenge:prepared_.packet.transitionChallenge;
  phase_=NameLinkPhase::Signing;reason_.clear();notify();const auto g=generation_;const auto event=prepared_.packet.id;const QPointer<NameLinkFlow> weak(this);const auto once=std::make_shared<bool>(false);
  services_.sign(intent_,side,script,challenge,[weak,g,event,side,once](QString signature,QString error){if(!weak||weak->generation_!=g||weak->prepared_.packet.id!=event||*once)return;*once=true;
   if(!error.isEmpty()||signature.isEmpty()){weak->fail(error.isEmpty()?QString("Wallet signature unavailable; the public packet is preserved."):error);return;}
   auto certificate=weak->prepared_.packet.certificate;certificate.insert(side==NameLinkSide::Native?"operator_proof":side==NameLinkSide::Social?"social_proof":"authorization_proof",signature);
   // Native prepare rechecks partial/full proofs. Neither a returned string
   // nor a local bool is accepted as verified consent by this state machine.
   weak->verifyPacket(canonical(certificate));
  });
 }
 void submit(bool explicitConsent){
  if(!explicitConsent){fail("Explicit submission consent is required.");return;}
  const bool complete=intent_.action=="bind"?prepared_.socialVerified&&prepared_.nativeVerified:prepared_.transitionVerified;
  if(phase_!=NameLinkPhase::NativeConsentReady||!complete||!services_.submit||prepared_.packet.source.isEmpty()){fail("All required Native-verified consent signatures are needed.");return;}
  phase_=NameLinkPhase::Submitting;reason_.clear();notify();const auto g=generation_;const auto event=prepared_.packet.id;const QPointer<NameLinkFlow> weak(this);const auto once=std::make_shared<bool>(false);
  services_.submit(intent_,prepared_.packet.source,[weak,g,event,once](QJsonObject answer,QString error){if(!weak||weak->generation_!=g||weak->prepared_.packet.id!=event||*once)return;*once=true;
   if(!error.isEmpty()){weak->fail(error,true);return;}
   if(answer.value("schema").toString()!=(weak->services_.currentDisplayOnly?"PGN_SOCIAL_DISPLAY_SUBMIT_2":"PGN_SOCIAL_CONSENT_SUBMIT_2")||answer.value("genesis").toString()!=weak->intent_.session.genesis||answer.value("event_id").toString()!=event||!answer.value("permissions_granted").isBool()||answer.value("permissions_granted").toBool()){weak->fail("Native submission response changed network, content or purpose.",true);return;}
   const auto state=answer.value("state").toString();weak->reason_=answer.value("reason").toString();
   if(weak->services_.currentDisplayOnly&&state=="verified_local")weak->phase_=NameLinkPhase::VerifiedLocal;
   else if(state=="pending"||state=="queued"){weak->phase_=NameLinkPhase::Pending;weak->submitted_=true;}
   else if(!weak->services_.currentDisplayOnly&&state=="included")weak->phase_=NameLinkPhase::Included;
   else if(state=="uncertain"||state=="busy")weak->phase_=NameLinkPhase::Uncertain;
   else if(state=="conflict"||state=="rejected")weak->phase_=NameLinkPhase::Blocked;
   else{weak->fail("Unknown Native submission outcome; keep the public packet.",true);return;}
   // Keep exact signed source for replay/restart and canonical observation.
   weak->notify();
  });
 }
};
}
