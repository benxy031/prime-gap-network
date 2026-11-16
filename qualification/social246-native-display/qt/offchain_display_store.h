#pragma once
#include "social/social_link_packet_store.h"
#include <QMap>
#include <QSet>
namespace pgn::social {
// Isolated current-display projection. NOT a full historical V2 validator,
// permission registry, new wire format or public admission policy.
struct DisplayProjection {
 NameLinkPacket packet;QString label,anchor;int height{-1};
};
struct DisplayHead {QString state{"unlinked"},label,head,operatorId,nativeScript;};
class OffchainDisplayStore {
 QString directory_,genesis_;int perScriptLimit_; // explicit TEST policy, no production default
 QMap<QString,QMap<QString,DisplayProjection>> verified_;
 QSet<QString> reset_;
 QString save(QString script,const QMap<QString,DisplayProjection>& records,bool reset){
  QJsonArray packets;for(const auto& item:records)packets.append(QString::fromUtf8(item.packet.source));
  const auto bytes=canonical(QJsonObject{{"schema","PGN_PRIVATE_CURRENT_DISPLAY_CACHE_1"},{"genesis",genesis_},{"social_script",script},{"retention_reset",reset},{"certificates",packets}});
  if(bytes.size()>perScriptLimit_*24576+4096||!QDir(directory_).mkpath("."))return "Bounded public-proof storage unavailable.";
  QSaveFile file(directory_+"/"+script+".json");file.setDirectWriteFallback(false);
  if(!file.open(QIODevice::WriteOnly)||file.write(bytes)!=bytes.size()||!file.commit())return "Atomic public-proof commit failed; prior display state preserved.";
  return {};
 }
public:
 OffchainDisplayStore(QString directory,QString genesis,int perScriptLimit):directory_(std::move(directory)),genesis_(std::move(genesis)),perScriptLimit_(perScriptLimit){Q_ASSERT(perScriptLimit>=4&&perScriptLimit<=64);}
 QStringList persistedScripts()const{QStringList out;for(const auto& name:QDir(directory_).entryList({"0014*.json"},QDir::Files)){const auto s=name.chopped(5);if(QRegularExpression("^0014[0-9a-f]{40}$").match(s).hasMatch())out.append(s);}return out;}
 QString pending(QString script,QVector<QByteArray>& packets){
  QFile file(directory_+"/"+script+".json");if(!file.exists())return {};
  if(!file.open(QIODevice::ReadOnly)||file.size()>perScriptLimit_*24576+4096)return "Public display cache unreadable; fallback retained.";
  QJsonParseError e;const auto root=QJsonDocument::fromJson(file.readAll(),&e).object();
  if(e.error!=QJsonParseError::NoError||root["schema"]!="PGN_PRIVATE_CURRENT_DISPLAY_CACHE_1"||root["genesis"]!=genesis_||root["social_script"]!=script||!root["retention_reset"].isBool()||!root["certificates"].isArray()||root["certificates"].toArray().size()>perScriptLimit_)return "Public display cache context invalid; file preserved.";
  if(root["retention_reset"].toBool())reset_.insert(script);
  for(const auto& raw:root["certificates"].toArray()){NameLinkPacket p;if(!raw.isString()||!decodeNameLinkPacket(raw.toString().toUtf8(),genesis_,p).isEmpty()||p.socialScript!=script)return "Stored certificate invalid; no display authority loaded.";packets.append(p.source);}
  return {};
 }
 void clearVerified(){verified_.clear();} // reload never trusts a persisted bool/name
 void invalidateScript(const QString& script){verified_.remove(script);}
 QString accept(const DisplayProjection& proof,bool persist=true){
  if(proof.packet.statement["genesis"]!=genesis_||proof.anchor.size()!=64||proof.height<0||!publicLabel(proof.label))return "Verified current Native projection required.";
  const auto script=proof.packet.socialScript;auto candidate=verified_.value(script);
  if(candidate.contains(proof.packet.id)){verified_[script][proof.packet.id]=proof;return {};} // refresh current label/context, no append
  const auto& cert=proof.packet.certificate;
  if(proof.packet.action=="bind"&&(cert["social_proof"].toString().isEmpty()||cert["operator_proof"].toString().isEmpty()))return "Both verified consent proofs are required before display admission.";
  if(proof.packet.action=="withdraw"){
   if(cert["authorization_proof"].toString().isEmpty())return "Verified withdrawal consent is required.";
   if(!candidate.contains(proof.packet.previous))return "MissingParent: preserve the public withdrawal packet; supply its binding proof before applying it.";
   const auto& parent=candidate[proof.packet.previous].packet;
   if(parent.socialScript!=script||parent.nativeScript!=proof.packet.nativeScript||parent.operatorId!=proof.packet.operatorId)return "Withdrawal cannot borrow another Native identity's binding.";
  }
  if(reset_.contains(script))return "RetentionReset affects this Social identity only; no name is asserted.";
  if(candidate.size()>=perScriptLimit_){
   // Do not invent a timeless revocation floor for V2 (no signed sequence).
   // Test saturation fails closed PER S, never as a global coverage hold.
   const auto error=persist?save(script,candidate,true):QString{};if(!error.isEmpty())return error;reset_.insert(script);return "RetentionReset affects this Social identity only.";
  }
  candidate.insert(proof.packet.id,proof);
  const auto error=persist?save(script,candidate,false):QString{};if(!error.isEmpty())return error;verified_.insert(script,std::move(candidate));return {};
 }
 DisplayHead head(QString script)const{
  if(reset_.contains(script))return {"retention_reset"};
  const auto records=verified_.value(script);if(records.isEmpty())return {};
  QMap<QString,QStringList> children;QStringList roots;
  // A K-only withdrawal with an unavailable parent supplies no authority over
  // S and must not freeze another user's existing valid name.
  for(const auto& value:records){
   const auto& p=value.packet;
   if(p.previous==QString(64,'0')){if(p.action=="bind")roots.append(p.id);continue;}
   if(!records.contains(p.previous))continue;
   const auto& parent=records.value(p.previous).packet;
   if(p.action=="withdraw"&&(p.nativeScript!=parent.nativeScript||p.operatorId!=parent.operatorId))continue;
   children[p.previous].append(p.id);
  }
  if(roots.size()>1)return {"conflict"};
  if(roots.isEmpty())return {"pending"};
  auto id=roots.front();QSet<QString> visited;
  while(true){
   if(visited.contains(id))return {"conflict"};visited.insert(id);
   const auto descendants=children.value(id);
   if(descendants.size()>1)return {"conflict"};
   if(descendants.isEmpty())break;id=descendants.front();
  }
  // A consent signed by S with a missing causal parent is unresolved, not an
  // arbitrary winner over a competing known chain.
  for(const auto& value:records)if(value.packet.action=="bind"&&value.packet.previous!=QString(64,'0')&&!records.contains(value.packet.previous))return {"pending"};
  const auto& at=records.value(id);if(at.packet.action=="withdraw")return {"withdrawn",{},id,at.packet.operatorId,at.packet.nativeScript};
  return {"linked",at.label,id,at.packet.operatorId,at.packet.nativeScript};
 }
 QString display(const Identity& identity,const QString& script)const{
  if(identity.genesis!=genesis_)return {};const auto resolved=head(script);
  return resolved.state=="linked"?publicIdentityDisplayName(script,resolved.label):QString{};
 }
};
}

