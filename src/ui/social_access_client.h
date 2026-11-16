#pragma once
#include "social/social_access_wire.h"
#include <QFile>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSaveFile>
#include <QUrl>
#include <QUrlQuery>
#include <QCryptographicHash>
#include <stdexcept>
#include <memory>

namespace pgn::ui {
namespace access_client {
inline QStringList accessStringList(const QJsonValue& value){
 if(!value.isArray()||value.toArray().size()>128)throw std::runtime_error("Invalid access list");
 QStringList out;for(const auto& x:value.toArray()){if(!x.isString())throw std::runtime_error("Invalid access text");out<<x.toString();}return out;
}
inline social::AccessSnapshot decode(const QJsonObject& v,const social::Identity& identity,const social::AccessNativeContext& native){
 using namespace social;
 if(v["identity"]!=identity.subject||v["genesis"]!=identity.genesis||v["community"]!=native.community||v["authenticated"]!=true)throw std::runtime_error("Access response belongs to another account or network");
 AccessSnapshot s;s.identity=identity;s.authenticated=true;s.community=native.community;s.revision=v["revision"].toString();s.source=v["source"].toString();s.observedUtc=QDateTime::fromMSecsSinceEpoch(v["observed_ms"].toInteger(),Qt::UTC);s.moderatorRoleId=v["moderator_role_id"].toString();s.permissionCatalog=accessStringList(v["permissions"]);s.grantablePermissions=accessStringList(v["grantable"]);
 if(!v["roles"].isArray()||v["roles"].toArray().size()>100||!v["members"].isArray()||v["members"].toArray().size()>200||!v["overwrites"].isArray()||v["overwrites"].toArray().size()>500||!v["availability"].isArray()||v["availability"].toArray().size()>1000)throw std::runtime_error("Access page exceeds its limits");
 for(const auto& x:v["roles"].toArray()){const auto o=x.toObject();s.roles<<AccessRole{o["id"].toString(),o["name"].toString(),o["position"].toInt(-1),accessStringList(o["permissions"]),o["managed"].toBool()};}
 for(const auto& x:v["members"].toArray()){const auto o=x.toObject();s.members<<AccessMember{o["id"].toString(),o["name"].toString(),accessStringList(o["roles"])};}
 for(const auto& x:v["overwrites"].toArray()){const auto o=x.toObject();s.overwrites<<ChannelOverwrite{o["channel"].toString(),o["target"].toString(),o["member"].toBool(),accessStringList(o["allow"]),accessStringList(o["deny"])};}
 const QStringList actions{"create_role","edit_role","delete_role","move_role","assign_role","remove_role","kick_member","ban_member","set_overwrite","remove_content","pin_content","react_content"};
 for(const auto& x:v["availability"].toArray()){const auto o=x.toObject();const auto at=actions.indexOf(o["action"].toString());if(at<0||!o["allowed"].isBool())throw std::runtime_error("Unknown access action");Availability a;a.action=static_cast<AccessAction>(at);a.target=o["target"].toString();a.role=o["role"].toString();a.channel=o["channel"].toString();a.allowed=o["allowed"].toBool();a.reason=o["reason"].toString();const QStringList scopes{"social","chat","publications","forum","miners"};const auto scope=scopes.indexOf(o["scope"].toString("social"));if(scope<0)throw std::runtime_error("Unknown access scope");a.scope=static_cast<AccessScope>(scope);a.parent=o["parent"].toString();a.reaction=o["reaction"].toString();s.availability<<a;}
 const auto a=v["session"].toObject();if(a["identity"]!=identity.subject||a["state"]!="verified"||a["device"]!=native.script||!a["devices"].isArray()||a["devices"].toArray().size()>20)throw std::runtime_error("The verified wallet session changed");
 s.session.identity=identity;s.session.state=AuthState::Verified;s.session.reference=a["reference"].toString();s.session.verificationRef=a["verification_ref"].toString();s.session.revocationRevision=a["revocation_revision"].toString();s.session.device=a["device"].toString();s.session.expiresUtc=QDateTime::fromMSecsSinceEpoch(a["expires_ms"].toInteger(),Qt::UTC);
 for(const auto& x:a["devices"].toArray()){const auto o=x.toObject();s.session.devices<<AuthDevice{o["id"].toString(),o["label"].toString(),o["revoked"].toBool()};}
 const auto error=validateAccess(s,identity);if(!error.isEmpty())throw std::runtime_error(error.toStdString());return s;
}
inline QJsonObject body(const QByteArray& wire){
 const QByteArray prefix="PGN/SOCIAL/ACCESS/1\n";if(!wire.startsWith(prefix)||wire.size()>16384)return {};
 QJsonParseError error;const auto d=QJsonDocument::fromJson(wire.mid(prefix.size()),&error);return error.error==QJsonParseError::NoError&&d.isObject()?d.object():QJsonObject{};
}
inline QByteArray intentDigest(const QByteArray& wire){
 auto b=body(wire);if(b.isEmpty())return {};b.remove("created_ms");b.remove("request_id");b.remove("revision");
 if(b["action"]=="create_role"){auto args=b["args"].toObject();args.remove("role");b["args"]=args;}
 return QCryptographicHash::hash(QJsonDocument(b).toJson(QJsonDocument::Compact),QCryptographicHash::Sha256).toHex();
}
// Exactly one unresolved write per wallet scope. A lost reply is retried with
// identical bytes, including after process restart and a new UI request UUID.
class Journal {
 QString path_;social::Identity identity_;social::AccessNativeContext native_;
public:
 Journal(QString path,social::Identity identity,social::AccessNativeContext native):path_(std::move(path)),identity_(std::move(identity)),native_(std::move(native)){}
 QString load(const QByteArray& requested,QByteArray& wire,QString& signature){
  QFile f(path_);if(!f.exists())return {};
  if(f.size()>32768||!f.open(QIODevice::ReadOnly))return "The previous action could not be read. Its outcome is retained.";
  QJsonParseError e;const auto d=QJsonDocument::fromJson(f.readAll(),&e).object();const auto saved=d["message"].toString().toUtf8();const auto b=body(saved);
  if(e.error!=QJsonParseError::NoError||d["schema"]!="PGN_SOCIAL_ACCESS_PENDING_1"||b["genesis"]!=identity_.genesis||b["client"]!=identity_.subject||b["actor"]!=native_.script||b["community"]!=native_.community||d["signature"].toString().isEmpty()||d["signature"].toString().size()>512)return "The previous action has an invalid account binding; it was preserved.";
  if(intentDigest(saved)!=intentDigest(requested))return "Another action is still awaiting confirmation. Retry that action first.";
  wire=saved;signature=d["signature"].toString();return {};
 }
 QString save(const QByteArray& wire,const QString& signature){
  if(wire.isEmpty()||wire.size()>16384||signature.isEmpty()||signature.size()>512)return "The signed action is invalid.";
  const auto raw=QJsonDocument(QJsonObject{{"schema","PGN_SOCIAL_ACCESS_PENDING_1"},{"message",QString::fromUtf8(wire)},{"signature",signature}}).toJson(QJsonDocument::Compact);QSaveFile f(path_);
  if(raw.size()>32768||!f.open(QIODevice::WriteOnly)||f.write(raw)!=raw.size()||!f.commit())return "The action could not be saved safely; it was not sent.";return {};
 }
 bool clear(){return !QFile::exists(path_)||QFile::remove(path_);}
};
}

class SocialAccessClient final:public QObject {
public:
 using Signer=std::function<void(QByteArray,std::function<void(QString,QString)>)>;
private:
 social::Identity identity_;social::AccessNativeContext native_;QUrl endpoint_;Signer signer_;access_client::Journal journal_;QNetworkAccessManager network_;int active_{},signing_{};bool writing_{};bool endpointValid_{};
 using Reply=std::function<void(QJsonObject,QString,bool)>;
 void post(QByteArray wire,QString signature,Reply done){
  if(!endpointValid_||active_>=4){done({},"Community connection unavailable or busy.",false);return;}
  ++active_;QNetworkRequest request(endpoint_);request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);request.setTransferTimeout(15000);
  auto* reply=network_.post(request,QJsonDocument(QJsonObject{{"message",QString::fromUtf8(wire)},{"signature",signature}}).toJson(QJsonDocument::Compact));const auto bytes=std::make_shared<QByteArray>();
  connect(reply,&QNetworkReply::readyRead,reply,[reply,bytes]{bytes->append(reply->readAll());if(bytes->size()>1048576)reply->abort();});
  connect(reply,&QNetworkReply::finished,this,[this,reply,bytes,done]{--active_;bytes->append(reply->readAll());const auto status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();QJsonParseError e;const auto d=QJsonDocument::fromJson(*bytes,&e);const auto value=d.object();const bool valid=bytes->size()<=1048576&&e.error==QJsonParseError::NoError&&d.isObject();
   reply->deleteLater();if(!valid||(status!=200&&status!=403&&status!=409)){done({},"The service did not confirm the action. Retrying will preserve it.",false);return;}
   if(status==200&&value["ok"]==true&&value["value"].isObject()){done(value["value"].toObject(),{},true);return;}
   if((status==403||status==409)&&value["ok"]==false){done(value,value["reason"].toString().left(300),true);return;}
   done({},"The service response is inconsistent; confirmation is still pending.",false);
  });
 }
public:
 SocialAccessClient(social::Identity identity,social::AccessNativeContext native,QUrl endpoint,QString journal,Signer signer,QObject* parent=nullptr,bool loopbackTest=false):QObject(parent),identity_(std::move(identity)),native_(std::move(native)),endpoint_(std::move(endpoint)),signer_(std::move(signer)),journal_(std::move(journal),identity_,native_),network_(this){
  endpointValid_=endpoint_.isValid()&&endpoint_.userInfo().isEmpty()&&endpoint_.query().isEmpty()&&endpoint_.fragment().isEmpty()&&(!endpoint_.host().isEmpty())&&(endpoint_.scheme()=="https"||(loopbackTest&&endpoint_.scheme()=="http"&&endpoint_.host()=="127.0.0.1"));
 }
 void projection(QString channel,QStringList targets,QByteArray etag,std::function<void(QJsonObject,QByteArray,QString)> done){
  if(!endpointValid_||active_>=4||targets.size()>50){done({}, {},"Community projection unavailable or busy.");return;}
  QSet<QString> seen;for(const auto& id:targets){if(!QRegularExpression("^[0-9a-f]{64}$").match(id).hasMatch()||seen.contains(id)){done({}, {},"Invalid projection target.");return;}seen.insert(id);}
  auto url=endpoint_;auto path=url.path();if(!path.endsWith("/access")){done({}, {},"Invalid projection endpoint.");return;}path.chop(6);url.setPath(path+"projection");QUrlQuery query;query.addQueryItem("community",native_.community);query.addQueryItem("channel",channel);query.addQueryItem("targets",targets.join(','));url.setQuery(query);
  ++active_;QNetworkRequest request(url);request.setTransferTimeout(10000);request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::ManualRedirectPolicy);if(!etag.isEmpty())request.setRawHeader("If-None-Match",etag);
  auto* reply=network_.get(request);const auto bytes=std::make_shared<QByteArray>();connect(reply,&QNetworkReply::readyRead,reply,[reply,bytes]{bytes->append(reply->readAll());if(bytes->size()>24576)reply->abort();});
  connect(reply,&QNetworkReply::finished,this,[this,reply,bytes,targets,channel,done]{--active_;bytes->append(reply->readAll());const auto status=reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();const auto tag=reply->rawHeader("ETag");reply->deleteLater();
   if(status==304){done({},tag,{});return;}QJsonParseError error;const auto doc=QJsonDocument::fromJson(*bytes,&error);const auto envelope=doc.object();const auto v=envelope["value"].toObject();
   if(status!=200||envelope["ok"]!=true||bytes->size()>24576||error.error!=QJsonParseError::NoError||v["schema"]!="PGN_SOCIAL_PROJECTION_1"||v["genesis"]!=identity_.genesis||v["community"]!=native_.community||v["channel"]!=channel||v["targets"]!=QJsonArray::fromStringList(targets)){done({}, {},"Community projection response could not be verified.");return;}
   try{for(const auto& key:{"removed","pinned"})for(const auto& id:access_client::accessStringList(v[key]))if(!targets.contains(id))throw std::runtime_error("Unexpected projection target");done(v,tag,{});}catch(...){done({}, {},"Invalid community projection.");}
  });
 }
 void read(social::AccessReadRequest request,std::function<void(social::AccessSnapshot,QString)> done){
  if(request.identity!=identity_||!signer_||!endpointValid_||active_+signing_>=4){done({},"The community account connection is unavailable or busy.");return;}
  const auto wire=social::accessReadWire(request,native_);if(wire.isEmpty()){done({},"The access request is invalid.");return;}const QPointer<SocialAccessClient> weak(this);
  ++signing_;const auto once=std::make_shared<bool>(false);
  signer_(wire,[weak,wire,done,once](QString signature,QString error){if(!weak||*once)return;*once=true;--weak->signing_;if(!error.isEmpty()||signature.isEmpty()){done({},error.isEmpty()?QString("The wallet did not confirm access."):error);return;}
   weak->post(wire,signature,[weak,done](QJsonObject value,QString error,bool){if(!weak)return;if(!error.isEmpty()){done({},error);return;}try{done(access_client::decode(value,weak->identity_,weak->native_),{});}catch(const std::exception& e){done({},QString::fromUtf8(e.what()));}});
  });
 }
 void manage(social::AccessRequest request,std::function<void(social::AccessResult)> done){
  using namespace social;AccessResult result{request.identity,request.community,request.channel,request.requestId,AccessOutcome::Unavailable,{},{}};
  if(request.identity!=identity_||request.community!=native_.community||writing_||!signer_||!endpointValid_||active_+signing_>=4){result.reason="The community account connection is unavailable or busy.";done(result);return;}
  auto wire=accessCommandWire(request,native_);if(wire.isEmpty()){result.reason="This action is invalid or unavailable.";done(result);return;}
  QString signature;const auto error=journal_.load(wire,wire,signature);if(!error.isEmpty()){result.reason=error;done(result);return;}writing_=true;const QPointer<SocialAccessClient> weak(this);
  auto send=[weak,wire,result,done](QString signature,QString error)mutable{if(!weak)return;if(!error.isEmpty()||signature.isEmpty()){weak->writing_=false;result.reason=error.isEmpty()?QString("The wallet did not sign this action."):error;done(result);return;}
   const auto saved=weak->journal_.save(wire,signature);if(!saved.isEmpty()){weak->writing_=false;result.reason=saved;done(result);return;}
   weak->post(wire,signature,[weak,wire,result,done](QJsonObject answer,QString error,bool terminal)mutable{if(!weak)return;weak->writing_=false;
    if(terminal&&error.isEmpty()){
     const auto b=access_client::body(wire);if(answer["community"]!=weak->native_.community||answer["request_id"]!=b["request_id"]||answer["identity"]!=weak->identity_.subject||answer["outcome"]!="accepted"||!answer["revision"].isDouble()){result.reason="The action confirmation belongs to another operation.";done(result);return;}
     result.outcome=AccessOutcome::Accepted;result.revision=QString::number(answer["revision"].toInteger());
    }else if(terminal){result.outcome=answer["error"]=="Conflict"?AccessOutcome::Conflict:AccessOutcome::Denied;result.reason=error;}
    else result.reason=error;
    if(terminal&&!weak->journal_.clear())result.reason="The result is confirmed; its saved retry record could not be retired.";done(result);
   });
  };
  if(!signature.isEmpty())send(signature,{});else{++signing_;const auto once=std::make_shared<bool>(false);signer_(wire,[weak,once,send](QString signature,QString error)mutable{if(!weak||*once)return;*once=true;--weak->signing_;send(signature,error);});}
 }
};
}
