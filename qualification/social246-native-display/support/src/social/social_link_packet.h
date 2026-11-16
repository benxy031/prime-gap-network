#pragma once
#include "social/social_data.h"
#include "social/social_signing_contract.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QCryptographicHash>
namespace pgn::social {
// Existing V2 certificate, not a new profile/permission certificate. Human
// display name is absent from signed data and never supplies identity/rights.
struct NameLinkPacket {
 QJsonObject certificate,statement;QString id,socialScript,nativeScript,operatorId,previous,action;
 QByteArray source,socialChallenge,nativeChallenge,transitionChallenge;
};
inline QString linkHash(const QByteArray& value){return QString::fromLatin1(QCryptographicHash::hash(value,QCryptographicHash::Sha256).toHex());}
inline bool linkObjectKeys(const QJsonObject& object,const QStringList& expected){auto actual=object.keys(),sorted=expected;std::sort(actual.begin(),actual.end());std::sort(sorted.begin(),sorted.end());return actual==sorted;}
inline QString decodeNameLinkPacket(const QByteArray& source,const QString& genesis,NameLinkPacket& out){
 if(!QRegularExpression("^[0-9a-f]{64}$").match(genesis).hasMatch())return "A verified network context is required.";
 if(source.size()>24576||source.isEmpty())return "The consent packet exceeds the existing Social envelope limit.";
 QJsonParseError error;const auto document=QJsonDocument::fromJson(source,&error);if(error.error!=QJsonParseError::NoError||!document.isObject())return "A canonical public consent packet is required.";
 const auto certificate=document.object();if(canonical(certificate)!=source||!linkObjectKeys(certificate,{"statement","social_proof","operator_proof","authorizer_script","authorization_proof"})||!certificate.value("statement").isObject())return "The consent packet has unexpected fields or encoding.";
 const auto statement=certificate.value("statement").toObject();
 if(!linkObjectKeys(statement,{"v","genesis","origin","scope","action","social_script","operator_script","operator_id","anchor_hash","anchor_height","previous_link","nonce"})||statement.value("v").toInt()!=2||statement.value("genesis").toString()!=genesis||statement.value("origin").toString()!="pgn-social://"+genesis||statement.value("scope").toString()!="display_link_for_script_history")return "The consent belongs to another version, network or purpose.";
 const auto hex=[](const QString& value,int length){return value.size()==length&&QRegularExpression("^[0-9a-f]+$").match(value).hasMatch();};
 const auto script=[&](const QString& value){return hex(value,44)&&value.startsWith("0014");};
 const auto s=statement.value("social_script").toString(),k=statement.value("operator_script").toString();const auto action=statement.value("action").toString();
 if(!script(s)||!script(k)||!hex(statement.value("operator_id").toString(),64)||!hex(statement.value("anchor_hash").toString(),64)||!hex(statement.value("previous_link").toString(),64)||!hex(statement.value("nonce").toString(),64)||!statement.value("anchor_height").isDouble()||statement.value("anchor_height").toInteger(-1)<0||statement.value("anchor_height").toInteger()>2147483647||statement.value("anchor_height").toDouble()!=double(statement.value("anchor_height").toInteger()))return "The consent has invalid public keys, head or Native anchor.";
 for(const auto& field:{"social_proof","operator_proof","authorization_proof"})if(!certificate.value(field).isString()||certificate.value(field).toString().size()>512||(!certificate.value(field).toString().isEmpty()&&!QRegularExpression("^[A-Za-z0-9+/]+={0,2}$").match(certificate.value(field).toString()).hasMatch()))return "A bounded Native BIP322 proof is required.";
 if(!certificate.value("authorizer_script").isString())return "The consent authorizer must be a public script or empty.";
 const auto authorizer=certificate.value("authorizer_script").toString();
 if(action=="bind"){if(!authorizer.isEmpty()||!certificate.value("authorization_proof").toString().isEmpty())return "Binding requires the two explicit consent domains.";}
 else if(action=="withdraw"){if((authorizer!=s&&authorizer!=k)||statement.value("previous_link").toString()==QString(64,'0')||!certificate.value("social_proof").toString().isEmpty()||!certificate.value("operator_proof").toString().isEmpty())return "Withdrawal requires the current participant and Native head.";}
 else return "This consent action is not supported.";
 NameLinkPacket packet;packet.certificate=certificate;packet.statement=statement;packet.source=source;packet.id=linkHash("PGN/SOCIAL/IDENTITY-LINK-ID/2\n"+canonical(statement));packet.socialScript=s;packet.nativeScript=k;packet.operatorId=statement.value("operator_id").toString();packet.previous=statement.value("previous_link").toString();packet.action=action;
 const auto challenge=[&](const QByteArray& side){return "PGN/SOCIAL/IDENTITY-LINK/"+side+"/2\n"+canonical(QJsonObject{{"event_id",packet.id},{"statement",statement}});};packet.socialChallenge=challenge("SOCIAL-CONSENT");packet.nativeChallenge=challenge("OPERATOR-CONSENT");packet.transitionChallenge=challenge("STATE-TRANSITION");out=std::move(packet);return {};
}
}
