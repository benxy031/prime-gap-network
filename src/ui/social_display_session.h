#pragma once
#include "social/social_display_consent.h"
#include "ui/social_name_link_dialog.h"
#include "ui/social_view.h"
#include <QMessageBox>
#include <QTabWidget>
#include <QCursor>
namespace pgn::ui {
// One session per verified wallet binding, owned by its CoreSocialService.
// The Native index is authoritative; only a recoverable public packet is local.
class SocialDisplaySession final:public QObject {
 social::PublicDisplayRpc rpc_;social::Identity identity_;QString recovery_;
 QPointer<SocialView> view_;social::NativeDisplayResolver resolver_;
 social::CurrentDisplayConsent consent_;QPointer<SocialNameLinkDialog> dialog_;
 QTimer refresh_{this};quint64 openGeneration_{};bool opening_{};QString pendingKey_;
 void finishOpen(){opening_=false;pendingKey_.clear();if(view_)view_->unsetCursor();}
 void requestOpen(QString key,quint64 generation,int remaining){
  if(!view_||!view_->isVisible()){finishOpen();return;}
  const QPointer<SocialDisplaySession> weak(this);
  rpc_("getsocialdisplaylinks",{QJsonValue(QJsonArray{identity_.subject})},[weak,key,generation,remaining](QJsonValue value,QString error){
   if(!weak||generation!=weak->openGeneration_||!weak->opening_||!weak->view_)return;
   if(!weak->view_->isVisible()){weak->finishOpen();return;}
   const auto root=value.toObject();const auto rows=root["results"].toArray();
   const bool context=root["schema"]=="PGN_SOCIAL_CURRENT_DISPLAY_1"&&root["genesis"]==weak->identity_.genesis;
   // NativeContext builds asynchronously. Loading deliberately has no rows or
   // anchor yet; it is neither an absent capability nor an unlinked identity.
   if(error.isEmpty()&&context&&root["state"]=="loading"){
    if(remaining>0)QTimer::singleShot(250,weak.data(),[weak,key,generation,remaining]{if(weak&&generation==weak->openGeneration_&&weak->opening_)weak->requestOpen(key,generation,remaining-1);});
    else{weak->finishOpen();QMessageBox::information(weak->view_,"Public name association","The Native name registry is still loading. Please retry this action. No name was published.");}
    return;
   }
   if(!error.isEmpty()||!context||rows.size()!=1||rows[0].toObject()["social_script"]!=weak->identity_.subject){weak->finishOpen();QMessageBox::information(weak->view_,"Public name association",error.isEmpty()?QString("This Core cannot currently resolve public display links. Chat remains available; no name was published."):error);return;}
   const auto row=rows[0].toObject();const auto state=row["state"].toString();
   const auto anchor=root["bestblockhash"].toString();const auto height=root["height"].toDouble(-1);
   if((state!="unlinked"&&state!="linked"&&state!="withdrawn")||row["complete"]!=true||!social::hexField(anchor,64)||height<0||height>2147483647||height!=int(height)){weak->finishOpen();QMessageBox::information(weak->view_,"Public name association","The current proof state is incomplete or conflicting. The script fallback is retained; no new head is chosen automatically.");return;}
   const QString previous=state=="unlinked"?QString(64,'0'):row["certificate_id"].toString();
   if(!social::hexField(previous,64)){weak->finishOpen();return;}
   weak->rpc_("getblockhash",{int(height)},[weak,key,generation,row,anchor,previous](QJsonValue current,QString error){
    if(!weak||generation!=weak->openGeneration_||!weak->opening_||!weak->view_)return;
    if(!weak->view_->isVisible()){weak->finishOpen();return;}
    weak->finishOpen();
    if(!error.isEmpty()||current.toString()!=anchor){QMessageBox::information(weak->view_,"Public name association","The Native snapshot changed. Please retry this action; no name was published.");return;}
    if(weak->dialog_)delete weak->dialog_;
    const auto native=key.isEmpty()?row["operator_script"].toString():key;
    weak->dialog_=new SocialNameLinkDialog(weak->consent_.services(),{weak->identity_,weak->identity_.subject,native,"bind",previous},weak->recovery_,weak->view_);
    weak->dialog_->restorePublicPacket();weak->dialog_->show();weak->dialog_->raise();
   });
  });
 }
public:
 SocialDisplaySession(social::PublicDisplayRpc rpc,social::NameLinkWalletSigner signer,social::Identity identity,QString recovery,SocialView* view,QObject* parent):
 QObject(parent),rpc_(std::move(rpc)),identity_(std::move(identity)),recovery_(std::move(recovery)),view_(view),resolver_(rpc_,identity_,this),consent_(rpc_,std::move(signer),identity_,this){
  consent_.changed=[this]{if(view_)view_->refresh();};
  // Display-link events have their own stream, not the Chat history cursor.
  // Refresh the visible Chat only; this reads local Core, never whole history.
  refresh_.setInterval(5000);connect(&refresh_,&QTimer::timeout,this,[this]{if(!view_||!view_->isVisible())return;auto* tabs=view_->findChild<QTabWidget*>("pgn-social-sections");if(tabs&&tabs->currentIndex()==0)view_->refresh();});refresh_.start();
 }
 ~SocialDisplaySession()override{finishOpen();if(dialog_)delete dialog_;}
 void decorate(social::Snapshot snapshot,social::Services::Reply done){resolver_.decorate(std::move(snapshot),std::move(done));}
 void open(QString key={}){
  if(!view_||!view_->isVisible()||(opening_&&pendingKey_==key))return;
  opening_=true;pendingKey_=key;view_->setCursor(Qt::BusyCursor);
  // At most one request in flight per explicit opening, 10 seconds of loading
  // retries, and no signing/submission. A changed intent invalidates old work.
  const auto generation=++openGeneration_;const QPointer<SocialDisplaySession> weak(this);
  QTimer::singleShot(10000,this,[weak,generation]{if(!weak||generation!=weak->openGeneration_||!weak->opening_)return;weak->finishOpen();if(weak->view_&&weak->view_->isVisible())QMessageBox::information(weak->view_,"Public name association","The Native name registry is still loading. Please retry this action. No name was published.");});
  requestOpen(std::move(key),generation,40);
 }
};
}
