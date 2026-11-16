#include "wallet/existing_wallet_discovery.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>
using namespace pgn::wallet;
static int checks{};
static void need(bool ok,const char* label){if(!ok){std::cerr<<"FAIL "<<label<<'\n';std::exit(1);}++checks;}
static void put(const QString& path,QByteArray bytes){QDir().mkpath(QFileInfo(path).absolutePath());QFile f(path);need(f.open(QIODevice::WriteOnly),"fixture file");need(f.write(bytes)==bytes.size(),"fixture write");}
int main(int argc,char** argv){QCoreApplication app(argc,argv);QTemporaryDir temp;need(temp.isValid(),"private root");
 qputenv("APPDATA",(temp.path()+"/roaming").toUtf8());qputenv("XDG_DATA_HOME",(temp.path()+"/xdg").toUtf8());
 const auto root=temp.path()+"/PGN",exe=root+"/Core/pgn-core",genesis=QString(64,'a');put(exe,"INERT CORE NEVER EXECUTED");
 const QJsonObject bundle{{"product","PGN Core"},{"chain","test"},{"frontend","daemon"},{"executable",exe},{"executable_sha256",QString(64,'b')},{"genesis_hash",genesis},{"rpc_port",30101},{"peer_port",30102},{"peers",QJsonArray{}}};
 const auto data=defaultCoreDataDirectory(genesis),wallet=data+"/testnet/wallets/Any_User-7/wallet.dat";auto result=discoverDefaultExistingWallet(bundle,root);
 need(!result.found&&!QFileInfo::exists(data),"absent wallet stays absent without creating directories");
 const QByteArray bytes=QByteArray("SQLite format 3\0",16)+"INERT WALLET FIXTURE";put(wallet,bytes);
 result=discoverDefaultExistingWallet(bundle,root);need(result.found&&result.setup.error.isEmpty(),"single existing standard wallet found");auto profile=CoreProfile::fromJson(result.setup.profile);
 need(profile.walletName=="Any_User-7"&&profile.dataDirectory==data,"actual discovered name and generic user directory retained");need(profile.userSelectedDirectory&&!profile.createWallet&&profile.validate(root).isEmpty(),"existing wallet deployment never permits creation");
 QFile input(wallet);need(input.open(QIODevice::ReadOnly)&&input.readAll()==bytes,"read-only detection preserves wallet bytes");input.close();
 auto bad=result.setup.profile;bad["create_wallet"]=true;need(!CoreProfile::fromJson(bad).validate(root).isEmpty(),"automatic recovery cannot authorize wallet creation");
 auto other=bundle;other["genesis_hash"]=QString(64,'c');need(!discoverDefaultExistingWallet(other,root).found,"another network folder is not searched");
 put(data+"/testnet/wallets/Second/wallet.dat",bytes);result=discoverDefaultExistingWallet(bundle,root);need(!result.found&&!result.notice.isEmpty(),"multiple wallets need explicit selection");
 QFile::remove(data+"/testnet/wallets/Second/wallet.dat");put(wallet,"CORRUPT FILE");need(!discoverDefaultExistingWallet(bundle,root).found,"invalid wallet header is not silently adopted");
 put(wallet,bytes);QDir().mkpath(data+"/testnet/wallets");for(int i=0;i<65;++i)QDir().mkpath(data+"/testnet/wallets/entry-"+QString::number(i));need(!discoverDefaultExistingWallet(bundle,root).found,"directory probe has a fixed work bound");
 std::cout<<"PASS existing default wallet discovery, no creation, unchanged bytes, network isolation and ambiguity bounds; checks="<<checks<<'\n';
}
