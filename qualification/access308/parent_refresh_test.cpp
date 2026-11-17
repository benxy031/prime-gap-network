#include "ui/social_view.h"
#include "ui/social_access_dialog.h"
#include <QApplication>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>
using namespace pgn;
static void require(bool v,const char* why){if(!v)throw std::runtime_error(why);}
int main(int argc,char** argv){
 qputenv("QT_QPA_PLATFORM","offscreen");QApplication app(argc,argv);
 try{
  QTemporaryDir dir;social::Identity identity{QString(64,'a'),"0014"+QString(40,'1')};int reads=0,writes=0;
  social::AccessSnapshot focused;focused.identity=identity;focused.authenticated=true;focused.community=QString(64,'c');focused.revision="1";focused.source="SYNTHETIC PARENT/UI BOUNDARY ONLY";focused.observedUtc=QDateTime::currentDateTimeUtc();focused.moderatorRoleId="moderator";focused.permissionCatalog={"manage_roles"};focused.grantablePermissions=focused.permissionCatalog;focused.roles={{"moderator","Moderator",999,focused.permissionCatalog,true},{"helper","Helper",10,{},false}};
  auto& auth=focused.session;auth.identity=identity;auth.state=social::AuthState::Verified;auth.reference="proof-A";auth.verificationRef="native:proof-A";auth.revocationRevision="1";auth.device="fixture-native-account";auth.devices={{auth.device,"Fixture",false}};auth.expiresUtc=QDateTime::currentDateTimeUtc().addSecs(120);
  for(auto action:{social::AccessAction::EditRole,social::AccessAction::MoveRole,social::AccessAction::DeleteRole})focused.availability.append({action,{},"helper",{},true,{}});
  auto parent=focused;std::function<void(social::AccessSnapshot,QString)> pending;
  social::Services services;services.read=[&](social::ReadRequest r,auto done){social::Snapshot s;s.identity=r.identity;s.section=r.section;s.authenticated=true;s.source="SYNTHETIC PARENT/UI BOUNDARY ONLY";s.observedUtc=QDateTime::currentDateTimeUtc();s.channel="global";s.access=parent;done(s,{});};
  bool defer=false;services.readAccess=[&](social::AccessReadRequest,auto done){++reads;if(defer)pending=done;else done(focused,{});};
  services.manage=[&](social::AccessRequest,auto){++writes;throw std::runtime_error("Regression test must not mutate service data");};
  ui::SocialView view(dir.path()+"/drafts.json",services);view.setIdentity(identity);view.refresh();app.processEvents();auto* open=view.findChild<QPushButton*>("pgn-social-open-access");require(open,"Missing real roles opener");open->click();app.processEvents();
  auto* dialog=dynamic_cast<ui::SocialAccessDialog*>(view.findChild<QDialog*>("pgn-social-access-dialog"));require(dialog,"Missing actual child role dialog");auto* roles=dialog->findChild<QListWidget*>("pgn-social-role-list");auto* name=dialog->findChild<QLineEdit*>("pgn-social-role-name");auto* save=dialog->findChild<QPushButton*>("pgn-social-save-role");auto* refresh=dialog->findChild<QPushButton*>("pgn-social-refresh-access");require(roles&&name&&save&&refresh,"Missing role editor controls");
  for(int i=0;i<roles->count();++i)if(roles->item(i)->data(Qt::UserRole)=="helper")roles->setCurrentRow(i);
  require(save->isEnabled(),"Fixture did not grant exact editable role");
  focused.session.reference="proof-B";focused.session.verificationRef="native:proof-B";focused.observedUtc=focused.observedUtc.addMSecs(10);refresh->click();require(reads==1&&save->isEnabled(),"Focused read did not finish");name->setText("Unsaved role edit");parent=focused;view.refresh();app.processEvents();
  if(ORIGINAL_BEHAVIOR){require(!save->isEnabled(),"Original no longer reproduces parent invalidation");std::cout<<"PASS negative control: original parent refresh disabled current focused editor\n";return 0;}
  require(save->isEnabled()&&name->text()=="Unsaved role edit"&&reads==1,"Parent invalidated an already-current focused editor");
  parent.session.reference="proof-C";parent.session.verificationRef="native:proof-C";parent.observedUtc=parent.observedUtc.addMSecs(10);view.refresh();app.processEvents();require(save->isEnabled()&&name->text()=="Unsaved role edit"&&reads==1,"Equivalent background proof caused extra read or lost edit");
  parent=focused;parent.revision="old";parent.observedUtc=focused.observedUtc.addMSecs(-1);view.refresh();app.processEvents();require(save->isEnabled(),"Stale parent cache invalidated newer focused state");
  parent=focused;parent.revision="2";parent.session.revocationRevision="2";parent.observedUtc=focused.observedUtc.addMSecs(30);view.refresh();app.processEvents();require(!save->isEnabled(),"Real policy change failed to invalidate old actions");
  focused=parent;refresh->click();require(save->isEnabled(),"Refresh after policy change did not restore service-authorized controls");
  defer=true;refresh->click();require(pending&&!save->isEnabled(),"Deferred current read not pending");parent=focused;parent.session.state=social::AuthState::Revoked;parent.session.devices[0].revoked=true;parent.observedUtc=focused.observedUtc.addMSecs(40);view.refresh();app.processEvents();auto late=std::move(pending);late(focused,{});app.processEvents();require(!save->isEnabled(),"Late focused callback resurrected revoked permission");
  defer=false;refresh->click();require(save->isEnabled(),"Independent service reread failed");parent=focused;parent.session.device="other-account";parent.session.devices={{"other-account","Other",false}};parent.observedUtc=focused.observedUtc.addMSecs(50);view.refresh();app.processEvents();require(!save->isEnabled(),"Verified account change retained old actions");require(writes==0,"Test mutated service");
  std::cout<<"PASS current/equivalent proof refresh, unsaved edits, stale parent cache, changed policy/account, revoked session and late callback; UI fixture only, no production rights\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
