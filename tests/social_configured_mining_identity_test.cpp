#include "social/social_configured_mining_identity.h"
#include <QCoreApplication>
#include <iostream>
#include <stdexcept>
using namespace pgn::social;
void need(bool value){if(!value)throw std::runtime_error("configured identity contract failed");}
int main(int argc,char**argv){QCoreApplication app(argc,argv);const QString g(64,'a'),address="tpgn"+QString(30,'A');
 QJsonObject p{{"miner_address",address},{"network_arguments",QJsonArray{"ssh-options","--network","--miner",address,"--genesis",g}}};
 need(configuredMiningAddress(p,g)==address);need(configuredMiningAddress(p,QString(64,'b')).isEmpty());need(configuredMiningAddress(p,"invalid").isEmpty());
 auto changed=p;changed["miner_address"]="different";need(configuredMiningAddress(changed,g).isEmpty());
 for(const auto flag:{"--genesis","--miner"}){changed=p;auto args=p["network_arguments"].toArray();args.append(flag);args.append(g);changed["network_arguments"]=args;need(configuredMiningAddress(changed,g).isEmpty());}
 changed=p;changed.remove("network_arguments");need(configuredMiningAddress(changed,g).isEmpty());
 changed=p;auto args=p["network_arguments"].toArray();args.append(17);changed["network_arguments"]=args;need(configuredMiningAddress(changed,g).isEmpty());
 args={};for(int i=0;i<129;++i)args.append("x");changed["network_arguments"]=args;need(configuredMiningAddress(changed,g).isEmpty());
 changed=p;changed["miner_address"]="bad";changed["network_arguments"]=QJsonArray{"--genesis",g,"--miner","bad"};need(configuredMiningAddress(changed,g).isEmpty());
 std::cout<<"PASS existing remote identity: no auxiliary identity file; exact network/address and bounded, conflicting, duplicate and invalid input guards\n";
}
