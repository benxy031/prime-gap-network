#include "social/social_automatic_name.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <QEventLoop>
#include <iostream>
using namespace pgn::social;
static int checks{};
static void need(bool ok,const char* what){if(!ok){std::cerr<<"FAIL "<<what<<'\n';std::exit(1);}++checks;}
static void settle(AutomaticPublicName& name){QElapsedTimer t;t.start();while(name.busy()&&t.elapsed()<3000)QCoreApplication::processEvents(QEventLoop::AllEvents,10);need(!name.busy(),"bounded completion");}
struct Fixture {
 Identity identity{QString(64,'a'),"0014"+QString(40,'b')};QString native="0014"+QString(40,'c'),state="unlinked";
 int reads{},signs{},submits{},prepares{};bool uncertain{};QByteArray submitted;QString packetId;
 PublicDisplayRpc rpc(){return [this](QString method,QJsonArray,auto done){++reads;
  if(method=="validateaddress")done(QJsonObject{{"isvalid",true},{"scriptPubKey",native}},{});
  else done(QJsonObject{{"schema","PGN_SOCIAL_CURRENT_DISPLAY_1"},{"genesis",identity.genesis},{"current_display_only",true},{"permissions_granted",false},{"results",QJsonArray{QJsonObject{{"social_script",identity.subject},{"complete",true},{"state",state},{"operator_script",native}}}}},{});
 };}
 NameLinkServices services(){NameLinkServices services;services.currentDisplayOnly=true;
  services.prepare=[this](NameLinkIntent intent,QByteArray source,auto done){++prepares;
   if(source.isEmpty())source=canonical(QJsonObject{{"statement",QJsonObject{{"v",2},{"genesis",identity.genesis},{"origin","pgn-social://"+identity.genesis},{"scope","display_link_for_script_history"},{"action","bind"},{"social_script",identity.subject},{"operator_script",native},{"operator_id",QString(64,'d')},{"anchor_hash",QString(64,'e')},{"anchor_height",9},{"previous_link",intent.previousHead},{"nonce",QString(64,'f')}}},{"social_proof",""},{"operator_proof",""},{"authorizer_script",""},{"authorization_proof",""}});
   NameLinkPacket packet;need(decodeNameLinkPacket(source,identity.genesis,packet).isEmpty(),"valid protocol fixture");packetId=packet.id;
   done(NameLinkPrepared{packet,"Verified public alias",QString(64,'e'),9,!packet.certificate["social_proof"].toString().isEmpty(),!packet.certificate["operator_proof"].toString().isEmpty(),false,false,false},{});
  };
  services.sign=[this](NameLinkIntent intent,NameLinkSide side,QString script,QByteArray,auto done){++signs;need(intent.session==identity&&script==(side==NameLinkSide::Native?native:identity.subject),"exact configured identity and side");done("cHJvb2Y=",{});};
  services.submit=[this](NameLinkIntent,QByteArray source,auto done){++submits;submitted=source;if(uncertain){done({},"reply lost");return;}done(QJsonObject{{"schema","PGN_SOCIAL_DISPLAY_SUBMIT_2"},{"genesis",identity.genesis},{"event_id",packetId},{"permissions_granted",false},{"state","verified_local"}},{});};return services;
 }
};
int main(int argc,char** argv){QCoreApplication app(argc,argv);QTemporaryDir root;need(root.isValid(),"isolated directory");
 {Fixture f;AutomaticPublicName name(f.rpc(),f.services(),f.identity,root.path()+"/fresh.json");QString status;name.changed=[&](QString s,QString){status=s;};name.ensure("configured-address");settle(name);need(status=="active"&&f.signs==2&&f.submits==1,"automatic name without a dialog, two proofs and one submission");const auto reads=f.reads;for(int i=0;i<100;++i)name.ensure("configured-address");need(f.reads==reads&&f.signs==2&&f.submits==1,"successful name creates no repetitive network work");}
 for(const auto state:{"linked","withdrawn","conflict"}){Fixture f;f.state=state;AutomaticPublicName name(f.rpc(),f.services(),f.identity,root.path()+"/"+state+".json");name.ensure("configured-address");settle(name);need(f.prepares==0&&f.signs==0&&f.submits==0,"existing links, withdrawals and conflicts are never replaced");}
 {Fixture f;f.uncertain=true;const auto path=root.path()+"/retry.json";QByteArray first;
  {AutomaticPublicName name(f.rpc(),f.services(),f.identity,path);name.ensure("configured-address");settle(name);first=f.submitted;need(f.signs==2&&f.submits==1&&!first.isEmpty(),"uncertain complete packet saved");}
  f.uncertain=false;{AutomaticPublicName name(f.rpc(),f.services(),f.identity,path);name.ensure("configured-address");settle(name);need(f.signs==2&&f.submits==2&&f.submitted==first,"restart reuses byte-identical signed packet without signing again");}
 }
 {Fixture f;const auto path=root.path()+"/unwritable";QDir().mkpath(path);AutomaticPublicName name(f.rpc(),f.services(),f.identity,path);name.ensure("configured-address");settle(name);need(f.signs==0&&f.submits==0,"failed durable save prevents signatures and publication");}
 {Fixture f;auto rpc=[](QString,QJsonArray,auto done){done(QJsonObject{{"isvalid",false}},{});};AutomaticPublicName name(rpc,f.services(),f.identity,root.path()+"/invalid.json");name.ensure("invalid-address");settle(name);need(f.signs==0&&f.submits==0,"invalid mining identity remains unchanged");}
 {Fixture f;std::function<void(QJsonValue,QString)> late;auto rpc=[&](QString,QJsonArray,auto done){late=done;};auto* name=new AutomaticPublicName(rpc,f.services(),f.identity,root.path()+"/late.json");name->ensure("configured-address");delete name;late(QJsonObject{{"isvalid",true},{"scriptPubKey",f.native}},{});need(f.signs==0&&f.submits==0,"late reply after wallet/session destruction cannot sign");}
 std::cout<<"PASS automatic public names: exact proof, no dialog, no repeated work, restart replay, explicit withdrawal and lifetime guards; checks="<<checks<<'\n';
}
