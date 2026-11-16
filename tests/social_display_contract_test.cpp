#include "social/social_display_rpc_contract.h"


#include <QCoreApplication>
#include <iostream>
using namespace pgn::social;
int main(int argc,char** argv){QCoreApplication app(argc,argv);unsigned checks{};const auto expect=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};
 const QString g(64,'1'),s="0014"+QString(40,'2'),k="0014"+QString(40,'3');
 QJsonObject statement{{"v",2},{"genesis",g},{"origin","pgn-social://"+g},{"scope","display_link_for_script_history"},{"action","bind"},{"social_script",s},{"operator_script",k},{"operator_id",QString(64,'4')},{"anchor_hash",QString(64,'5')},{"anchor_height",42},{"previous_link",QString(64,'0')},{"nonce",QString(64,'6')}};
 auto certificate=QJsonObject{{"statement",statement},{"social_proof",""},{"operator_proof",""},{"authorizer_script",""},{"authorization_proof",""}};NameLinkPacket p;
 expect(decodeNameLinkPacket(canonical(certificate),g,p).isEmpty(),"Canonical partial packet refused");
 expect(displayChallenge(p.socialChallenge,g)&&displayChallenge(p.nativeChallenge,g),"Separated consent domains rejected");
 expect(!displayChallenge(p.transitionChallenge,g),"Bind accepted withdrawal signing domain");
 expect(!displayChallenge(p.socialChallenge,QString(64,'9')),"Cross-genesis consent accepted");
 expect(!displayChallenge(p.socialChallenge+" ",g),"Noncanonical challenge accepted");
 expect(!displayChallenge("PGN/SOCIAL/EVENT/1\n"+canonical(statement),g),"Chat event became display consent");
 expect(validDisplayRpcArguments("getsocialdisplaylinks",{QJsonValue(QJsonArray{s,k})},g),"Bounded target batch rejected");
 expect(!validDisplayRpcArguments("getsocialdisplaylinks",{QJsonValue(QJsonArray{s,s})},g),"Duplicate target request accepted");
 QJsonArray many;for(int i=0;i<101;++i)many.append("0014"+QString("%1").arg(i,40,16,QChar('0')));
 expect(!validDisplayRpcArguments("getsocialdisplaylinks",{QJsonValue(many)},g),"101-target request accepted");
 for(const auto method:{"walletpassphrase","signmessage","sendtoaddress","submitblock","getblocktemplate","importdescriptors","getnewaddress"})expect(!validDisplayRpcArguments(method,{},g),"General/mutating RPC exposed");
 expect(validDisplayRpcArguments("getdescriptorinfo",{"raw("+k+")"},g),"Raw public script rejected");
 expect(!validDisplayRpcArguments("getdescriptorinfo",{"wpkh(private-key)"},g),"Private/arbitrary descriptor accepted");
 expect(!validDisplayRpcArguments("deriveaddresses",{"raw("+k+")#abcd1234",QJsonArray{0,1000}},g),"Descriptor range accepted");
 expect(!validDisplayRpcArguments("getblockhash",{42.5},g),"Fractional height accepted");
 expect(!validDisplayRpcArguments("submitsocialdisplaylink",{QString::fromUtf8(p.source)},g),"Unsigned packet submitted");
 certificate["social_proof"]="AA==";certificate["operator_proof"]="AA==";
 expect(validDisplayRpcArguments("submitsocialdisplaylink",{QString::fromUtf8(canonical(certificate))},g),"Complete proof carrier grammar rejected"); // Crypto remains Native's job; AA is NOT a valid signature.
 statement["action"]="withdraw";statement["previous_link"]=QString(64,'7');certificate={{"statement",statement},{"social_proof",""},{"operator_proof",""},{"authorizer_script",k},{"authorization_proof",""}};
 expect(decodeNameLinkPacket(canonical(certificate),g,p).isEmpty()&&displayChallenge(p.transitionChallenge,g),"K-only withdrawal domain rejected");
 expect(!displayChallenge(p.nativeChallenge,g),"Withdrawal accepted bind consent domain");
 std::cout<<"PRODUCTION DISPLAY GRAMMAR PASS "<<checks<<"; structural tests, no mock crypto presented as authentication\n";
}
