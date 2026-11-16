#pragma once
#include "ui/social_access_client.h"
#include <QDir>
#include <QTimer>

namespace pgn::ui {
// One instance belongs to one verified Core/social-key lifetime. Chat reads
// never await this optional service. No wallet key, credential or role is cached.
class SocialAccessSession final:public QObject {
public:
 using Rpc=std::function<void(QString,QJsonArray,std::function<void(QJsonValue,QString)>)>;
 using Signer=std::function<void(QString,QByteArray,std::function<void(QString,QString)>)>;
 std::function<void()> changed;
private:
 social::Identity identity_;QString community_,directory_,address_;QUrl endpoint_;Rpc rpc_;Signer signer_;
 SocialAccessClient* own_{};QPointer<SocialAccessClient> native_;
 std::optional<social::AccessSnapshot> access_;QMap<QString,QString> owners_;QSet<QString> removed_,pinned_;
 QString channel_,nativeAddress_;QStringList targetPage_;QByteArray etag_;bool resolving_{},projecting_{};qint64 lastRead_{},lastProjection_{};int rotation_{},reading_{};
 SocialAccessClient* client()const{return native_?native_.data():own_;}
 SocialAccessClient* makeClient(QString script,QString address){
  QDir().mkpath(directory_);const QPointer<SocialAccessSession> weak(this);
  return new SocialAccessClient(identity_,{script,address,community_},endpoint_,directory_+"/pending-"+script+".json",[weak,script](QByteArray wire,auto done){if(weak)weak->signer_(script,std::move(wire),std::move(done));},this);
 }
 void notify(){if(changed)QTimer::singleShot(0,this,[this]{if(changed)changed();});}
 void refreshProjection(QString channel,QStringList targets,bool force=false){
  const auto now=QDateTime::currentMSecsSinceEpoch();if(projecting_||(!force&&now-lastProjection_<4500)||targets.isEmpty())return;
  projecting_=true;lastProjection_=now;if(channel_!=channel||targetPage_!=targets)etag_.clear();channel_=channel;targetPage_=targets;const QPointer<SocialAccessSession> weak(this);
  own_->projection(channel,targets,etag_,[weak,targets](QJsonObject value,QByteArray tag,QString error){if(!weak)return;weak->projecting_=false;if(!error.isEmpty())return;weak->etag_=tag;if(value.isEmpty())return;bool changed=false;
   for(const auto& id:targets){const bool removed=value["removed"].toArray().contains(id),pinned=value["pinned"].toArray().contains(id);changed|=removed!=weak->removed_.contains(id)||pinned!=weak->pinned_.contains(id);if(removed)weak->removed_.insert(id);else weak->removed_.remove(id);if(pinned)weak->pinned_.insert(id);else weak->pinned_.remove(id);}
   if(changed)weak->notify();
  });
 }
public:
 SocialAccessSession(social::Identity identity,QString socialAddress,QString community,QUrl endpoint,QString directory,Rpc rpc,Signer signer,QObject* parent=nullptr):QObject(parent),identity_(std::move(identity)),community_(std::move(community)),directory_(std::move(directory)),address_(std::move(socialAddress)),endpoint_(std::move(endpoint)),rpc_(std::move(rpc)),signer_(std::move(signer)){
  own_=makeClient(identity_.subject,address_);
 }
 void ensureNative(QString address){
  if(address.isEmpty()||address==nativeAddress_||resolving_)return;resolving_=true;const QPointer<SocialAccessSession> weak(this);
  rpc_("validateaddress",{address},[weak,address](QJsonValue value,QString error){if(!weak)return;weak->resolving_=false;const auto o=value.toObject();const auto script=o["scriptPubKey"].toString();if(!error.isEmpty()||o["isvalid"]!=true||!QRegularExpression("^0014[0-9a-f]{40}$").match(script).hasMatch())return;
   if(weak->native_)weak->native_->deleteLater();weak->native_=weak->makeClient(script,address);weak->nativeAddress_=address;weak->access_.reset();weak->lastRead_=0;weak->notify();
  });
 }
 void read(social::AccessReadRequest request,std::function<void(social::AccessSnapshot,QString)> done){
  if(request.identity!=identity_||reading_>=3){done({},"Community permissions are being refreshed.");return;}
  // Chat members are Social keys; role-editor members are verified community
  // account IDs. Never invent an association between the two key namespaces.
  if(!request.member.isEmpty()&&!QRegularExpression("^[0-9a-f]{64}$").match(request.member).hasMatch())request.member.clear();
  ++reading_;lastRead_=QDateTime::currentMSecsSinceEpoch();const QPointer<SocialAccessSession> weak(this);
  client()->read(request,[weak,done](social::AccessSnapshot snapshot,QString error){if(!weak)return;--weak->reading_;if(error.isEmpty())weak->access_=snapshot;done(std::move(snapshot),error);});
 }
 void manage(social::AccessRequest request,std::function<void(social::AccessResult)> done){
  const bool ownRemoval=request.action==social::AccessAction::RemoveContent&&owners_.value(request.target)==identity_.subject;
  const QPointer<SocialAccessSession> weak(this);(ownRemoval?own_:client())->manage(request,[weak,request,done](social::AccessResult result){if(!weak)return;
   if(result.outcome==social::AccessOutcome::Accepted){weak->access_.reset();weak->lastRead_=0;weak->lastProjection_=0;weak->etag_.clear();QStringList targets{request.target};weak->refreshProjection(request.channel,targets,true);weak->notify();}
   done(std::move(result));
  });
 }
 void decorate(social::Snapshot& snapshot){
  if(snapshot.identity!=identity_||snapshot.section!=social::Section::Chat)return;
  for(const auto& e:snapshot.entries)owners_.insert(e.id,e.authorId);
  while(owners_.size()>500){const auto id=owners_.firstKey();owners_.remove(id);removed_.remove(id);pinned_.remove(id);}
  // Rotate through retained IDs with a constant <=50-ID request budget.
  const auto keys=owners_.keys();QStringList targets;if(!keys.isEmpty()){rotation_%=keys.size();for(int i=0;i<qMin<qsizetype>(50,keys.size());++i)targets<<keys[(rotation_+i)%keys.size()];rotation_=(rotation_+targets.size())%keys.size();}
  refreshProjection(snapshot.channel,targets);
  const auto now=QDateTime::currentMSecsSinceEpoch();
  if(!reading_&&now-lastRead_>=60000){const QPointer<SocialAccessSession> weak(this);read({identity_,snapshot.channel,{},{},targets},[weak](auto,QString error){if(weak&&error.isEmpty())weak->notify();});}
  if(access_&&social::validateAccess(*access_,identity_).isEmpty())snapshot.access=access_;
  snapshot.removedContent=removed_.values();snapshot.pinnedContent=pinned_.values();
 }
};
}
