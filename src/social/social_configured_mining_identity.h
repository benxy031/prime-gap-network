#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStringList>
namespace pgn::social {
// Configuration selects a key; Core and both wallet proofs still verify it.
inline QString configuredMiningAddress(const QJsonObject& profile,const QString& genesis){
 if(!QRegularExpression("^[0-9a-f]{64}$").match(genesis).hasMatch())return {};
 const auto values=profile.value("network_arguments").toArray();if(values.isEmpty()||values.size()>128)return {};
 QStringList args;for(const auto& value:values){if(!value.isString()||value.toString().size()>4096)return {};args.append(value.toString());}
 const auto option=[&](const QString& name){const auto at=args.indexOf(name);return args.count(name)==1&&at>=0&&at+1<args.size()?args[at+1]:QString{};};
 const auto address=option("--miner");
 if(option("--genesis")!=genesis||profile.value("miner_address").toString()!=address||!QRegularExpression("^(?:tpgn1[0-9a-z]{8,100}|tpgn[1-9A-HJ-NP-Za-km-z]{20,60})$").match(address).hasMatch())return {};
 return address;
}
}
