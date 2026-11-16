#pragma once
#include "social/social_link_flow.h"
#include "social/social_leaderboard.h"
namespace pgn::social {
inline bool displayScript(const QString& s){return QRegularExpression("^0014[0-9a-f]{40}$").match(s).hasMatch();}
inline bool displayAddress(const QJsonValue& v){return v.isString()&&QRegularExpression("^[a-z0-9]{8,100}$").match(v.toString()).hasMatch();}
inline bool displayChallenge(const QByteArray& bytes,const QString& genesis){
 if(bytes.size()<64||bytes.size()>24576)return false;
 const auto split=bytes.indexOf('\n');const auto domain=bytes.left(split);
 if(domain!="PGN/SOCIAL/IDENTITY-LINK/SOCIAL-CONSENT/2"&&domain!="PGN/SOCIAL/IDENTITY-LINK/OPERATOR-CONSENT/2"&&domain!="PGN/SOCIAL/IDENTITY-LINK/STATE-TRANSITION/2")return false;
 const auto body=QJsonDocument::fromJson(bytes.mid(split+1)).object();
 if(!linkObjectKeys(body,{"event_id","statement"})||canonical(body)!=bytes.mid(split+1)||!body["statement"].isObject())return false;
 const auto statement=body["statement"].toObject();const bool withdrawal=statement["action"]=="withdraw";
 NameLinkPacket packet;const auto source=canonical(QJsonObject{{"statement",statement},{"social_proof",""},{"operator_proof",""},{"authorizer_script",withdrawal?statement["social_script"].toString():QString{}},{"authorization_proof",""}});
 if(!decodeNameLinkPacket(source,genesis,packet).isEmpty()||body["event_id"]!=packet.id)return false;
 return withdrawal?bytes==packet.transitionChallenge:(bytes==packet.socialChallenge||bytes==packet.nativeChallenge);
}
// Closed public-read/display-submit grammar. No wallet, transaction, descriptor
// import, file access, template, mining or arbitrary RPC is exposed.
inline bool validDisplayRpcArguments(const QString& method,const QJsonArray& args,const QString& genesis){
 if(method=="getsocialdisplaylinks"){
  if(args.size()!=1||!args[0].isArray()||args[0].toArray().isEmpty()||args[0].toArray().size()>100)return false;
  QSet<QString> seen;for(const auto& v:args[0].toArray()){if(!v.isString()||!displayScript(v.toString())||seen.contains(v.toString()))return false;seen.insert(v.toString());}return true;
 }
 if(method=="submitsocialdisplaylink"){
  if(args.size()!=1||!args[0].isString())return false;NameLinkPacket p;
  if(!decodeNameLinkPacket(args[0].toString().toUtf8(),genesis,p).isEmpty())return false;
  return p.action=="bind"?(!p.certificate["social_proof"].toString().isEmpty()&&!p.certificate["operator_proof"].toString().isEmpty()):!p.certificate["authorization_proof"].toString().isEmpty();
 }
 if(method=="getblockhash")return args.size()==1&&args[0].isDouble()&&args[0].toInteger(-1)>=0&&args[0].toInteger()<=2147483647&&args[0].toDouble()==double(args[0].toInteger());
 if(method=="getpgnleaderboard")return validLeaderboardRpcArguments(args);
 if(method=="getpgnparticipants")return args.size()==3&&args[0]==0&&args[1]==1&&displayAddress(args[2]);
 if(method=="getdescriptorinfo")return args.size()==1&&args[0].isString()&&QRegularExpression("^raw\\(0014[0-9a-f]{40}\\)$").match(args[0].toString()).hasMatch();
 if(method=="deriveaddresses")return args.size()==1&&args[0].isString()&&QRegularExpression("^raw\\(0014[0-9a-f]{40}\\)#[a-z0-9]{8}$").match(args[0].toString()).hasMatch();
 if(method=="validateaddress")return args.size()==1&&displayAddress(args[0]);
 if(method=="verifymessage")return args.size()==3&&displayAddress(args[0])&&args[1].isString()&&!args[1].toString().isEmpty()&&args[1].toString().size()<=512&&args[2].isString()&&displayChallenge(args[2].toString().toUtf8(),genesis);
 return false;
}
}
