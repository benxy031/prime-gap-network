#pragma once
#include "ui/component_versions_dialog.h"
#include "update/component_update_service.h"
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QFileInfo>
#include <QPointer>
#include <QToolButton>
#include <QStyle>
#include <QTimer>
#include <QDateTime>
#include <QtConcurrent/QtConcurrentRun>
#include <functional>
#include <stdexcept>

namespace pgn::ui {
// A compact view of the SAME verified local inventory used by Components.
// Unknown remote metadata is deliberately never rendered as "up to date".
class ComponentStatusStrip final:public QWidget {
public:
 explicit ComponentStatusStrip(ComponentVersionsContext context,QWidget* parent=nullptr)
  :QWidget(parent),context_(std::move(context)){
  setObjectName("pgn-component-status-strip");
  auto* layout=new QHBoxLayout(this);layout->setContentsMargins(0,0,0,0);layout->setSpacing(3);
  updates_=new QToolButton(this);updates_->setObjectName("pgn-component-update-status");updates_->setText("Updates · not checked");updates_->setAccessibleName("Software updates and installed versions");updates_->setCursor(Qt::PointingHandCursor);layout->addWidget(updates_);
  connect(updates_,&QToolButton::clicked,this,[this]{openComponents();});
  setStyleSheet("QToolButton{font-family:'Segoe UI';font-size:12px;color:#a6c1c8;background:#0c2028;border:1px solid #20414d;border-radius:5px;padding:4px 7px;}QToolButton:hover{color:#efffff;border-color:#46cbb5;background:#12323a;}QToolButton:focus{border-color:#f1c572;}QToolButton[state=current]{color:#9de1d1;}QToolButton[state=unidentified],QToolButton[state=update],QToolButton[state=stale],QToolButton[state=unknown],QToolButton[state=ahead]{color:#edc17c;}");
  refreshTooltip();
  profileIdentity_=componentProfileIdentity(context_);refreshLocal();
  if(!context_.coreProfilePath.isEmpty()){
   auto* profileClock=new QTimer(this);profileClock->setInterval(1000);profileClock->setTimerType(Qt::CoarseTimer);
   connect(profileClock,&QTimer::timeout,this,[this]{
    const auto identity=componentProfileIdentity(context_);if(identity==profileIdentity_)return;
    profileIdentity_=identity;++profileRevision_;context_=refreshComponentVersionsContext(context_);
    updateRequest_={};refreshLocal();if(dialog_)dialog_->refreshLocal();
   });profileClock->start();
  }
  // A running client must discover a later release without reopening the menu.
  // Signed metadata only: optional installation is never triggered by this clock.
  if(context_.fetchOnOpen){
   auto* refresh=new QTimer(this);refresh->setObjectName("pgn-component-update-refresh");
   refresh->setTimerType(Qt::CoarseTimer);refresh->setInterval(10*60*1000);
   connect(refresh,&QTimer::timeout,this,[this]{if(!dialog_)checkSignedUpdates();});
   refresh->start();
  }
 }
 void refreshLocal(){
  if(localBusy_){localAgain_=true;return;}
  localBusy_=true;context_=refreshComponentVersionsContext(context_);const auto revision=profileRevision_;
  auto* watcher=new QFutureWatcher<QJsonObject>(this);
  connect(watcher,&QFutureWatcher<QJsonObject>::finished,this,[this,watcher,revision]{
   localBusy_=false;const auto value=watcher->result();watcher->deleteLater();
   if(revision==profileRevision_)presentLocal(value);
   if(localAgain_||revision!=profileRevision_){localAgain_=false;refreshLocal();return;}
   if(context_.fetchOnOpen)checkSignedUpdates();
  });
  watcher->setFuture(QtConcurrent::run([context=context_]{return inspectLocalComponentVersions(context);}));
 }
 void presentLocal(const QJsonObject& snapshot){
  setProperty("pgn_component_local_snapshot",snapshot);
  if(remoteFresh_)presentPublished(property("pgn_component_authenticated_catalog").toJsonObject());
  else refreshTooltip();
 }
 void presentPublished(const QJsonObject& catalog){
  setProperty("pgn_component_release_catalog",catalog);
  if(catalog.value("schema")=="PGN_AUTHENTICATED_COMPONENT_VIEW_V1"&&catalog.value("publisher_authenticated").toBool()){
   remoteFresh_=true;
   setProperty("pgn_component_authenticated_catalog",catalog);
   setProperty("pgn_component_last_checked_utc",catalog.value("checked_at_utc").toString());
   QMap<QString,QString> available;
   for(const auto& value:catalog.value("components").toArray()){
    const auto row=value.toObject();
    available.insert(row.value("id").toString(),row.value("version").toString());
   }
   int changes=0,unknown=0;
   for(const auto& id:componentIds()){
    const auto row=installed(id);
    const auto comparison=row.value("state")=="matched_local_manifest"?
        pgn::update::componentVersionComparison(row.value("version").toString(),available.value(id)):std::optional<int>{};
    if(!comparison||*comparison>0)++unknown;
    else if(*comparison<0)++changes;
   }
   if(changes)presentUpdateStatus(QString("Updates · %1 available").arg(changes),"update");
   else if(unknown)presentUpdateStatus("Updates · partially verified","unknown");
   else presentUpdateStatus("Updates · up to date","current");
   return;
  }
  if(property("pgn_component_authenticated_catalog").isValid())return;
  const bool available=catalog.value("component_versions_available").toBool();
  presentUpdateStatus(available?"Updates · review":"Updates · versions unavailable","unknown",
     available?QString{}:QString("No verified component version catalogue is available."));
 }
 void presentUpdateChecking(){
  remoteFresh_=false;
  presentUpdateStatus("Updates · checking","checking");
 }
 void presentUpdateFailure(const QString& error){
  remoteFresh_=false;
  const auto checked=property("pgn_component_last_checked_utc").toString();
  presentUpdateStatus(error.contains("GitHub update service")?"Updates · GitHub unavailable":
     error.contains("Internet connection")?"Updates · connection failed":"Updates · check failed","stale",
     error+(checked.isEmpty()?QString("\nNo successful version check yet."):QString("\nLast successful check: ")+checked));
 }
 void checkSignedUpdates(){
  try{
   const auto root=QFileInfo(context_.manifestPath).absolutePath();auto deployment=pgn::update::componentUpdateDeployment(root,context_.testnet);
   if(!context_.genesisHash.isEmpty()&&context_.genesisHash!=deployment.context.value("genesis_hash").toString())throw std::runtime_error("Update deployment differs from the active Core epoch");
   updateRequest_=pgn::update::componentUpdateRequest(deployment,property("pgn_component_local_snapshot").toJsonObject(),root,context_.mutableDataRoot,"check",{"ui","core","gpu","cpu"});
   if(!updateService_){
    updateService_=new pgn::update::ComponentUpdateService(std::move(deployment),this);
    updateService_->onResult=[this](const QJsonObject& value){presentPublished(value);};
    updateService_->onError=[this](const QString& error){presentUpdateFailure(error);};
   }
   if(updateService_->start(updateRequest_))presentUpdateChecking();
  }catch(const std::exception& error){presentUpdateFailure(QString::fromUtf8(error.what()));}
 }
 void openComponents(){
  if(dialog_){dialog_->show();dialog_->raise();dialog_->activateWindow();return;}
  dialog_=new ComponentVersionsDialog(context_,this);dialog_->setAttribute(Qt::WA_DeleteOnClose);
  const QPointer<ComponentStatusStrip> guard(this);
  dialog_->onLocalSnapshot=[guard](const QJsonObject& value){if(guard)guard->presentLocal(value);};
  dialog_->onPublishedCatalog=[guard](const QJsonObject& value){if(guard)guard->presentPublished(value);};
  dialog_->onPublishedFailure=[guard](const QString& error){if(guard)guard->presentUpdateFailure(error);};
  dialog_->show();
 }
private:
 static const QStringList& componentIds(){
  static const QStringList ids{"ui","core","gpu","cpu"};
  return ids;
 }
 QJsonObject installed(const QString& id)const{
  const auto components=property("pgn_component_local_snapshot").toJsonObject().value("components").toArray();
  for(const auto& value:components)if(value.toObject().value("id")==id)return value.toObject();
  return {};
 }
 void refreshTooltip(){
  QStringList lines;
  if(!updateDetail_.isEmpty())lines<<updateDetail_;
  for(const auto& id:componentIds()){
   const auto row=installed(id);
   const auto version=row.value("state")=="matched_local_manifest"?row.value("version").toString():
       row.isEmpty()?QString("checking"):row.value("code_root").toString().isEmpty()?QString("not configured"):QString("unverified");
   lines<<id.toUpper()+" · "+version;
  }
  lines<<"Open to manage software updates.";
  updates_->setToolTip(lines.join('\n'));
 }
 void presentUpdateStatus(const QString& label,const char* state,const QString& detail={}){
  updates_->setText(label);
  updates_->setProperty("state",state);
  updates_->style()->unpolish(updates_);
  updates_->style()->polish(updates_);
  updateDetail_=detail;
  refreshTooltip();
 }
 ComponentVersionsContext context_;QToolButton* updates_{};QPointer<ComponentVersionsDialog> dialog_;
 QString updateDetail_;
 pgn::update::ComponentUpdateService* updateService_{};QJsonObject updateRequest_;bool remoteFresh_{},localBusy_{},localAgain_{};QByteArray profileIdentity_;quint64 profileRevision_{};
};
}
