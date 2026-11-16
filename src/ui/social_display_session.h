#pragma once
#include "social/social_display_consent.h"
#include "ui/social_name_link_dialog.h"
#include "ui/social_view.h"
#include <QMessageBox>
#include <QTabWidget>
namespace pgn::ui {
// One session per verified wallet binding, owned by its CoreSocialService.
// The Native index is authoritative; only a recoverable public packet is local.
class SocialDisplaySession final:public QObject {
 social::PublicDisplayRpc rpc_;social::Identity identity_;QString recovery_;
 QPointer<SocialView> view_;social::NativeDisplayResolver resolver_;
 social::CurrentDisplayConsent consent_;QPointer<SocialNameLinkDialog> dialog_;
 QTimer refresh_{this};
public:
 SocialDisplaySession(social::PublicDisplayRpc rpc,social::NameLinkWalletSigner signer,social::Identity identity,QString recovery,SocialView* view,QObject* parent):
 QObject(parent),rpc_(std::move(rpc)),identity_(std::move(identity)),recovery_(std::move(recovery)),view_(view),resolver_(rpc_,identity_,this),consent_(rpc_,std::move(signer),identity_,this){
  consent_.changed=[this]{if(view_)view_->refresh();};
  // Display-link events have their own stream, not the Chat history cursor.
  // Refresh the visible Chat only; this reads local Core, never whole history.
  refresh_.setInterval(5000);connect(&refresh_,&QTimer::timeout,this,[this]{if(!view_||!view_->isVisible())return;auto* tabs=view_->findChild<QTabWidget*>("pgn-social-sections");if(tabs&&tabs->currentIndex()==0)view_->refresh();});refresh_.start();
 }
 ~SocialDisplaySession()override{if(dialog_)delete dialog_;}
 void decorate(social::Snapshot snapshot,social::Services::Reply done){resolver_.decorate(std::move(snapshot),std::move(done));}
 void open(QString key={}){
  if(!view_)return;const QPointer<SocialDisplaySession> weak(this);
  rpc_("getsocialdisplaylinks",{QJsonValue(QJsonArray{identity_.subject})},[weak,key](QJsonValue value,QString error){
   if(!weak||!weak->view_)return;const auto root=value.toObject();const auto rows=root["results"].toArray();
   if(!error.isEmpty()||root["state"]=="loading"||root["schema"]!="PGN_SOCIAL_CURRENT_DISPLAY_1"||root["genesis"]!=weak->identity_.genesis||rows.size()!=1||rows[0].toObject()["social_script"]!=weak->identity_.subject){QMessageBox::information(weak->view_,"Public name association",error.isEmpty()?QString("This Core cannot currently resolve public display links. Chat remains available; no name was published."):error);return;}
   const auto row=rows[0].toObject();const auto state=row["state"].toString();
   if(state!="unlinked"&&state!="linked"&&state!="withdrawn"){QMessageBox::information(weak->view_,"Public name association","The current proof state is incomplete or conflicting. The script fallback is retained; no new head is chosen automatically.");return;}
   const QString previous=state=="unlinked"?QString(64,'0'):row["certificate_id"].toString();
   if(!social::hexField(previous,64))return;
   if(weak->dialog_)delete weak->dialog_;
   const auto native=key.isEmpty()?row["operator_script"].toString():key;
   weak->dialog_=new SocialNameLinkDialog(weak->consent_.services(),{weak->identity_,weak->identity_.subject,native,"bind",previous},weak->recovery_,weak->view_);
   weak->dialog_->restorePublicPacket();weak->dialog_->show();weak->dialog_->raise();
  });
 }
};
}
