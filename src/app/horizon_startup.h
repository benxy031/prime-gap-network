#pragma once
#include "wallet/core_bundle_template.h"
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include <optional>

namespace pgn::app {
struct HorizonStartup {
    bool testnet{true};
    bool developerLegacy{};
    QString profileState;
    QString diagnostic;
};

// Structural code-layout recognition only, not publisher authentication or a
// substitute for CoreBundle's executable hashes/network validation. Never use
// data/runtime receipts or wallet profiles to infer the application root.
inline bool hasHorizonCoreCodeMarker(const QString& directory) {
    const QFileInfo info(QDir(directory).filePath("Core/core-template.json"));
    if(!info.isFile()||info.isSymLink()||info.size()<=0||info.size()>wallet::CoreBundleTemplateLimit)return false;
    const auto bytes=wallet::readCoreBundleTemplate(directory);
    if(!bytes)return false;
    const auto deployment=wallet::parseCoreBundleTemplate(*bytes).value("deployment").toObject();
    return deployment.value("product")=="PGN Core"&&
        (deployment.value("chain")=="test"||deployment.value("chain")=="main");
}
inline QString resolveHorizonApplicationRoot(const QStringList& arguments,const QString& applicationDirectory) {
    if(arguments.size()>1&&!arguments[1].startsWith("--"))return arguments[1];
    const QDir application(applicationDirectory);
    // A nearest declaration, including a broken one, must keep its diagnostic.
    if(QFileInfo::exists(application.filePath("Core/core-template.json")))return application.absolutePath();
    const auto parent=QDir::cleanPath(application.filePath(".."));
    if(application.dirName().compare("bin",Qt::CaseInsensitive)==0&&hasHorizonCoreCodeMarker(parent))return parent;
    return application.absolutePath();
}

// This Horizon release is a testnet application. Missing installation metadata
// selects normal wallet setup, never a prototype backend or historical writer.
// This declaration does not replace CoreBundle's file/network verification.
inline HorizonStartup resolveHorizonStartup(const QStringList& arguments,
        const std::optional<QByteArray>& coreTemplate) {
    HorizonStartup result;
    if(arguments.contains("--developer-legacy-prototype")&&!arguments.contains("--testnet")) {
        result.testnet=false;result.developerLegacy=true;result.profileState="explicit-developer-legacy";
        return result;
    }
    if(!coreTemplate) {
        result.profileState="setup-required";
        result.diagnostic="Horizon is running on the test network.";
        return result;
    }
    const auto deployment=wallet::parseCoreBundleTemplate(*coreTemplate).value("deployment").toObject();
    const auto chain=deployment.value("chain");
    if(chain!="test"&&chain!="main") {
        result.profileState="invalid-core-template";
        result.diagnostic="The PGN Core bundle configuration is invalid. Use wallet setup to select a valid testnet Core bundle.";
    } else if(chain=="main") {
        result.profileState="incompatible-mainnet-template";
        result.diagnostic="This Horizon release uses testnet. The Core bundle is for mainnet; select a testnet Core bundle in wallet setup.";
    } else result.profileState="declared-testnet";
    return result;
}
}
