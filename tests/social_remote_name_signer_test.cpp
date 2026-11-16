#include "ui/social_remote_name_signer.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>
using namespace pgn;
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);unsigned checks{};
 const auto need=[&](bool ok,const char* reason){++checks;if(!ok)throw std::runtime_error(reason);};
 try{
  QTemporaryDir folder;need(folder.isValid(),"Temporary directory unavailable");
  const auto ssh=folder.path()+"/ssh.exe",key=folder.path()+"/identity";
  for(const auto& name:{ssh,key}){QFile file(name);need(file.open(QIODevice::WriteOnly),"Test file unavailable");file.write("TEST ONLY; NEVER EXECUTED");}
  social::Identity identity{QString(64,'a'),"0014"+QString(40,'b')};const auto native="0014"+QString(40,'c');
  QJsonObject statement{{"v",2},{"genesis",identity.genesis},{"origin","pgn-social://"+identity.genesis},{"scope","display_link_for_script_history"},{"action","bind"},{"social_script",identity.subject},{"operator_script",native},{"operator_id",QString(64,'d')},{"anchor_hash",QString(64,'e')},{"anchor_height",9},{"previous_link",QString(64,'0')},{"nonce",QString(64,'f')}};
  const auto certificate=social::canonical(QJsonObject{{"statement",statement},{"social_proof",""},{"operator_proof",""},{"authorizer_script",""},{"authorization_proof",""}});
  const QString miner="rpgn1qtestpublicminingaddress123456789";
  QJsonObject profile{{"program",ssh},{"miner_address",miner},{"hardware_endpoint",QJsonObject{{"host","100.64.0.1"},{"user","qa"},{"key_file",key},{"host_key_alias","qa-miner"},{"port",22}}},{"network_arguments",QJsonArray{"qa@100.64.0.1","C:/PGN/network/horizon-engine-host.exe","--network","--rpc-port","48954","--cookie","C:/PGN/node/testnet/.cookie","--wallet","qa-wallet","--miner",miner,"--genesis",identity.genesis}}};
  const auto encode=[](const QJsonObject& p){return QJsonDocument(p).toJson(QJsonDocument::Compact);};
  const auto plan=ui::remoteNameConsentPlan(encode(profile),identity,social::NameLinkSide::Native,native,certificate);
  if(!plan.error.isEmpty())std::cerr<<plan.error.toStdString()<<'\n';need(plan.error.isEmpty(),"Valid selected remote-wallet plan refused");
  const auto body=QJsonDocument::fromJson(plan.input).object();need(body["script"]==native&&body["genesis"]==identity.genesis,"Identity scope changed");
  need(body["challenge"].toString().startsWith("PGN/SOCIAL/IDENTITY-LINK/OPERATOR-CONSENT/2\n"),"Not the exact Native-only consent domain");
  need(plan.arguments.contains("StrictHostKeyChecking=yes")&&plan.arguments.contains("BatchMode=yes"),"SSH trust checks lost");
  need(plan.arguments.contains("HostKeyAlias=qa-miner"),"Known-host identity changed");
  need(!plan.arguments.join(' ').contains(miner)&&!plan.arguments.join(' ').contains("qa-wallet"),"Request data was interpolated into remote command text");
  need(!plan.input.contains("signature")&&!plan.input.contains("password"),"Unexpected credential/signature input");
  need(!ui::remoteNameConsentPlan(encode(profile),identity,social::NameLinkSide::Social,native,certificate).error.isEmpty(),"Social consent was delegated to mining wallet");
  need(!ui::remoteNameConsentPlan(encode(profile),identity,social::NameLinkSide::Native,identity.subject,certificate).error.isEmpty(),"Wrong Native key admitted");
  auto bad=profile;auto args=bad["network_arguments"].toArray();args.append("--rpc-port");args.append("1");bad["network_arguments"]=args;
  need(!ui::remoteNameConsentPlan(encode(bad),identity,social::NameLinkSide::Native,native,certificate).error.isEmpty(),"Duplicate RPC port admitted");
  bad=profile;args=bad["network_arguments"].toArray();args[args.size()-1]=QString(64,'0');bad["network_arguments"]=args;
  need(!ui::remoteNameConsentPlan(encode(bad),identity,social::NameLinkSide::Native,native,certificate).error.isEmpty(),"Foreign network admitted");
  bad=profile;bad["miner_address"]="rpgn1qanotherpublicminingaddress1234";
  need(!ui::remoteNameConsentPlan(encode(bad),identity,social::NameLinkSide::Native,native,certificate).error.isEmpty(),"Different configured mining identity admitted");
  bad=profile;auto hardware=bad["hardware_endpoint"].toObject();hardware["host"]="host;command";bad["hardware_endpoint"]=hardware;
  need(!ui::remoteNameConsentPlan(encode(bad),identity,social::NameLinkSide::Native,native,certificate).error.isEmpty(),"Shell host text admitted");
  bad=profile;args=bad["network_arguments"].toArray();for(qsizetype i=0;i+1<args.size();++i)if(args.at(i).toString()=="--wallet"){args[i+1]="wallet' $(literal)";break;}bad["network_arguments"]=args;
  const auto literal=ui::remoteNameConsentPlan(encode(bad),identity,social::NameLinkSide::Native,native,certificate);
  need(literal.error.isEmpty()&&!literal.arguments.join(' ').contains("$(literal)"),"Wallet data entered executable text");
  need(QJsonDocument::fromJson(literal.input).object()["wallet"]=="wallet' $(literal)","Literal wallet name changed");
  need(!ui::remoteNameConsentPlan(QByteArray(65537,'x'),identity,social::NameLinkSide::Native,native,certificate).error.isEmpty(),"Oversized profile admitted");
  std::cout<<"REMOTE NAME CONTRACT PASS "<<checks<<"; request planning only, no process or cryptographic proof simulated\n";return 0;
 }catch(const std::exception& e){std::cerr<<"REMOTE NAME CONTRACT FAIL "<<checks<<": "<<e.what()<<'\n';return 1;}
}
