#include "wallet/installed_core_binding.h"
#include "wallet/setup_selection.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QJsonArray>
#include <iostream>
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);QTemporaryDir t;if(!t.isValid())return 1;
 const auto root=t.filePath("app"),profile=t.filePath("config/wallet-core.json");QDir().mkpath(root+"/Core");QDir().mkpath(t.filePath("config"));
 const QByteArray bytes("INERT CORE NOT EXECUTED");QFile image(root+"/Core/pgn-core.exe");if(!image.open(QIODevice::WriteOnly))return 2;image.write(bytes);image.close();image.setPermissions(QFileDevice::ReadOwner|QFileDevice::WriteOwner|QFileDevice::ExeOwner);
 const auto hash=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
 QJsonObject deployment{{"product","PGN Core"},{"chain","test"},{"frontend","daemon"},{"executable","pgn-core.exe"},{"executable_sha256",hash},{"genesis_hash",QString(64,'a')},{"rpc_port",48972},{"peer_port",48973}};
 QFile manifest(root+"/Core/core-template.json");if(!manifest.open(QIODevice::WriteOnly))return 3;manifest.write(QJsonDocument(QJsonObject{{"schema","PGN_CORE_BUNDLE_V1"},{"deployment",deployment},{"files",QJsonArray{QJsonObject{{"path","pgn-core.exe"},{"bytes",bytes.size()},{"sha256",hash}}}}}).toJson());manifest.close();
 if(pgn::wallet::installedCoreBinding(root,profile,true).program.isEmpty()||QFileInfo::exists(profile))return 4;
 const auto oldDefault=pgn::wallet::defaultCoreDataDirectory();const auto fresh=pgn::wallet::defaultCoreDataDirectory(QString(64,'a'));
 if(fresh==oldDefault||!fresh.endsWith("networks/aaaaaaaaaaaaaaaa")||QFileInfo::exists(fresh))return 13;
 if(pgn::wallet::defaultCoreDataDirectory("invalid")!=oldDefault)return 14;
 if(!pgn::wallet::installedCoreBinding(root,profile,false).program.isEmpty())return 5;
 QFile explicitProfile(profile);if(!explicitProfile.open(QIODevice::WriteOnly))return 6;explicitProfile.write("{bad json");explicitProfile.close();
 if(!pgn::wallet::installedCoreBinding(root,profile,true).program.isEmpty())return 7;
 explicitProfile.open(QIODevice::WriteOnly);explicitProfile.write(QJsonDocument(QJsonObject{{"executable",root+"/missing.exe"}}).toJson());explicitProfile.close();
 if(!pgn::wallet::installedCoreBinding(root,profile,true).program.isEmpty())return 8;
 explicitProfile.remove();image.remove();if(!pgn::wallet::installedCoreBinding(root,profile,true).program.isEmpty())return 9;
 const auto wallet=t.filePath("old-node/testnet/wallets/Rob");QDir().mkpath(wallet);QFile key(wallet+"/wallet.dat");key.open(QIODevice::WriteOnly);key.write("PRESERVE");key.close();
 const auto error=pgn::wallet::validateWalletSetup({wallet,"Rob",true},root);if(!error.contains("wallet folder")||!error.contains("unchanged"))return 10;
 if(pgn::wallet::validateWalletSetup({wallet,"Rob",false},root).isEmpty())return 11;
 key.open(QIODevice::ReadOnly);if(key.readAll()!="PRESERVE")return 12;
 std::cout<<"PASS unconfigured bundled Core inventory; network mismatch, invalid explicit profile, missing executable fail closed; wallet-subfolder diagnosis preserves keys\n";
}
