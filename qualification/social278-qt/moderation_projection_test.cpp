#include "ui/social_view.h"
#include "ui/social_access_session.h"
#include "ui/social_entry_delegate.h"
#include <QApplication>
#include <QTemporaryDir>
#include <QMenu>
#include <QAction>
#include <iostream>
#include <stdexcept>
using namespace pgn;
static void require(bool good,const char* why){if(!good)throw std::runtime_error(why);}
int main(int argc,char** argv){
 qputenv("QT_QPA_PLATFORM","offscreen");QApplication app(argc,argv);
 try{
  QTemporaryDir dir;const social::Identity identity{QString(64,'a'),"0014"+QString(40,'1')};const auto oldId=QString(64,'1'),newId=QString(64,'2');const auto created=QDateTime::currentDateTimeUtc().addSecs(-30);bool recentOnly=false,removed=false,pinned=false;int writes=0,focused=0;
  social::AccessSnapshot access;access.identity=identity;access.authenticated=true;access.community=QString(64,'c');access.revision="1";access.source="SYNTHETIC UI BOUNDARY ONLY";access.observedUtc=QDateTime::currentDateTimeUtc();access.moderatorRoleId="moderator";access.permissionCatalog={"manage_messages"};access.grantablePermissions=access.permissionCatalog;access.roles={{"moderator","Moderator",999,access.permissionCatalog,true}};
  auto& session=access.session;session.identity=identity;session.state=social::AuthState::Verified;session.reference="focused-reference";session.verificationRef="synthetic-proof";session.revocationRevision="1";session.device=identity.subject;session.devices={{identity.subject,"Synthetic fixture",false}};session.expiresUtc=QDateTime::currentDateTimeUtc().addSecs(120);
  for(auto action:{social::AccessAction::RemoveContent,social::AccessAction::PinContent})access.availability.append({action,oldId,{},"global",true,{},social::AccessScope::Chat,{},{}});
  social::Services services;services.read=[&](social::ReadRequest r,auto done){social::Snapshot s;s.identity=r.identity;s.section=r.section;s.authenticated=true;s.source="SYNTHETIC UI BOUNDARY ONLY";s.observedUtc=QDateTime::currentDateTimeUtc();s.channel="global";s.chatHistoryGeneration=QString(64,'d');s.channels=QVector<social::Channel>{{"global","Global",{}}};s.chatSend=social::SendAvailability{true,{}};
   const auto entry=[&](QString id,QString body,int offset){social::Entry e;e.id=id;e.author="Synthetic";e.authorId=identity.subject;e.body=body;e.createdUtc=created.addSecs(offset);return e;};
   if(!recentOnly)s.entries.append(entry(oldId,"Retained older message",0));s.entries.append(entry(newId,"Recent message",1));if(removed)s.removedContent<<oldId;if(pinned)s.pinnedContent<<oldId;done(s,{});
  };
  services.readAccess=[&](social::AccessReadRequest r,auto done){++focused;require(r.targets==QStringList{oldId},"Context menu did not focus exact immutable message ID");done(access,{});};
  services.manage=[&](social::AccessRequest r,auto done){++writes;require(r.target==oldId&&r.sessionReference==access.session.reference&&r.scope==social::AccessScope::Chat,"Menu retained stale permissions or wrong message scope");if(r.action==social::AccessAction::PinContent)pinned=true;else if(r.action==social::AccessAction::RemoveContent)removed=true;else throw std::runtime_error("Unexpected moderation action");done(social::AccessResult{r.identity,r.community,r.channel,r.requestId,social::AccessOutcome::Accepted,{},"2"});};
  ui::SocialView view(dir.path()+"/drafts.json",services);view.resize(1200,800);view.setIdentity(identity);view.show();view.refresh();app.processEvents();auto* list=view.findChild<QListWidget*>("pgn-social-list-0");require(list&&list->count()==2,"Initial chat window missing");recentOnly=true;view.refresh();app.processEvents();require(list->count()==2,"Retained history unexpectedly lost older message");
  const auto action=[&](QString title){list->setCurrentRow(0);const auto pos=list->visualItemRect(list->item(0)).center();QMetaObject::invokeMethod(list,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,pos));app.processEvents();auto* menu=view.findChild<QMenu*>("pgn-social-context-0");require(menu,"No actual context menu");QAction* target=nullptr;for(auto* a:menu->actions())if(a->text()==title)target=a;require(target&&target->isEnabled(),"Focused signed-permission result did not enable the context action");target->trigger();menu->close();app.processEvents();};
  action("Pin message");require(writes==1&&list->count()==2&&list->item(0)->data(ui::PinnedRole).toBool(),"Accepted pin not applied to retained message");
  action("Remove message");require(writes==2&&focused==2&&list->count()==1&&list->item(0)->data(Qt::UserRole)==newId,"Accepted removal not applied to retained message");
  view.refresh();app.processEvents();require(list->count()==1,"Deleted retained message reappeared after refresh");
  std::cout<<"PASS focused context authorization, pin marker, retained-history removal and refresh persistence; UI fixture only, no production rights\n";return 0;
 }catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
