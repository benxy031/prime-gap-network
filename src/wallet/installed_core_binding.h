#pragma once
#include "wallet/core_bundle.h"
#include <QCryptographicHash>
namespace pgn::wallet {
struct InstalledCoreProfile {QByteArray bytes,identity;};
inline InstalledCoreProfile readInstalledCoreProfile(const QString& path){
 QFile profile(path);
 if(!profile.exists())return {{},"absent"};
 if(!profile.open(QIODevice::ReadOnly))return {{},"unreadable"};
 if(profile.size()>32768)return {{},"oversized"};
 auto bytes=profile.read(32769);
 if(profile.error()!=QFileDevice::NoError)return {{},"unreadable"};
 if(bytes.size()>32768)return {{},"oversized"};
 return {bytes,QCryptographicHash::hash(bytes,QCryptographicHash::Sha256)};
}
struct InstalledCoreBinding {QString program,genesis;};
// Code inventory does not require a wallet. An explicit existing profile still
// takes precedence: never hide its failure by silently selecting another Core.
inline InstalledCoreBinding installedCoreBinding(const QString& applicationRoot,const InstalledCoreProfile& profile,bool testnet){
 if(profile.identity!="absent"){
  QJsonParseError error;const auto document=QJsonDocument::fromJson(profile.bytes,&error);
  if(error.error!=QJsonParseError::NoError||!document.isObject())return {};
  const auto object=document.object();const QFileInfo program(object.value("executable").toString());
  if(!program.isAbsolute()||!program.isFile()||!program.isExecutable())return {};
  return {program.canonicalFilePath(),object.value("genesis_hash").toString()};
 }
 const auto bundle=readCoreBundle(applicationRoot,testnet?QString("test"):QString("main"));
 if(!bundle.error.isEmpty())return {};
 return {bundle.deployment.value("executable").toString(),bundle.deployment.value("genesis_hash").toString()};
}
inline InstalledCoreBinding installedCoreBinding(const QString& applicationRoot,const QString& profilePath,bool testnet){
 return installedCoreBinding(applicationRoot,readInstalledCoreProfile(profilePath),testnet);
}
}
