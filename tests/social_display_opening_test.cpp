#include "ui/social_display_session.h"
#include <QApplication>
#include <QCheckBox>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QLineEdit>
#include <QMessageBox>
#include <QTemporaryDir>
#include <QThread>
#include <iostream>

using namespace pgn;
int main(int argc,char** argv){
 QApplication app(argc,argv);unsigned checks{};QStringList notices;
 const auto need=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};
 const auto until=[&](auto predicate,int limit=2000){QElapsedTimer clock;clock.start();while(!predicate()&&clock.elapsed()<limit){app.processEvents(QEventLoop::AllEvents,10);QThread::msleep(1);}return predicate();};
 // Close only test-created information boxes, while retaining their exact text.
 QTimer closer;closer.setInterval(1);QObject::connect(&closer,&QTimer::timeout,[&]{for(auto* w:app.topLevelWidgets())if(auto* box=qobject_cast<QMessageBox*>(w);box&&box->isVisible()){notices.append(box->text());box->accept();}});closer.start();
 try{
  need(argc==2,"Actual Core23 response fixture argument required");QFile input(QString::fromLocal8Bit(argv[1]));need(input.open(QIODevice::ReadOnly),"Fixture unreadable");const auto fixture=QJsonDocument::fromJson(input.readAll()).object();
  const auto loading=fixture["loading"].toObject(),ready=fixture["ready"].toObject();
  social::Identity identity{fixture["genesis"].toString(),fixture["social_script"].toString()};const auto key=fixture["native_script"].toString();const auto anchor=ready["bestblockhash"].toString();
  need(loading["state"]=="loading"&&!loading.contains("results"),"Cold real response must lack ready rows");need(ready["results"].toArray().size()==1&&ready["results"].toArray()[0].toObject()["state"]=="unlinked","First never-linked identity must have one unlinked row");
  QTemporaryDir temp;need(temp.isValid(),"Private test directory unavailable");
  unsigned signatures{};social::NameLinkWalletSigner signer=[&](auto,auto,auto,auto,auto done){++signatures;done({},"Test must never sign");};
  {
   ui::SocialView view(temp.path()+"/cold-draft.json",{});view.setIdentity(identity);view.resize(1366,990);view.show();app.processEvents();
   unsigned reads{},anchorReads{};social::PublicDisplayRpc rpc=[&](QString method,QJsonArray args,auto done){
    if(method=="getsocialdisplaylinks"){++reads;need(args==QJsonArray{QJsonArray{identity.subject}},"Opening changed the exact Social target");done(reads==1?loading:ready,{});}
    else if(method=="getblockhash"){++anchorReads;need(args==QJsonArray{ready["height"]},"Anchor height changed");done(anchor,{});}
    else throw std::runtime_error("Opening exposed an unexpected RPC");
   };
   ui::SocialDisplaySession session(rpc,signer,identity,temp.path()+"/public-cold.json",&view,&view);
   session.open(key);need(notices.isEmpty(),"First cold click was reported as unavailable");session.open(key);
   need(reads==1,"Repeated first click did not coalesce the in-flight loading request");
   need(until([&]{return view.findChild<QDialog*>("pgn-social-name-link")!=nullptr;}),"Cold first click never opened the real consent dialogue after Native became ready");
   auto* dialog=view.findChild<QDialog*>("pgn-social-name-link");need(dialog->isVisible(),"Real consent dialogue not visible");
   need(notices.isEmpty(),"Cold loading was misreported as unavailable");need(reads==2&&anchorReads==1,"Loading retry or canonical anchor verification missing");
   need(dialog->findChild<QLineEdit*>("pgn-social-link-native-key")->text()==key,"Selected Native key changed");
   need(!dialog->findChild<QCheckBox*>("pgn-social-link-explicit-consent")->isChecked(),"Consent was checked automatically");
   need(!dialog->findChild<QPushButton*>("pgn-social-link-sign-native")->isEnabled(),"Native signing became available without explicit consent");need(signatures==0,"Opening requested a signature");
   dialog->close();view.close();
  }
  {
   ui::SocialView view(temp.path()+"/stale-draft.json",{});view.setIdentity(identity);view.show();app.processEvents();
   std::vector<std::function<void(QJsonValue,QString)>> pending;unsigned anchorReads{};
   social::PublicDisplayRpc rpc=[&](QString method,QJsonArray,auto done){if(method=="getsocialdisplaylinks")pending.push_back(done);else if(method=="getblockhash"){++anchorReads;done(anchor,{});}else throw std::runtime_error("Unexpected stale-opening RPC");};
   ui::SocialDisplaySession session(rpc,signer,identity,temp.path()+"/public-stale.json",&view,&view);
   session.open(key);session.open({});need(pending.size()==2,"Changed intent did not start a new generation");pending[0](ready,{});need(!view.findChild<QDialog*>("pgn-social-name-link")&&anchorReads==0,"Old callback opened or advanced a consent");
   pending[1](ready,{});need(until([&]{return view.findChild<QDialog*>("pgn-social-name-link")!=nullptr;}),"Current callback lost");need(view.findChild<QDialog*>("pgn-social-name-link")->findChild<QLineEdit*>("pgn-social-link-native-key")->text().isEmpty(),"Old selected key leaked into new intent");view.close();
  }
  {
   ui::SocialView view(temp.path()+"/reorg-draft.json",{});view.setIdentity(identity);view.show();app.processEvents();const int before=notices.size();
   social::PublicDisplayRpc rpc=[&](QString method,QJsonArray,auto done){done(method=="getblockhash"?QJsonValue(QString(64,'f')):QJsonValue(ready),{});};
   ui::SocialDisplaySession session(rpc,signer,identity,temp.path()+"/public-reorg.json",&view,&view);session.open(key);
   need(notices.size()==before+1&&notices.back().contains("snapshot changed"),"Reorganized anchor was not refused explicitly");need(!view.findChild<QDialog*>("pgn-social-name-link"),"Reorganized anchor opened a consent");view.close();
  }
  {
   ui::SocialView view(temp.path()+"/hidden-draft.json",{});view.setIdentity(identity);view.show();app.processEvents();std::function<void(QJsonValue,QString)> pending;
   social::PublicDisplayRpc rpc=[&](QString,QJsonArray,auto done){pending=done;};
   auto* session=new ui::SocialDisplaySession(rpc,signer,identity,temp.path()+"/public-hidden.json",&view,&view);session->open(key);view.hide();pending(ready,{});need(!view.findChild<QDialog*>("pgn-social-name-link"),"Hidden view stole focus with a late dialogue");delete session;pending(ready,{});need(signatures==0,"Late callback signed");
  }
  {
   ui::SocialView view(temp.path()+"/bounded-draft.json",{});view.setIdentity(identity);view.show();app.processEvents();const int before=notices.size();unsigned reads{};
   social::PublicDisplayRpc rpc=[&](QString method,QJsonArray,auto done){need(method=="getsocialdisplaylinks","Loading advanced beyond head resolution");++reads;done(loading,{});};
   ui::SocialDisplaySession session(rpc,signer,identity,temp.path()+"/public-bounded.json",&view,&view);session.open(key);
   need(until([&]{return notices.size()>before;},12000),"Loading did not terminate with an explicit retry");need(notices.back().contains("still loading"),"Loading was reported as missing capability");need(reads>1&&reads<=45,"Loading retry work was not bounded");need(!view.findChild<QDialog*>("pgn-social-name-link")&&signatures==0,"Unready context opened or signed a consent");view.close();
  }
  std::cout<<"DISPLAY OPENING PASS "<<checks<<"; actual Qt consumer, recorded Core23 replies; no crypto/signatures mocked as authentication\n";return 0;
 }catch(const std::exception& error){std::cerr<<"DISPLAY OPENING FAIL after "<<checks<<": "<<error.what()<<"\n";return 1;}
}
