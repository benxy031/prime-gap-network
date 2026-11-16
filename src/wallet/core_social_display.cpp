#include "wallet/core_runtime.h"
#include "social/social_display_rpc_contract.h"
#include "social/social_rpc_queue.h"
namespace pgn::wallet {
void CoreRuntime::socialDisplayRequest(const QString& method,const QJsonArray& args,std::function<void(QJsonValue,QString)> consume){
 if(!social::validDisplayRpcArguments(method,args,profile_.genesisHash)||!connected_||stopping_||managedUpgradePending_||recovery_.state()==CoreRecoveryPolicy::State::Suppressed||!processAlive()){consume({},"Verified display context and a bounded public request are required");return;}
 const auto owner=epoch();const auto lifecycle=recovery_.generation();const auto binding=walletBinding();const auto genesis=profile_.genesisHash;
 const auto current=[this,owner,lifecycle,binding,genesis]{return owner.matches(epoch())&&lifecycle==recovery_.generation()&&binding==walletBinding()&&genesis==profile_.genesisHash&&connected_&&!stopping_&&!managedUpgradePending_&&recovery_.state()!=CoreRecoveryPolicy::State::Suppressed&&processAlive();};
 if(!socialRpcQueue_)socialRpcQueue_=new social::RpcQueue(this);
 const auto queued=socialRpcQueue_->enqueue("display/"+method,[this,method,args,current,genesis,consume](social::RpcQueue::Done done){
  if(!current()){consume({},"Display connection changed while queued");done();return;}
  rpc(method,args,[current,genesis,method,consume,done](QJsonValue value,QString error){
   if(!current())consume({},"Display connection changed; response discarded");
   else if(!error.isEmpty())consume({},error);
   else if((method=="getsocialdisplaylinks"||method=="submitsocialdisplaylink"||method=="getpgnparticipants"||method=="getpgnleaderboard")&&(!value.isObject()||value.toObject()["genesis"]!=genesis))consume({},"Display response belongs to another network");
   else consume(value,{});done();
  });
 });
 if(!queued)consume({},"Display verification is busy; Chat and mining remain available");
}
void CoreRuntime::signSocialDisplayConsent(const QString& sessionScript,const QString& genesis,social::NameLinkSide side,const QString& script,const QByteArray& certificate,std::function<void(QString,QString)> consume){
 social::NameLinkPacket packet;const auto invalid=social::decodeNameLinkPacket(certificate,genesis,packet);
 const auto wanted=side==social::NameLinkSide::Native?packet.nativeScript:side==social::NameLinkSide::Social?packet.socialScript:packet.certificate["authorizer_script"].toString();
 const auto challenge=side==social::NameLinkSide::Native?packet.nativeChallenge:side==social::NameLinkSide::Social?packet.socialChallenge:packet.transitionChallenge;
 const bool purpose=(packet.action=="bind"&&(side==social::NameLinkSide::Social||side==social::NameLinkSide::Native))||(packet.action=="withdraw"&&side==social::NameLinkSide::Transition);
 if(!invalid.isEmpty()||!purpose||script!=wanted||!social::displayScript(sessionScript)||(side==social::NameLinkSide::Social&&script!=sessionScript)||genesis!=profile_.genesisHash||!connected_||!walletReady_||stopping_||managedUpgradePending_||recovery_.state()==CoreRecoveryPolicy::State::Suppressed||!processAlive()){consume({},"The explicit display consent does not match the verified wallet or purpose");return;}
 const auto owner=epoch();const auto lifecycle=recovery_.generation();const auto binding=walletBinding();
 const auto current=[this,owner,lifecycle,binding,genesis]{return owner.matches(epoch())&&lifecycle==recovery_.generation()&&binding==walletBinding()&&genesis==profile_.genesisHash&&connected_&&walletReady_&&!stopping_&&!managedUpgradePending_&&recovery_.state()!=CoreRecoveryPolicy::State::Suppressed&&processAlive();};
 // Confirm this service-wallet session independently of the caller. K may be a
 // different owned key; if it is on another device only a public packet moves.
 readSocialIdentity([this,current,sessionScript,script,challenge,consume](QJsonObject identity,QString error){
  if(!current()||!error.isEmpty()||identity["script"]!=sessionScript){consume({},"Social wallet identity changed; no signature requested");return;}
  if(!socialRpcQueue_)socialRpcQueue_=new social::RpcQueue(this);
  const auto queued=socialRpcQueue_->enqueue("display-sign/"+script,[this,current,script,challenge,consume](social::RpcQueue::Done done){
   const auto once=std::make_shared<bool>(false);const auto finish=[once,consume,done](QString signature,QString error){if(*once)return;*once=true;consume(signature,error);done();};
   if(!current()){finish({},"Display wallet changed while queued");return;}
   rpc("getdescriptorinfo",{"raw("+script+")"},[this,current,script,challenge,finish](QJsonValue value,QString error){
    if(!current()||!error.isEmpty()){finish({},"Display public descriptor unavailable");return;}
    rpc("deriveaddresses",{value.toObject()["descriptor"]},[this,current,script,challenge,finish](QJsonValue value,QString error){
     const auto addresses=value.toArray();if(!current()||!error.isEmpty()||addresses.size()!=1){finish({},"Display signing address unavailable");return;}const auto address=addresses[0].toString();
     rpc("getaddressinfo",{address},[this,current,script,address,challenge,finish](QJsonValue value,QString error){
      const auto info=value.toObject();if(!current()||!error.isEmpty()||!info["ismine"].toBool()||info["scriptPubKey"]!=script){finish({},"This wallet does not own the selected key. Export the public packet to its owner; no signature was requested.");return;}
      rpc("signmessage",{address,QString::fromUtf8(challenge)},[current,finish](QJsonValue value,QString error){
       if(!current()){finish({},"Wallet changed; consent signature discarded");return;}
       if(!error.isEmpty()||!value.isString()||value.toString().isEmpty()||value.toString().size()>512){finish({},"Wallet consent signature unavailable. Unlock only through the normal explicit wallet controls.");return;}
       finish(value.toString(),{});
      },true);
     },true);
    });
   });
  });
  if(!queued)consume({},"Display consent queue is busy; the public packet is preserved");
 });
}
}
