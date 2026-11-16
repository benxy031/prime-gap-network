#include "wallet/core_runtime.h"
#include "social/social_access_signing_contract.h"
#include "social/social_rpc_queue.h"
namespace pgn::wallet {
void CoreRuntime::signSocialAccessConsent(const QString& sessionScript,const QString& genesis,const QString& script,const QString& community,const QByteArray& command,std::function<void(QString,QString)> consume){
 const social::Identity identity{genesis,sessionScript};const auto invalid=social::accessSigningError(command,identity,script,community);const auto challenge=command;
 if(!invalid.isEmpty()||genesis!=profile_.genesisHash||!connected_||!walletReady_||stopping_||managedUpgradePending_||recovery_.state()==CoreRecoveryPolicy::State::Suppressed||!processAlive()){consume({},"The access command does not match the verified wallet or purpose");return;}
 const auto owner=epoch();const auto lifecycle=recovery_.generation();const auto binding=walletBinding();
 const auto current=[this,owner,lifecycle,binding,genesis]{return owner.matches(epoch())&&lifecycle==recovery_.generation()&&binding==walletBinding()&&genesis==profile_.genesisHash&&connected_&&walletReady_&&!stopping_&&!managedUpgradePending_&&recovery_.state()!=CoreRecoveryPolicy::State::Suppressed&&processAlive();};
 // Confirm this service-wallet session independently of the caller. K may be a
 // different owned key; if it is on another device only a public packet moves.
 readSocialIdentity([this,current,sessionScript,script,challenge,consume](QJsonObject identity,QString error){
  if(!current()||!error.isEmpty()||identity["script"]!=sessionScript){consume({},"Social wallet identity changed; no signature requested");return;}
  if(!socialRpcQueue_)socialRpcQueue_=new social::RpcQueue(this);
  const auto queued=socialRpcQueue_->enqueue("community-sign/"+script,[this,current,script,challenge,consume](social::RpcQueue::Done done){
   const auto once=std::make_shared<bool>(false);const auto finish=[once,consume,done](QString signature,QString error){if(*once)return;*once=true;consume(signature,error);done();};
   if(!current()){finish({},"Community wallet changed while queued");return;}
   rpc("getdescriptorinfo",{"raw("+script+")"},[this,current,script,challenge,finish](QJsonValue value,QString error){
    if(!current()||!error.isEmpty()){finish({},"Community public descriptor unavailable");return;}
    rpc("deriveaddresses",{value.toObject()["descriptor"]},[this,current,script,challenge,finish](QJsonValue value,QString error){
     const auto addresses=value.toArray();if(!current()||!error.isEmpty()||addresses.size()!=1){finish({},"Community signing address unavailable");return;}const auto address=addresses[0].toString();
     rpc("getaddressinfo",{address},[this,current,script,address,challenge,finish](QJsonValue value,QString error){
      const auto info=value.toObject();if(!current()||!error.isEmpty()||info["scriptPubKey"]!=script){finish({},"The community-signing wallet context could not be verified.");return;}
      if(!info["ismine"].toBool()){finish({},"PGN_ACCESS_OTHER_WALLET");return;}
      rpc("signmessage",{address,QString::fromUtf8(challenge)},[current,finish](QJsonValue value,QString error){
       if(!current()){finish({},"Wallet changed; consent signature discarded");return;}
       if(!error.isEmpty()||!value.isString()||value.toString().isEmpty()||value.toString().size()>512){finish({},"Wallet consent signature unavailable. Unlock only through the normal explicit wallet controls.");return;}
       finish(value.toString(),{});
      },true);
     },true);
    });
   });
  });
  if(!queued)consume({},"Community consent queue is busy; the public packet is preserved");
 });
}
}
