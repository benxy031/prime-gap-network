#pragma once
#include "wallet/core_bundle.h"
#include "wallet/setup_deployment.h"
#include <QDirIterator>
namespace pgn::wallet {
struct ExistingWalletDiscovery {WalletSetupDeployment setup;QString notice;bool found{};};
// Bounded read-only probe of the current user's standard folder for this exact
// genesis. It never scans other users, creates a wallet or edits chain data.
inline ExistingWalletDiscovery discoverDefaultExistingWallet(const QJsonObject& bundle,const QString& applicationRoot){
 ExistingWalletDiscovery result;const auto genesis=bundle.value("genesis_hash").toString();const auto chain=bundle.value("chain").toString();
 if(!QRegularExpression("^[0-9a-fA-F]{64}$").match(genesis).hasMatch()||(chain!="test"&&chain!="main")){result.notice="The verified Core network is unavailable.";return result;}
 const auto data=defaultCoreDataDirectory(genesis);const auto network=data+(chain=="test"?"/testnet":QString{});const auto wallets=network+"/wallets";
 if(!QFileInfo(data).exists())return result;
 for(const auto& path:{data,network,wallets})if(QFileInfo(path).isSymLink()){result.notice="An existing wallet folder uses a redirected path. Choose the folder to reopen explicitly.";return result;}
 if(!validateWalletLocation(data,applicationRoot).isEmpty())return result;
 QString selected;int visited=0;
 QDirIterator entries(wallets,QDir::Dirs|QDir::Files|QDir::NoDotAndDotDot);
 while(entries.hasNext()){
  entries.next();if(++visited>64){result.notice="The existing wallet folder needs an explicit selection.";return result;}
  const auto entry=entries.fileInfo();if(entry.isSymLink()){result.notice="The existing wallet folder contains redirected entries; choose a wallet explicitly.";return result;}
  const auto path=entry.isDir()?entry.filePath()+"/wallet.dat":entry.filePath();const QFileInfo file(path);
  if(!file.isFile())continue;
  if(file.isSymLink()){result.notice="The existing wallet file uses a redirected path; choose it explicitly.";return result;}
  QFile input(path);if(!input.open(QIODevice::ReadOnly)){result.notice="An existing wallet cannot be read. Its files are preserved; select the wallet explicitly.";return result;}const auto header=input.read(16);
  if(header!=QByteArray("SQLite format 3\0",16)&&header.mid(12,4)!=QByteArray::fromHex("62310500")){if(entry.isDir()){result.notice="An existing wallet needs verification. No replacement wallet was created.";return result;}continue;}
  const auto name=entry.fileName();if(!QRegularExpression("^[A-Za-z0-9_-]{1,64}$").match(name).hasMatch()){result.notice="An existing wallet needs an explicit name selection.";return result;}
  if(!selected.isEmpty()){result.notice="Several existing wallets were found. Choose the wallet to reopen.";return result;}selected=name;
 }
 if(selected.isEmpty())return result;
 result.setup=selectedWalletDeployment(bundle,{data,selected,true},applicationRoot);
 if(!result.setup.error.isEmpty()){result.notice=result.setup.error;return result;}
 result.setup.profile.insert("data_directory_origin","default_existing_wallet");result.setup.profile.insert("create_wallet",false);
 result.found=true;result.notice="Existing wallet found. Connecting to PGN Core…";return result;
}
inline ExistingWalletDiscovery prepareDefaultExistingWallet(const QString& applicationRoot,const QString& chain){
 const auto bundle=readCoreBundle(applicationRoot,chain);if(!bundle.error.isEmpty())return {{},bundle.error,false};
 return discoverDefaultExistingWallet(bundle.deployment,applicationRoot);
}
}
