#pragma once
#include <QString>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
namespace pgn::wallet {
struct WalletSetupSelection {
    QString dataDirectory,walletName{"PGN"};
    bool existing{};
};
inline QString defaultCoreDataDirectory(const QString& genesis={}){
    QString base;
#ifdef Q_OS_WIN
    const auto roaming=qEnvironmentVariable("APPDATA");
    if(!roaming.isEmpty())base=QDir(roaming).filePath("PGNCore");
#endif
    if(base.isEmpty())base=QDir(QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation)).filePath("PGNCore");
    // A fresh network gets its own suggested node folder. Existing configured
    // profiles retain their selected folder and keys without migration.
    static const QRegularExpression fingerprint("^[0-9a-fA-F]{64}$");
    return fingerprint.match(genesis).hasMatch()?QDir(base).filePath("networks/"+genesis.toLower().left(16)):base;
}
// This validation is read-only. A dialog can be opened/cancelled without
// installing a node, creating directories or touching an existing wallet.
inline QString validateWalletLocation(const QString& dataDirectory,const QString& applicationDirectory){
    const WalletSetupSelection choice{dataDirectory,"PGN",false};
    static const QRegularExpression name("^[A-Za-z0-9_-]{1,64}$");
    if(!name.match(choice.walletName).hasMatch())return "Use 1–64 letters, digits, underscores or hyphens for the wallet name.";
    if(choice.dataDirectory.isEmpty()||!QFileInfo(choice.dataDirectory).isAbsolute())return "Choose an absolute data folder.";
    const auto target=QDir::cleanPath(QFileInfo(choice.dataDirectory).absoluteFilePath());
    if(QDir(target).isRoot()||target.compare(QDir::cleanPath(QDir::homePath()),Qt::CaseInsensitive)==0)return "Choose a dedicated PGN Core folder.";
    const auto application=QFileInfo(applicationDirectory).canonicalFilePath();
    QFileInfo ancestor(target);while(!ancestor.exists()){
        const QFileInfo parent(ancestor.absolutePath());if(parent.filePath()==ancestor.filePath())break;ancestor=parent;
    }
    const auto resolved=ancestor.canonicalFilePath();
    if(resolved.isEmpty())return "The selected folder's parent is unavailable.";
    const auto effective=QDir::cleanPath(resolved+"/"+QDir(ancestor.absoluteFilePath()).relativeFilePath(target));
    if(!application.isEmpty()&&(effective.compare(application,Qt::CaseInsensitive)==0||effective.startsWith(application+"/",Qt::CaseInsensitive)))return "Keep wallet data outside the Horizon application folder.";
    if(QFileInfo(target).exists()&&!QFileInfo(target).isDir())return "Choose a folder, not a wallet file.";
    return {};
}
inline QString validateWalletSetup(const WalletSetupSelection& choice,const QString& applicationDirectory){
    static const QRegularExpression name("^[A-Za-z0-9_-]{1,64}$");
    if(!name.match(choice.walletName).hasMatch())return "Use 1–64 letters, digits, underscores or hyphens for the wallet name.";
    const auto invalid=validateWalletLocation(choice.dataDirectory,applicationDirectory);if(!invalid.isEmpty())return invalid;
    const auto target=QDir::cleanPath(QFileInfo(choice.dataDirectory).absoluteFilePath());
    if(choice.existing){
        if(!QFileInfo(target).isDir())return "Choose the existing PGN Core data folder.";
        const QDir directory(target);
        if(directory.exists("wallet.dat")&&!directory.exists("blocks")&&!directory.exists("chainstate")&&!directory.exists("wallets")&&!directory.exists("testnet"))return "This is a wallet folder, not a Core data folder. Select the node's data folder, or create a new node in an empty folder first. Existing wallet files are unchanged.";
        if(!directory.exists("blocks")&&!directory.exists("chainstate")&&!directory.exists("wallets")&&!directory.exists("testnet"))return "This folder does not contain an existing Core data layout.";
    }else if(QFileInfo(target).isDir()&&!QDir(target).entryList(QDir::AllEntries|QDir::NoDotAndDotDot|QDir::Hidden).isEmpty())return "Use an empty folder for a new wallet, or choose Open existing.";
    return {};
}
}
