#include "ui/social_remote_access_signer.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using namespace pgn;
static void require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
 QTemporaryDir folder;const auto ssh=folder.path()+"/ssh.exe",key=folder.path()+"/test.key";for(const auto& path:{ssh,key}){QFile file(path);require(file.open(QIODevice::WriteOnly),"Fixture file");file.write("test fixture only");}
 const social::Identity identity{QString(64,'a'),"0014"+QString(40,'1')};const social::AccessNativeContext native{"0014"+QString(40,'2'),"FixtureAddress12345678",QString(64,'c')};
 QJsonObject hardware{{"host","example.test"},{"user","fixture"},{"key_file",key},{"host_key_alias","pgn-fixture"},{"port",22}};
 QJsonArray arguments{"C:/fixture/miner.exe","--network","--genesis",identity.genesis,"--wallet","fixture","--cookie","C:/fixture/.cookie","--miner",native.address,"--rpc-port","12345"};
 const auto profile=QJsonDocument(QJsonObject{{"program",ssh},{"hardware_endpoint",hardware},{"miner_address",native.address},{"network_arguments",arguments}}).toJson(QJsonDocument::Compact);
 const auto wire=social::accessReadWire({identity,"global",{},{},{QString(64,'d')}},native);const auto plan=ui::remoteAccessConsentPlan(profile,identity,native.community,native.script,wire);require(plan.error.isEmpty(),"Valid configured access plan refused");
 require(plan.arguments.join(' ').size()<8191,"Windows SSH command line exceeds shell limit");const auto frames=plan.input.split('\n');require(frames.size()==3&&frames[2].isEmpty(),"Two bounded stdin frames required");require(QByteArray::fromBase64(frames[0])==ui::nativeAccessSignerPowerShell(),"Stdin program differs from compiled scoped signer");const auto request=QJsonDocument::fromJson(frames[1]).object();require(request["challenge"].toString().toUtf8()==wire&&request["client"]==identity.subject,"Public command changed in transport");require(!ui::remoteAccessConsentPlan(profile,identity,QString(64,'e'),native.script,wire).error.isEmpty(),"Wrong community admitted");
 std::cout<<"PASS short Windows SSH command, exact bounded stdin signer/request frames, wrong-community refusal\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
