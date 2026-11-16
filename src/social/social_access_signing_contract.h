#pragma once
#include "social/social_access_wire.h"
#include <cmath>
namespace pgn::social {
// Signing scope only. The server separately recomputes authorization and
// verifies the Native key-to-account binding for each immutable command.
inline QString accessSigningError(const QByteArray& wire,const Identity& identity,const QString& script,const QString& community){
 const QByteArray domain="PGN/SOCIAL/ACCESS/1\n";
 if(!identity.valid()||wire.size()>16384||!wire.startsWith(domain))return "A bounded Social access command is required.";
 QJsonParseError error;const auto document=QJsonDocument::fromJson(wire.mid(domain.size()),&error);
 if(error.error!=QJsonParseError::NoError||!document.isObject()||document.toJson(QJsonDocument::Compact)!=wire.mid(domain.size()))return "The Social access command is not canonical.";
 const auto b=document.object();QSet<QString> keys;for(auto i=b.begin();i!=b.end();++i)keys.insert(i.key());
 if(keys!=QSet<QString>{"v","genesis","community","request_id","actor","client","address","created_ms","revision","action","args"})return "Unexpected Social access fields.";
 const QRegularExpression key("^0014[0-9a-f]{40}$"),hex("^[0-9a-f]{64}$");
 if(!key.match(identity.subject).hasMatch()||!key.match(script).hasMatch()||!hex.match(community).hasMatch()||b["v"]!=1||b["genesis"]!=identity.genesis||b["actor"]!=script||b["client"]!=identity.subject||b["community"]!=community)return "The command belongs to another Social account or network.";
 const auto id=b["request_id"].toString();if(id!=QUuid(id).toString(QUuid::WithoutBraces)||!b["address"].isString()||!QRegularExpression("^[A-Za-z0-9]{8,128}$").match(b["address"].toString()).hasMatch()||!b["args"].isObject())return "Invalid access operation identity.";
 const auto integral=[](QJsonValue v){return v.isDouble()&&std::isfinite(v.toDouble())&&v.toDouble()>=0&&v.toDouble()<=9007199254740991.&&std::floor(v.toDouble())==v.toDouble();};
 if(!integral(b["revision"])||!integral(b["created_ms"]))return "Invalid access revision or clock.";
 const auto now=QDateTime::currentMSecsSinceEpoch(),created=b["created_ms"].toInteger();if(created<now-120000||created>now+60000)return "The access confirmation expired; refresh first.";
 const auto action=b["action"].toString();bool known=action=="read_access";
 for(const auto a:{AccessAction::CreateRole,AccessAction::EditRole,AccessAction::DeleteRole,AccessAction::MoveRole,AccessAction::AssignRole,AccessAction::RemoveRole,AccessAction::SetOverwrite,AccessAction::RemoveContent,AccessAction::PinContent})known|=action==accessActionName(a);
 if(!known)return "This access action is not enabled.";
 return {};
}
}
