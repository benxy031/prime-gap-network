#pragma once
#include "public_display_verifier.h"
#include "social/social_link_core_service.h"
#include <QRandomGenerator>
namespace pgn::social {
class CurrentDisplayConsent:public QObject{
 PublicDisplayRpc rpc_;NameLinkWalletSigner signer_;Identity identity_;PublicDisplayVerifier verifier_;OffchainDisplayStore store_;NameLinkPacket current_;
 void project(NameLinkIntent intent,QByteArray source,NameLinkPreparedReply done){
  verifier_.verify(source,[this,intent,done](DisplayProjection p,QString error){
   if(!error.isEmpty()){done({},error);return;}
   if(p.packet.socialScript!=intent.socialScript||p.packet.nativeScript!=intent.nativeScript||p.packet.previous!=intent.previousHead||p.packet.action!=intent.action){done({},"Current display packet scope changed.");return;}
   current_=p.packet;const auto& c=p.packet.certificate;const auto head=store_.head(p.packet.socialScript);
   NameLinkPrepared prepared{p.packet,p.label,p.anchor,p.height,!c["social_proof"].toString().isEmpty(),!c["operator_proof"].toString().isEmpty(),!c["authorization_proof"].toString().isEmpty(),false};
   prepared.displayVerified=head.head==p.packet.id&&(head.state=="linked"||head.state=="withdrawn");done(std::move(prepared),{});
  });
 }
 void restoreOne(QString script,QVector<QByteArray> packets,int index,QVector<DisplayProjection> checked,std::function<void(QString)> done){
  if(index>=packets.size()){
   auto staged=store_;staged.invalidateScript(script);for(const auto& p:checked){const auto error=staged.accept(p,false);if(!error.isEmpty()){store_.invalidateScript(script);done(error);return;}}
   store_=std::move(staged);done({});return;
  }
  verifier_.verify(packets[index],[this,script,packets,index,checked,done](DisplayProjection p,QString error)mutable{
   if(!error.isEmpty()){store_.invalidateScript(script);done(error);return;}checked.append(std::move(p));restoreOne(script,packets,index+1,std::move(checked),done);
  });
 }
public:
 std::function<void()> changed;
 CurrentDisplayConsent(PublicDisplayRpc rpc,NameLinkWalletSigner signer,Identity identity,QString dir,QObject* parent=nullptr):
 QObject(parent),rpc_(std::move(rpc)),signer_(std::move(signer)),identity_(std::move(identity)),verifier_(rpc_,identity_,this),store_(dir,identity_.genesis,16){}
 auto& store(){return store_;}
 void restore(QString script,std::function<void(QString)> done){
  QVector<QByteArray> packets;auto error=store_.pending(script,packets);if(!error.isEmpty()){done(error);return;}
  // Only public, fully reverified packets load. Topological order ensures an
  // unknown K withdrawal is not consumed before its consenting parent arrives.
  QVector<QByteArray> ordered;QSet<QString> known;int left=packets.size();
  while(left){bool progress=false;for(auto i=packets.begin();i!=packets.end();){NameLinkPacket p;decodeNameLinkPacket(*i,identity_.genesis,p);if(p.previous==QString(64,'0')||known.contains(p.previous)){known.insert(p.id);ordered.append(*i);i=packets.erase(i);--left;progress=true;}else ++i;}if(!progress){done("Missing parent in persisted public context; fallback retained.");return;}}
  restoreOne(script,ordered,0,{},[this,done](QString e){if(changed)changed();done(e);});
 }
 QString display(QString script)const{return store_.display(identity_,script);}
 void verifyAndAdmit(QByteArray source,std::function<void(QString)> done){
  verifier_.verify(source,[this,done](DisplayProjection p,QString error){if(error.isEmpty())error=store_.accept(p);if(changed)changed();done(error);});
 }
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
   weak->verifyAndAdmit(source,[weak,intent,id,s,done](QString error){if(!weak)return;const auto state=weak->store_.head(s);
    done(QJsonObject{{"schema","PGN_SOCIAL_DISPLAY_SUBMIT_2"},{"genesis",intent.session.genesis},{"event_id",id},{"permissions_granted",false},{"state",error.isEmpty()&&(state.state=="linked"||state.state=="withdrawn")?"verified_local":"conflict"},{"reason",error.isEmpty()?state.state:error}},error);
   });
  };
  return result;
 }
};
}

