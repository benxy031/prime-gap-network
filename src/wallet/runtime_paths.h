#pragma once
#include "wallet/core_bundle_template.h"
#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
namespace pgn::wallet {
inline QString coreTemplateGenesis(const QByteArray& bytes,bool testnet){
    const auto deployment=parseCoreBundleTemplate(bytes).value("deployment").toObject();
    if(deployment.value("chain")!=(testnet?"test":"main"))return {};
    const auto genesis=deployment.value("genesis_hash").toString().toLower();
    return QRegularExpression("^[0-9a-f]{64}$").match(genesis).hasMatch()?genesis:QString{};
}
inline QString installedCoreGenesis(const QString& applicationRoot,bool testnet){
    const auto bytes=readCoreBundleTemplate(applicationRoot);
    return bytes?coreTemplateGenesis(*bytes,testnet):QString{};
}
inline bool runtimeProfileMatches(const QString& directory,const QString& genesis,bool testnet){
    QFile file(directory+"/wallet-core.json");
    if(!file.open(QIODevice::ReadOnly)||file.size()>32768)return false;
    const auto object=QJsonDocument::fromJson(file.readAll()).object();
    return object.value("product")=="PGN Core"&&object.value("chain")==(testnet?"test":"main")&&
        (genesis.isEmpty()||object.value("genesis_hash").toString().compare(genesis,Qt::CaseInsensitive)==0);
}
inline QString coreRuntimeDirectory(const QString& applicationRoot,bool testnet){
    // An existing development deployment is kept exactly where its ownership
    // receipt points. Fresh installations use the OS user configuration path.
    const auto legacy=applicationRoot+(testnet?"/data/testnet/ui-runtime":"/data/runtime");
    const auto genesis=installedCoreGenesis(applicationRoot,testnet);
    if(runtimeProfileMatches(legacy,genesis,testnet))return legacy;
    const auto family=QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath(testnet?"wallet/testnet":"wallet/main");
    // Keep an explicitly matching existing installation. A different genesis
    // gets a new profile; its older wallet/configuration is never overwritten.
    if(runtimeProfileMatches(family,genesis,testnet))return family;
    return genesis.isEmpty()?family:QDir(family).filePath(genesis);
}
inline QString normalEngineStateDirectory(const QString& applicationRoot,const QString& genesis){
    if(!QRegularExpression("^[0-9a-f]{64}$").match(genesis).hasMatch()||genesis==QString(64,'0'))return {};
    const auto legacy=applicationRoot+"/data/testnet/local-engine";
    if(QFileInfo(legacy).isDir()&&runtimeProfileMatches(applicationRoot+"/data/testnet/ui-runtime",genesis,true))return legacy;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath("engine/"+genesis);
}
}
