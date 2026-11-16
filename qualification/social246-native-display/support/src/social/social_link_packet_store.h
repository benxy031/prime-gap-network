#pragma once
#include "social/social_link_packet.h"
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QDir>
namespace pgn::social {
// Atomic local recovery of PUBLIC consent bytes. No secret, RPC credential,
// trusted phase, verified boolean, role grant or automatic signature is stored.
class NameLinkPacketStore {
 QString path_;Identity context_;
public:
 NameLinkPacketStore(QString path,Identity context):path_(std::move(path)),context_(std::move(context)){}
 QString save(const QByteArray& packet){
  NameLinkPacket parsed;const auto invalid=decodeNameLinkPacket(packet,context_.genesis,parsed);if(!invalid.isEmpty())return invalid;
  const auto source=canonical(QJsonObject{{"schema",1},{"genesis",context_.genesis},{"session_script",context_.subject},{"certificate",QString::fromUtf8(packet)}});
  if(source.size()>32768||!QDir(QFileInfo(path_).absolutePath()).mkpath("."))return "The public consent packet could not be saved.";
  QSaveFile file(path_);file.setDirectWriteFallback(false);if(!file.open(QIODevice::WriteOnly))return "The existing public consent packet is preserved.";
  file.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner);if(file.write(source)!=source.size()||!file.commit())return "Atomic consent-packet save did not commit. Keep the exported packet.";return {};
 }
 QString load(QByteArray& packet){
  QFile file(path_);if(!file.exists())return {};if(!file.open(QIODevice::ReadOnly)||file.size()>32768)return "Existing consent-packet file is unreadable and preserved.";
  const auto source=file.readAll();QJsonParseError error;const auto root=QJsonDocument::fromJson(source,&error).object();
  if(error.error!=QJsonParseError::NoError||root.value("schema").toInt()!=1||root.value("genesis").toString()!=context_.genesis||root.value("session_script").toString()!=context_.subject||!root.value("certificate").isString())return "Consent-packet recovery context changed. Existing file preserved.";
  const auto candidate=root.value("certificate").toString().toUtf8();NameLinkPacket parsed;const auto invalid=decodeNameLinkPacket(candidate,context_.genesis,parsed);if(!invalid.isEmpty())return invalid;packet=candidate;return {};
 }
};
}
