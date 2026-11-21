#include "app/horizon_user_paths.h"
#include "ui/component_versions_dialog.h"
#include "wallet/installed_core_binding.h"
#include "update/component_catalog.h"
#include <QSignalBlocker>
#include <QDateTime>
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QFutureWatcher>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QColor>
#include <QPushButton>
#include <QProgressBar>
#include <QTableWidget>
#include <QTimer>
#include <QFontMetrics>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
namespace pgn::ui {
namespace {
const QStringList ids{"ui","core","gpu","cpu"},names{"Interface","Core","GPU engine (local)","CPU engine (local)"};
QString codeDirectory(const QString& path){const QFileInfo info(path);return info.isDir()?info.canonicalFilePath():QString{};}
QLabel* label(const QString& text={}){auto* value=new QLabel(text);value->setTextFormat(Qt::PlainText);value->setWordWrap(true);return value;}
QTableWidget* table(const QStringList& headers){auto* value=new QTableWidget(0,headers.size());value->setHorizontalHeaderLabels(headers);value->verticalHeader()->hide();value->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);value->horizontalHeader()->setStretchLastSection(true);value->horizontalHeader()->setFixedHeight(32);value->setEditTriggers(QAbstractItemView::NoEditTriggers);value->setSelectionBehavior(QAbstractItemView::SelectRows);value->setSelectionMode(QAbstractItemView::SingleSelection);return value;}
void identifyInstalledCpu(ComponentVersionsContext& context,const QString& app){
 // This dialog inventories the installed local update components, just as for
 // GPU/Core/UI. A remote worker profile describes execution, not local bytes.
 const auto root=QDir(app).filePath("EngineRuntime/cpu");
 context.codeRoots.insert("cpu",codeDirectory(root));
 auto program=QDir(root).filePath("horizon-cpu-controller");
#ifdef Q_OS_WIN
 program+=".exe";
#endif
 const QFileInfo file(program);
 if(file.isAbsolute()&&file.isFile()&&file.isExecutable())context.primaryPrograms.insert("cpu",file.canonicalFilePath());
}
}
ComponentVersionsContext walletComponentVersionsContext(const QString& productRoot,const QString& runtimeDirectory,bool testnet){
 ComponentVersionsContext context;context.testnet=testnet;const auto app=QCoreApplication::applicationDirPath();context.manifestPath=QDir(app).filePath("components.json");context.codeRoots.insert("ui",codeDirectory(app));context.primaryPrograms.insert("ui",QCoreApplication::applicationFilePath());
 context.applicationRoot=pgn::app::cleanApplicationRoot(productRoot);context.mutableDataRoot=pgn::app::mutableDataRoot(productRoot);
 const auto engine=QDir(app).filePath("EngineRuntime");
 context.codeRoots.insert("gpu",codeDirectory(engine));
 auto gpuHost=QDir(engine).filePath("network/horizon-engine-host");
#ifdef Q_OS_WIN
 gpuHost+=".exe";
#endif
 context.primaryPrograms.insert("gpu",gpuHost);
 identifyInstalledCpu(context,app);
 context.coreProfilePath=QDir(runtimeDirectory).filePath("wallet-core.json");
 return refreshComponentVersionsContext(std::move(context));
}
QByteArray componentProfileIdentity(const ComponentVersionsContext& context){
 if(context.coreProfilePath.isEmpty())return {};
 return pgn::wallet::readInstalledCoreProfile(context.coreProfilePath).identity;
}
ComponentVersionsContext refreshComponentVersionsContext(ComponentVersionsContext context){
 if(context.coreProfilePath.isEmpty())return context;
 context.codeRoots.remove("core");context.primaryPrograms.remove("core");context.genesisHash.clear();
 const auto profile=pgn::wallet::readInstalledCoreProfile(context.coreProfilePath);
 context.profileIdentity=profile.identity;
 const auto binding=pgn::wallet::installedCoreBinding(context.applicationRoot,profile,context.testnet);
 if(!binding.program.isEmpty()){
  context.genesisHash=binding.genesis;const QFileInfo executable(binding.program);
  context.codeRoots.insert("core",executable.canonicalPath());context.primaryPrograms.insert("core",executable.canonicalFilePath());
 }
 return context;
}
static QJsonObject inspectResolvedLocalComponentVersions(const ComponentVersionsContext& context,ComponentInventory mode){
 QJsonObject snapshot{{"schema","PGN_LOCAL_COMPONENT_VIEW_V1"},{"manifest_state","absent"},{"components",QJsonArray{}},{"release_version",QJsonValue{}},{"published",QJsonValue{}}};QJsonObject declarations;QString reason="No local component manifest is present.";
 QFile file(context.manifestPath);
 if(file.exists()){
  snapshot.insert("manifest_state","invalid");reason="The local component manifest is unavailable or invalid.";
  if(file.open(QIODevice::ReadOnly)&&file.size()<=2*1024*1024){QJsonParseError parse;const auto doc=QJsonDocument::fromJson(file.readAll(),&parse);const auto manifest=doc.object();const auto components=manifest.value("components").toArray();bool valid=parse.error==QJsonParseError::NoError&&doc.isObject()&&manifest.value("schema")=="PGN_COMPONENT_MANIFEST_V1"&&components.size()<=4&&manifest.value("components").isArray()&&manifest.value("published").isBool()&&(manifest.value("network")=="testnet"||manifest.value("network")=="main");
   for(const auto& value:components){const auto entry=value.toObject();const auto id=entry.value("id").toString();if(!pgn::update::componentId(id)||declarations.contains(id)){valid=false;break;}declarations.insert(id,entry);}
   if(valid){snapshot.insert("manifest_state","valid");snapshot.insert("published",manifest.value("published"));snapshot.insert("network",manifest.value("network"));if(pgn::update::versionText(manifest.value("release_version").toString()))snapshot.insert("release_version",manifest.value("release_version"));reason="Component has no declared matching local version.";}
   else declarations={};
  }
 }
 QJsonArray rows;
 for(const auto& id:ids){QJsonObject observed{{"id",id},{"state","unidentified"},{"version",QJsonValue{}},{"reason",reason}};const auto root=context.codeRoots.value(id);if(!root.isEmpty()&&declarations.contains(id)){
   const auto declaration=declarations.value(id).toObject();
   if(mode==ComponentInventory::Files)observed=pgn::update::inspectComponent(root,declaration,context.primaryPrograms.value(id));
   else {
    const auto program=QFileInfo(context.primaryPrograms.value(id));
    const auto version=declaration.value("version").toString();
    if(program.isFile()&&program.isExecutable()&&!program.canonicalFilePath().isEmpty()&&pgn::update::versionText(version)){
     observed.insert("state","reported_local_manifest");observed.insert("version",version);
     observed.insert("reason","Version declared by the local manifest. Files are checked when preparing an update.");
    }
   }
  }else if(root.isEmpty())observed.insert("reason","The local code root is unknown.");observed.insert("code_root",root);rows.append(observed);}
 snapshot.insert("inventory_mode",mode==ComponentInventory::Manifest?"manifest":"files");
 snapshot.insert("components",rows);return snapshot;
}
QJsonObject inspectLocalComponentVersions(const ComponentVersionsContext& context,ComponentInventory mode){
 return inspectResolvedLocalComponentVersions(refreshComponentVersionsContext(context),mode);
}
ComponentVersionsDialog::ComponentVersionsDialog(ComponentVersionsContext context,QWidget* parent):QDialog(parent),context_(std::move(context)){
 setObjectName("pgn-component-versions");setWindowTitle("Software updates");resize(850,440);
 auto font=QFontDatabase::systemFont(QFontDatabase::GeneralFont);
#ifdef Q_OS_WIN
 font.setFamily("Segoe UI");
#endif
 font.setPointSize(10);setFont(font);
 setStyleSheet("QDialog{background:#061219;color:#dcefeb;}QLabel,QTableWidget,QPushButton{color:#dcefeb;}QTableView::indicator{width:14px;height:14px;border:1px solid #5ea596;border-radius:3px;background:#102c35;}QTableView::indicator:checked{background:#71d8bd;}QLabel,QTableWidget,QPushButton,QHeaderView::section{font-family:'"+font.family()+"';font-size:10pt;}QTableWidget{background:#091d25;border:1px solid #28505e;border-radius:8px;gridline-color:#173741;}QHeaderView::section{background:#102c35;color:#9fcacb;border:0;padding:8px;}QPushButton{padding:8px 14px;border:1px solid #2d5c68;border-radius:6px;background:#102c35;}QPushButton:hover{border-color:#5ad9bd;}QPushButton:disabled{color:#65818b;}QPushButton#pgn-components-prepare,QPushButton#pgn-components-install{background:#e5b96b;color:#14202a;font-weight:600;}");
 auto* box=new QVBoxLayout(this);box->setSizeConstraint(QLayout::SetMinimumSize);box->setContentsMargins(20,18,20,18);box->setSpacing(12);
 auto* title=label("Keep Horizon up to date");title->setStyleSheet("font-size:18px;font-weight:600;color:#f2f9f7;");box->addWidget(title);
 box->addWidget(label("Choose individual components or select every available update. Nothing installs automatically."));
 localSummary_=label("Checking installed files…");localSummary_->setObjectName("pgn-components-local-summary");box->addWidget(localSummary_);
 components_=table({"Component","Installed","Available","Status"});components_->setObjectName("pgn-components-table");components_->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);components_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Fixed);components_->setColumnWidth(0,200);components_->verticalHeader()->setDefaultSectionSize(34);components_->setMinimumHeight(177);box->addWidget(components_,1);
 auto* line=new QHBoxLayout;selectAll_=new QPushButton("Select available updates");selectAll_->setObjectName("pgn-components-select-all");refresh_=new QPushButton("Check for updates");refresh_->setObjectName("pgn-components-check");line->addWidget(selectAll_);line->addStretch();line->addWidget(refresh_);box->addLayout(line);
 status_=label("Available versions are checked against signed publisher metadata.");status_->setObjectName("pgn-components-status");status_->setAlignment(Qt::AlignLeft|Qt::AlignTop);
 auto* statusViewport=new QScrollArea(this);statusViewport->setWidget(status_);statusViewport->setWidgetResizable(true);
 statusViewport->setFrameShape(QFrame::NoFrame);statusViewport->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
 statusViewport->viewport()->setStyleSheet("background:#061219;");status_->setStyleSheet("background:#061219;color:#dcefeb;");
 statusViewport->setFixedHeight(fontMetrics().lineSpacing()*3+6);box->addWidget(statusViewport);
 progress_=new QProgressBar;progress_->setObjectName("pgn-components-progress");progress_->setMinimumHeight(22);progress_->hide();box->addWidget(progress_);
 auto* controls=new QHBoxLayout;auto* close=new QPushButton("Close");controls->addWidget(close);controls->addStretch();cancel_=new QPushButton("Cancel download");cancel_->hide();controls->addWidget(cancel_);
 recover_=new QPushButton("Restore previous version");recover_->setObjectName("pgn-components-recover");recover_->hide();controls->addWidget(recover_);
 prepare_=new QPushButton("Download selected");prepare_->setObjectName("pgn-components-prepare");prepare_->setEnabled(false);controls->addWidget(prepare_);install_=new QPushButton("Restart and install");install_->setObjectName("pgn-components-install");install_->hide();controls->addWidget(install_);box->addLayout(controls);
 connect(close,&QPushButton::clicked,this,&QDialog::close);connect(refresh_,&QPushButton::clicked,this,[this]{refreshLocal();});
 connect(selectAll_,&QPushButton::clicked,this,[this]{for(int row=0;row<components_->rowCount();++row){auto* item=components_->item(row,0);if(item&&(item->flags()&Qt::ItemIsUserCheckable))item->setCheckState(Qt::Checked);}});
 connect(components_,&QTableWidget::itemChanged,this,[this]{if(!busy_&&!installStarted_){preparedRequest_={};install_->hide();prepare_->show();prepare_->setEnabled(!selected().isEmpty());updateRecovery();}});
 connect(prepare_,&QPushButton::clicked,this,[this]{prepareSelected();});connect(install_,&QPushButton::clicked,this,[this]{installPrepared();});connect(cancel_,&QPushButton::clicked,this,[this]{
  if(preparationInventory_){preparationInventory_.clear();setBusy(false);status_->setText("Update cancelled. Installed components are unchanged.");return;}
  if(service_)service_->cancel();
 });
 connect(recover_,&QPushButton::clicked,this,[this]{if(busy_||installStarted_||!ensureService())return;preparedRequest_=request("recover",{});installPrepared();});
 resultPoll_=new QTimer(this);resultPoll_->setInterval(500);connect(resultPoll_,&QTimer::timeout,this,[this]{
  const auto value=pgn::update::readDetachedComponentInstallResult(property("pgn_update_result_file").toString());
  if(pgn::update::validComponentUpdateProgress(value)){presentProgress(value);return;}
  const auto schema=value.value("schema").toString();
  const bool stopped=schema=="PGN_UPDATE_INSTALL_RESULT_V1"
      &&value.value("publisher_authenticated")==QJsonValue(true)
      &&value.value("installation_committed")==QJsonValue(false)
      &&value.value("installation_performed")==QJsonValue(false)
      &&value.value("process_control_performed")==QJsonValue(true);
  if(schema=="PGN_UPDATE_HELPER_ERROR_V1"||stopped){
   resultPoll_->stop();installStarted_=false;setBusy(false);preparedRequest_={};
   install_->hide();prepare_->show();updateRecovery();
   if(!stopped)status_->setText(value.value("error").toString("Installation could not finish. The previous version is retained."));
   else if(recover_->isHidden())status_->setText("Installation did not complete. Check for updates again.");
   if(isHidden()&&testAttribute(Qt::WA_DeleteOnClose))deleteLater();
   else if(recover_->isHidden()&&inventoryProfileIdentity_!=componentProfileIdentity(context_))refreshLocal();
  }
  else if(schema=="PGN_UPDATE_RECOVERY_RESULT_V1"&&value.value("recovery_performed").toBool()){
   resultPoll_->stop();installStarted_=false;setBusy(false);preparedRequest_={};install_->hide();
   if(isHidden()&&testAttribute(Qt::WA_DeleteOnClose))deleteLater();
   else refreshLocal();
   status_->setText("The previous version has been restored and verified.");
  }
 });
 QTimer::singleShot(0,this,[this]{refreshLocal();});
}
ComponentVersionsDialog::~ComponentVersionsDialog(){closing_=true;if(service_){service_->onResult={};service_->onError={};service_->onStatus={};service_->onProgress={};if(service_->busy())service_->cancel();}}
void ComponentVersionsDialog::closeEvent(QCloseEvent* event){
 if(resultPoll_->isActive()){
  hide();
  event->ignore();
  return;
 }
 QDialog::closeEvent(event);
}
void ComponentVersionsDialog::reject(){
 if(resultPoll_->isActive()){
  hide();
  return;
 }
 QDialog::reject();
}
QStringList ComponentVersionsDialog::selected()const{
 QStringList result;for(int row=0;row<components_->rowCount();++row){const auto* item=components_->item(row,0);if(item&&(item->flags()&Qt::ItemIsUserCheckable)&&item->checkState()==Qt::Checked)result.append(item->data(Qt::UserRole).toString());}return result;
}
void ComponentVersionsDialog::setBusy(bool busy){
 busy_=busy;
 components_->setColumnCount(4);
 setProperty("pgn_update_progress",QVariant{});
 progress_->setToolTip({});
 progress_->setFormat(busy?QStringLiteral("Starting…"):QString{});
 progress_->setRange(0,busy?0:1);
 progress_->reset();
 progress_->setVisible(busy);
 refresh_->setEnabled(!busy);components_->setEnabled(!busy);selectAll_->setEnabled(!busy);
 prepare_->setEnabled(!busy&&!selected().isEmpty());install_->setEnabled(!busy&&!preparedRequest_.isEmpty());
 recover_->setEnabled(!busy);cancel_->setVisible(busy&&!installStarted_);
}
void ComponentVersionsDialog::presentProgress(const QJsonObject& value){
 if(!pgn::update::validComponentUpdateProgress(value))return;
 const auto stage=value.value("stage").toString(),component=value.value("component").toString();
 const QMap<QString,QString> labels{{"metadata","Checking signed versions"},{"download","Downloading shared package"},{"verify","Verifying components"},{"prepare","Preparing installation"},{"drain","Saving mining checkpoints and stopping Core"},{"startup","Restarting and checking Horizon"},{"committed","Installation completed"}};
 const auto done=value.value("completed").toInteger(),total=value.value("total").toInteger();
 const auto set=[&](QProgressBar* bar){bar->show();if(total>0&&total<=2147483647){bar->setRange(0,int(total));bar->setValue(int(done));const bool megabytes=stage=="download"||stage=="verify";const auto amount=megabytes?QString("%1 / %2 MB").arg(double(done)/1000000.0,0,'f',1).arg(double(total)/1000000.0,0,'f',1):QString("%1 / %2%3").arg(done).arg(total).arg(stage=="prepare"?" files":"");bar->setFormat(bar==progress_?labels.value(stage)+" · %p% · "+amount:amount);bar->setToolTip(labels.value(stage)+" · "+amount);}else{bar->setRange(0,0);bar->setFormat(labels.value(stage));}};
 if(component.isEmpty()){set(progress_);status_->setText(labels.value(stage));}
 else{const int row=ids.indexOf(component);if(row>=0){components_->setColumnCount(5);components_->setHorizontalHeaderItem(4,new QTableWidgetItem("Progress"));auto* cell=components_->cellWidget(row,4);auto* bar=cell?cell->findChild<QProgressBar*>():nullptr;if(!bar){auto* frame=new QWidget;
   auto* inside=new QHBoxLayout(frame);inside->setContentsMargins(8,5,8,5);
   bar=new QProgressBar(frame);bar->setObjectName("pgn-component-row-progress");bar->setMinimumWidth(132);
   bar->setAlignment(Qt::AlignCenter);bar->setStyleSheet("QProgressBar#pgn-component-row-progress{background:#0d303a;"
    "color:#e3ffff;border:1px solid #2b6570;border-radius:5px;text-align:center;font-size:12px;font-weight:600;}"
    "QProgressBar#pgn-component-row-progress::chunk{background:#207d73;border-radius:4px;}");
   inside->addWidget(bar);components_->setCellWidget(row,4,frame);}set(bar);}}
 setProperty("pgn_update_progress",value);
}
void ComponentVersionsDialog::updateRecovery(){
 const bool needed=!context_.genesisHash.isEmpty()&&pgn::update::componentUpdateNeedsRecovery(context_.genesisHash);
 recover_->setVisible(needed);if(needed){prepare_->setEnabled(false);install_->hide();status_->setText("An interrupted update needs recovery. Restore the previous version before installing another update. Wallets and results stay in their folders.");}
}
void ComponentVersionsDialog::refreshLocal(){
 if(localBusy_||busy_||installStarted_||closing_)return;
 context_=refreshComponentVersionsContext(context_);
 const auto profile=context_.profileIdentity;
 if(preparedRequest_.value("action")=="install"&&preparedProfileIdentity_!=profile)invalidatePreparedUpdate();
 localBusy_=true;refresh_->setEnabled(false);localSummary_->setText("Checking installed files…");
 auto* watcher=new QFutureWatcher<QJsonObject>(this);localWatcher_=watcher;const QPointer<ComponentVersionsDialog> weak(this);
 connect(watcher,&QFutureWatcher<QJsonObject>::finished,this,[weak,watcher,profile]{
  if(!weak||weak->closing_)return;
  weak->localBusy_=false;weak->localWatcher_=nullptr;
  const auto value=watcher->result();watcher->deleteLater();
  if(componentProfileIdentity(weak->context_)!=profile){weak->invalidatePreparedUpdate();return;}
  weak->inventoryProfileIdentity_=profile;weak->presentLocal(value);weak->refresh_->setEnabled(true);
  if(weak->context_.fetchOnOpen)weak->refreshPublished();
 });
 watcher->setFuture(QtConcurrent::run([context=context_]{return inspectResolvedLocalComponentVersions(context,ComponentInventory::Manifest);}));
}
void ComponentVersionsDialog::presentLocal(const QJsonObject& snapshot){localSnapshot_=snapshot;if(onLocalSnapshot)onLocalSnapshot(snapshot);localSummary_->setText(snapshot.value("manifest_state")=="valid"?"Installed versions from the local manifest. Files are checked before an update is prepared.":"Some installed versions could not be verified. Their update controls stay unavailable.");updateRows();updateRecovery();}
void ComponentVersionsDialog::updateRows(){
 const auto chosen=selected();const QSignalBlocker guard(components_);components_->setRowCount(4);
 for(int index=0;index<ids.size();++index){QJsonObject local,available;for(const auto& entry:localSnapshot_.value("components").toArray())if(entry.toObject().value("id")==ids[index])local=entry.toObject();for(const auto& entry:published_.value("components").toArray())if(entry.toObject().value("id")==ids[index])available=entry.toObject();
  const bool known=local.value("state")=="matched_local_manifest"||local.value("state")=="reported_local_manifest";const auto comparison=known&&publishedFresh_?pgn::update::componentVersionComparison(local.value("version").toString(),available.value("version").toString()):std::optional<int>{};
  auto* name=new QTableWidgetItem(names[index]);name->setData(Qt::UserRole,ids[index]);name->setFlags(Qt::ItemIsEnabled|Qt::ItemIsSelectable);const bool update=comparison&&*comparison<0;
  if(update){name->setFlags(name->flags()|Qt::ItemIsUserCheckable);name->setCheckState(chosen.isEmpty()||chosen.contains(ids[index])?Qt::Checked:Qt::Unchecked);}
  components_->setItem(index,0,name);components_->setItem(index,1,new QTableWidgetItem(known?local.value("version").toString():"Unverified"));components_->setItem(index,2,new QTableWidgetItem(available.value("version").toString("—")));
  auto* state=new QTableWidgetItem(!known?"Local version unverified":!publishedFresh_&&!available.isEmpty()?"Last check outdated":!comparison?"Not checked":*comparison<0?"Update available":*comparison==0?"Up to date":"Newer local build");state->setForeground(update?QColor("#e5b96b"):comparison&&*comparison==0?QColor("#70d8bb"):QColor("#a0b7c0"));state->setToolTip(local.value("reason").toString()+"\nLast successful check: "+published_.value("checked_at_utc").toString("not available"));components_->setItem(index,3,state);
 }
 prepare_->setEnabled(!busy_&&!selected().isEmpty());
}
QJsonObject ComponentVersionsDialog::request(const QString& action,const QStringList& selected)const{return pgn::update::componentUpdateRequest(deployment_,localSnapshot_,QFileInfo(context_.manifestPath).absolutePath(),context_.mutableDataRoot,action,selected);}
bool ComponentVersionsDialog::ensureService(){
 if(service_)return true;
 try{
  deployment_=pgn::update::componentUpdateDeployment(QFileInfo(context_.manifestPath).absolutePath(),context_.testnet);
  if(!context_.genesisHash.isEmpty()&&context_.genesisHash!=deployment_.context.value("genesis_hash").toString())throw std::runtime_error("Update repository belongs to another Core epoch.");
  service_=new pgn::update::ComponentUpdateService(deployment_,this);service_->onStatus=[this](const QString& text){status_->setText(text);};
  service_->onProgress=[this](const QJsonObject& value){presentProgress(value);};
  service_->onError=[this](const QString& error){presentUpdateFailure(error);};
  service_->onResult=[this](const QJsonObject& value){
   if(value.value("schema")=="PGN_AUTHENTICATED_COMPONENT_VIEW_V1"){presentPublished(value);return;}
   setBusy(false);if(value.value("schema")=="PGN_UPDATE_INSTALL_PREPARATION_V1"){
   try{preparedRequest_=request("install",selected());preparedRequest_.insert("activation",pgn::update::currentUpdateActivation(context_.applicationRoot,context_.mutableDataRoot));prepare_->hide();install_->show();install_->setEnabled(true);status_->setText("Download verified. Restart when you are ready; mining will pause during installation. Wallets and results keep their current folders.");}
   catch(const std::exception& error){preparedRequest_={};status_->setText(QString::fromUtf8(error.what()));}
  }};return true;
 }catch(const std::exception& error){presentUpdateFailure(QString::fromUtf8(error.what()));return false;}
}
void ComponentVersionsDialog::presentUpdateFailure(const QString& detail){
 publishedFresh_=false;setBusy(false);updateRows();
 const auto message=pgn::update::componentUpdateFailureMessage(detail);const auto checked=published_.value("checked_at_utc").toString();
 const auto summary=message.section(" Details: ",0,0);
 status_->setText(summary+(checked.isEmpty()?QString("\nNo successful version check yet."):QString("\nLast successful check: ")+checked));
 status_->setToolTip(message);
 if(onPublishedFailure)onPublishedFailure(message);updateRecovery();
}
void ComponentVersionsDialog::refreshPublished(){if(localBusy_||busy_||installStarted_||!ensureService())return;publishedFresh_=false;preparedRequest_={};install_->hide();prepare_->show();setBusy(true);updateRows();if(!service_->start(request("check",ids)))setBusy(false);}
void ComponentVersionsDialog::presentPublished(const QJsonObject& value,const QString& status){
 if(value.value("schema")!="PGN_AUTHENTICATED_COMPONENT_VIEW_V1"||!value.value("publisher_authenticated").toBool()){presentUpdateFailure("Available component versions could not be authenticated.");return;}
 setBusy(false);published_=value;publishedFresh_=true;if(!published_.contains("checked_at_utc"))published_.insert("checked_at_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODate));if(onPublishedCatalog)onPublishedCatalog(published_);updateRows();
 QString summary=status;
 if(summary.isEmpty()){
  bool newer=false,unknown=false;
  for(int row=0;row<components_->rowCount();++row){const auto text=components_->item(row,3)->text();newer|=text=="Newer local build";unknown|=text=="Local version unverified"||text=="Not checked"||text=="Last check outdated";}
  summary=!selected().isEmpty()?QString("Updates are available. Select components, download, then choose Restart and install.")
   :newer?QString("Your local build is newer than the published version. No downgrade will be installed.")
   :unknown?QString("No updates found for verified components. Some local versions could not be verified.")
   :QString("All components are up to date.");
 }
 status_->setText(summary);status_->setToolTip({});updateRecovery();
}
void ComponentVersionsDialog::prepareSelected(){
 if(busy_||localBusy_||recover_->isVisible()||!publishedFresh_||selected().isEmpty()||!ensureService())return;
 const auto set=selected();
#ifdef Q_OS_WIN
 setBusy(true);startPreparation(set);return;
#endif
 setBusy(true);status_->setText("Verifying installed files…");
 auto* inspection=new QFutureWatcher<QJsonObject>(this);preparationInventory_=inspection;
 const QPointer<ComponentVersionsDialog> weak(this);
 connect(inspection,&QFutureWatcher<QJsonObject>::finished,this,[weak,inspection,set]{
  const auto verified=inspection->result();inspection->deleteLater();
  if(!weak||weak->closing_||weak->preparationInventory_!=inspection)return;
  weak->preparationInventory_.clear();
  weak->presentLocal(verified);weak->startPreparation(set);
 });
 inspection->setFuture(QtConcurrent::run([context=context_]{return inspectResolvedLocalComponentVersions(context,ComponentInventory::Files);}));
}
void ComponentVersionsDialog::startPreparation(const QStringList& wanted){
 const auto selectedNow=selected();
 for(const auto& id:wanted)if(!selectedNow.contains(id)){
  presentUpdateFailure("The installed version of a selected component could not be verified. Check again before preparing the update.");
  return;
 }
 if(inventoryProfileIdentity_!=componentProfileIdentity(context_)){invalidatePreparedUpdate();return;}
 preparedProfileIdentity_=inventoryProfileIdentity_;
 status_->setText("Preparing the selected update…");
 if(!service_->start(request("prepare_install",wanted)))setBusy(false);
}
void ComponentVersionsDialog::invalidatePreparedUpdate(){
 preparedRequest_={};
 preparedProfileIdentity_.clear();
 install_->hide();
 prepare_->show();
 presentUpdateFailure("The wallet profile changed. Check for updates again.");
}
void ComponentVersionsDialog::installPrepared(){
 if(busy_||preparedRequest_.isEmpty()||installStarted_)return;
 if(preparedRequest_.value("action")=="install"&&preparedProfileIdentity_!=componentProfileIdentity(context_)){
  invalidatePreparedUpdate();
  return;
 }
 try{const auto path=pgn::update::startDetachedComponentInstall(deployment_,preparedRequest_);setProperty("pgn_update_result_file",path);installStarted_=true;setBusy(true);status_->setText(preparedRequest_.value("action")=="recover"?"Restoring the previous version, then restarting Horizon…":"Verifying the installation, then restarting Horizon…");resultPoll_->start();}
 catch(const std::exception& error){status_->setText(QString::fromUtf8(error.what()));}
}
}
