#pragma once
#include "social/social_access.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QUuid>
namespace pgn::social {
struct AccessNativeContext {QString script,address,community;};
inline QString accessActionName(AccessAction a){
 switch(a){case AccessAction::CreateRole:return "create_role";case AccessAction::EditRole:return "edit_role";case AccessAction::DeleteRole:return "delete_role";case AccessAction::MoveRole:return "move_role";case AccessAction::AssignRole:return "assign_role";case AccessAction::RemoveRole:return "remove_role";case AccessAction::KickMember:return "kick_member";case AccessAction::BanMember:return "ban_member";case AccessAction::SetOverwrite:return "set_overwrite";case AccessAction::RemoveContent:return "remove_content";case AccessAction::PinContent:return "pin_content";case AccessAction::ReactContent:return "react_content";}return {};
}
inline QByteArray accessWire(const Identity& identity,const AccessNativeContext& native,QString id,qint64 revision,QString action,QJsonObject args){
 const QRegularExpression script("^0014[0-9a-f]{40}$"),hex("^[0-9a-f]{64}$");
 if(!identity.valid()||!script.match(identity.subject).hasMatch()||!script.match(native.script).hasMatch()||!hex.match(native.community).hasMatch()||!QRegularExpression("^[A-Za-z0-9]{8,128}$").match(native.address).hasMatch()||id!=QUuid(id).toString(QUuid::WithoutBraces)||revision<0||revision>9007199254740991LL)return {};
 const QJsonObject body{{"v",1},{"genesis",identity.genesis},{"community",native.community},{"request_id",id},{"actor",native.script},{"client",identity.subject},{"address",native.address},{"created_ms",QDateTime::currentMSecsSinceEpoch()},{"revision",revision},{"action",action},{"args",args}};
 const auto wire=QByteArray("PGN/SOCIAL/ACCESS/1\n")+QJsonDocument(body).toJson(QJsonDocument::Compact);return wire.size()<=16384?wire:QByteArray{};
}
inline QByteArray accessReadWire(const AccessReadRequest& r,const AccessNativeContext& native){
 return accessWire(r.identity,native,QUuid::createUuid().toString(QUuid::WithoutBraces),0,"read_access",{{"channel",r.channel},{"member",r.member},{"role",r.role}});
}
inline QByteArray accessCommandWire(const AccessRequest& r,const AccessNativeContext& native){
 bool valid{};const auto revision=r.revision.toLongLong(&valid);if(!valid||r.community!=native.community)return {};
 QJsonObject args;
 switch(r.action){
 case AccessAction::CreateRole:case AccessAction::EditRole:args={{"role",r.action==AccessAction::CreateRole?"role-"+r.requestId:r.role},{"name",r.name},{"position",r.position},{"permissions",QJsonArray::fromStringList(r.permissions)}};break;
 case AccessAction::DeleteRole:args={{"role",r.role}};break;
 case AccessAction::MoveRole:args={{"role",r.role},{"position",r.position}};break;
 case AccessAction::AssignRole:case AccessAction::RemoveRole:args={{"target",r.target},{"role",r.role}};break;
 case AccessAction::KickMember:case AccessAction::BanMember:args={{"target",r.target}};break;
 case AccessAction::SetOverwrite:args={{"target",r.target},{"channel",r.channel},{"member",r.role.isEmpty()},{"allow",QJsonArray::fromStringList(r.allow)},{"deny",QJsonArray::fromStringList(r.deny)}};break;
 case AccessAction::RemoveContent:case AccessAction::PinContent:args={{"target",r.target},{"channel",r.channel}};break;
 case AccessAction::ReactContent:return {}; // Needs explicit selected-state contract before enabling.
 }
 return accessWire(r.identity,native,r.requestId,revision,accessActionName(r.action),args);
}
}
