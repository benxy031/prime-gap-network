#include "social/social_display_resolver.h"
#include <QCoreApplication>
#include <iostream>
#include <vector>
using namespace pgn::social;
using DisplayTestReply=std::function<void(QJsonValue,QString)>;
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);const QString genesis(64,'a'),subject="0014"+QString(40,'1'),anchor(64,'b');
 Identity identity{genesis,subject};Snapshot input;input.identity=identity;input.section=Section::Chat;input.entries.resize(1);input.entries[0].authorId=subject;input.entries[0].author="Miner fallback";
 enum Mode{Linked,Loading,Unavailable,Withdrawn,WrongNetwork};Mode mode=Linked;QString label="Alpha";bool held=false;std::vector<DisplayTestReply> anchors;
 PublicDisplayRpc rpc=[&](QString method,QJsonArray,DisplayTestReply done){
  if(method=="getblockhash"){if(held)anchors.push_back(std::move(done));else done(anchor,{});return;}
  if(method!="getsocialdisplaylinks"){done({},"Unexpected RPC");return;}
  if(mode==Unavailable){done({},"Transient Core queue unavailable");return;}
  QJsonObject root{{"schema","PGN_SOCIAL_CURRENT_DISPLAY_1"},{"genesis",mode==WrongNetwork?QString(64,'c'):genesis},{"current_display_only",true},{"permissions_granted",false}};
  if(mode==Loading){root["state"]="loading";done(root,{});return;}
  QJsonObject row{{"social_script",subject},{"state",mode==Withdrawn?"withdrawn":"linked"},{"complete",true},{"certificate_id",QString(64,'d')},{"operator_id",QString(64,'e')},{"operator_script","0014"+QString(40,'2')},{"display_name",label}};
  root["bestblockhash"]=anchor;root["height"]=1;root["results"]=QJsonArray{row};done(root,{});
 };
 NativeDisplayResolver resolver(rpc,identity);QString observed;int completions=0;
 auto read=[&]{resolver.decorate(input,[&](Snapshot value,QString error){if(!error.isEmpty())observed="ERROR";else observed=value.entries[0].author;++completions;});};
 auto require=[&](bool ok,const char* why){if(!ok)std::cerr<<why<<": "<<observed.toStdString()<<'\n';return ok;};
 read();if(!require(observed.startsWith("Alpha"),"Initial canonical name"))return 1;
 mode=Loading;read();
 if(argc==2&&QString::fromLocal8Bit(argv[1])=="--expect-old-flicker"){
  if(!require(observed=="Miner fallback","Original registry-loading flicker reproduced"))return 1;
  std::cout<<"PASS exact original: a loading refresh replaces the verified name with Miner\n";return 0;
 }
 if(!require(observed.startsWith("Alpha"),"Loading preserves the last canonical name"))return 1;
 mode=Unavailable;read();if(!require(observed.startsWith("Alpha"),"Transient RPC error preserves display"))return 1;
 mode=Linked;label="Beta";read();if(!require(observed.startsWith("Beta"),"Verified rename replaces the cache"))return 1;
 mode=WrongNetwork;read();if(!require(observed=="Miner fallback","Another network never receives a cached projection"))return 1;
 mode=Withdrawn;read();if(!require(observed=="Miner fallback","Canonical withdrawal removes the name"))return 1;
 mode=Loading;read();if(!require(observed=="Miner fallback","Loading cannot resurrect a withdrawn name"))return 1;
 mode=Linked;held=true;label="Older";read();label="Newest";read();
 if(!require(anchors.size()==2,"Two concurrent canonical checks"))return 1;
 anchors[1](anchor,{});if(!require(observed.startsWith("Newest"),"Newer response accepted"))return 1;
 anchors[0](anchor,{});if(!require(observed.startsWith("Newest"),"Late older response cannot undo the name"))return 1;
 if(!require(completions==9,"One completion per read"))return 1;
 mode=Linked;label="New live label";read();
 if(!require(observed.startsWith("Newest")&&completions==10&&anchors.size()==3,"Cached name displayed before held Core reply"))return 1;
 read();
 if(!require(completions==11&&anchors.size()==3,"Warm refresh is coalesced while Core is busy"))return 1;
 anchors[2](anchor,{});
 if(!require(completions==11,"Background refresh does not complete the same read twice"))return 1;
 held=false;read();
 if(!require(observed.startsWith("New live label"),"Latest verified name appears on the next refresh"))return 1;
 std::cout<<"PASS stable names through loading/error, canonical rename/withdrawal, identity separation, response ordering\n";
}
