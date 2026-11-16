#include "ui/social_name_link_dialog.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QPushButton>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QElapsedTimer>
#include <QThread>
#include <iostream>
using namespace pgn;using namespace pgn::social;
int checks=0;void expect(bool b,const char* why){++checks;if(!b)throw std::runtime_error(why);}
void drain(int ms=100){QElapsedTimer t;t.start();while(t.elapsed()<ms){QCoreApplication::processEvents();QThread::msleep(1);}}
struct Fixture{
 QString g=QString(64,'1'),s="0014"+QString(40,'2'),k="0014"+QString(40,'3');int signatures=0,submitted=0;bool delayed=false;std::function<void(QString,QString)> held;QVector<NameLinkSide> sides;
 NameLinkIntent intent(){return {{g,s},s,k,"bind",QString(64,'0')};}
 NameLinkServices services(){NameLinkServices out;out.currentDisplayOnly=true;
  out.prepare=[this](NameLinkIntent i,QByteArray bytes,NameLinkPreparedReply done){
   if(bytes.isEmpty()){
    QJsonObject statement{{"v",2},{"genesis",g},{"origin","pgn-social://"+g},{"scope","display_link_for_script_history"},{"action",i.action},{"social_script",i.socialScript},{"operator_script",i.nativeScript},{"operator_id",QString(64,'4')},{"anchor_hash",QString(64,'5')},{"anchor_height",42},{"previous_link",i.previousHead},{"nonce",QString(64,'6')}};
    bytes=canonical(QJsonObject{{"statement",statement},{"social_proof",""},{"operator_proof",""},{"authorizer_script",""},{"authorization_proof",""}});
   }
   NameLinkPacket p;const auto error=decodeNameLinkPacket(bytes,g,p);if(!error.isEmpty()){done({},error);return;}
   NameLinkPrepared prepared{p,"Wizz",QString(64,'5'),42,!p.certificate["social_proof"].toString().isEmpty(),!p.certificate["operator_proof"].toString().isEmpty(),false,false,false};done(prepared,{});
  };
  out.sign=[this](NameLinkIntent i,NameLinkSide side,QString key,QByteArray challenge,auto done){++signatures;sides.append(side);expect(i==intent(),"Automatic signer changed intent");expect(key==(side==NameLinkSide::Native?k:s),"Automatic signer changed owner");expect(!challenge.isEmpty(),"Empty consent challenge");if(delayed){held=done;return;}done("AA==",{});};
  out.submit=[this](NameLinkIntent i,QByteArray bytes,auto done){++submitted;NameLinkPacket p;expect(decodeNameLinkPacket(bytes,g,p).isEmpty(),"Invalid automatic submission");done({{"schema","PGN_SOCIAL_DISPLAY_SUBMIT_2"},{"genesis",g},{"event_id",p.id},{"permissions_granted",false},{"state","verified_local"}},{});};return out;
 }
};
int main(int argc,char**argv){QApplication app(argc,argv);QTemporaryDir tmp;expect(tmp.isValid(),"Temporary fixture directory failed");
 {Fixture f;ui::SocialNameLinkDialog d(f.services(),f.intent(),tmp.path()+"/one.json");d.show();drain();auto* quick=d.findChild<QPushButton*>("pgn-social-link-quick");expect(quick&&quick->isEnabled(),"Name confirmation missing or disabled");expect(f.signatures==0&&f.submitted==0,"Preparation signed or published without consent");expect(!d.findChild<QLineEdit*>("pgn-social-link-native-key")->isVisible(),"Raw key shown in ordinary flow");expect(!d.findChild<QPlainTextEdit*>("pgn-social-link-public-packet")->isVisible(),"Packet shown in ordinary flow");quick->click();{QElapsedTimer deadline;deadline.start();while(d.flow().phase()!=NameLinkPhase::VerifiedLocal&&deadline.elapsed()<3000)drain(10);}expect(f.signatures==2&&f.sides==QVector<NameLinkSide>{NameLinkSide::Native,NameLinkSide::Social},"One confirmation failed to request both scoped signatures");expect(f.submitted==1&&d.flow().phase()==NameLinkPhase::VerifiedLocal,"One confirmation did not publish exactly once");quick->click();drain();expect(f.submitted==1,"Repeated click duplicated publication");d.close();}
 {Fixture f;f.delayed=true;ui::SocialNameLinkDialog d(f.services(),f.intent(),tmp.path()+"/close.json");d.show();drain();d.findChild<QPushButton*>("pgn-social-link-quick")->click();drain();expect(f.signatures==1&&bool(f.held),"Delayed signature fixture not reached");d.close();f.held("AA==",{});drain();expect(f.signatures==1&&f.submitted==0,"Closing continued automatic signatures/publication");}
 {Fixture f;f.delayed=true;ui::SocialNameLinkDialog d(f.services(),f.intent(),tmp.path()+"/changed.json");d.show();drain();d.findChild<QPushButton*>("pgn-social-link-quick")->click();drain();auto next=f.intent();next.nativeScript="0014"+QString(40,'9');d.flow().setIntent(next);f.held("AA==",{});drain();expect(f.signatures==1&&f.submitted==0,"Changed intent accepted stale signature/publication");d.close();}
 std::cout<<"ONE-CONFIRMATION UI PASS "<<checks<<"; fake signing services test orchestration only, not cryptographic proof\n";
}
