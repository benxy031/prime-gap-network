#include "ui/component_versions_dialog.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include <QTableWidget>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QTemporaryDir>
#include <QJsonArray>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QStandardPaths>
#include <QUuid>
#include "update/update_paths.h"
#include <iostream>
#include <stdexcept>
int main(int argc,char** argv){
 QApplication app(argc,argv);int checks=0;const auto check=[&](bool ok){++checks;if(!ok)throw std::runtime_error("Update dialog check "+std::to_string(checks));};
 QStandardPaths::setTestModeEnabled(true);app.setApplicationName("PGN-Recovery-QA-"+QUuid::createUuid().toString(QUuid::Id128));
 pgn::ui::ComponentVersionsContext context;context.testnet=true;context.fetchOnOpen=false;
 pgn::ui::ComponentVersionsDialog dialog(context);dialog.show();
 QEventLoop settle;QTimer::singleShot(100,&settle,&QEventLoop::quit);settle.exec();
 auto* table=dialog.findChild<QTableWidget*>("pgn-components-table");auto* download=dialog.findChild<QPushButton*>("pgn-components-prepare");auto* install=dialog.findChild<QPushButton*>("pgn-components-install");
 check(table&&download&&install);check(!download->isEnabled()&&!install->isVisible());
 QJsonArray installed,available;for(const auto* id:{"ui","core","gpu","cpu"}){installed.append(QJsonObject{{"id",id},{"state","matched_local_manifest"},{"version","1.2"}});available.append(QJsonObject{{"id",id},{"version",QString(id)=="core"?"1.2.0":"1.3"}});}
 dialog.presentLocal({{"manifest_state","valid"},{"components",installed}});dialog.presentPublished({{"schema","PGN_AUTHENTICATED_COMPONENT_VIEW_V1"},{"publisher_authenticated",true},{"components",available}});
 check(table->rowCount()==4&&table->columnCount()==4);check(download->isEnabled());
 check(table->item(0,0)->checkState()==Qt::Checked);check(!(table->item(1,0)->flags()&Qt::ItemIsUserCheckable));check(table->item(1,3)->text()=="Up to date");
 for(int row=0;row<4;++row)if(table->item(row,0)->flags()&Qt::ItemIsUserCheckable)table->item(row,0)->setCheckState(Qt::Unchecked);
 check(!download->isEnabled());dialog.findChild<QPushButton*>("pgn-components-select-all")->click();check(download->isEnabled());
 auto unknown=installed[3].toObject();unknown.insert("state","unidentified");installed[3]=unknown;dialog.presentLocal({{"manifest_state","valid"},{"components",installed}});check(!(table->item(3,0)->flags()&Qt::ItemIsUserCheckable));
 dialog.resize(850,440);app.processEvents();check(table->viewport()->width()>700&&table->viewport()->height()>=136);
 QJsonArray newerLocal,olderPublic;for(const auto* id:{"ui","core","gpu","cpu"}){newerLocal.append(QJsonObject{{"id",id},{"state","matched_local_manifest"},{"version","3.0"}});olderPublic.append(QJsonObject{{"id",id},{"version","2.0"}});}
 dialog.presentLocal({{"manifest_state","valid"},{"components",newerLocal}});dialog.presentPublished({{"schema","PGN_AUTHENTICATED_COMPONENT_VIEW_V1"},{"publisher_authenticated",true},{"components",olderPublic}});
 check(!download->isEnabled()&&table->item(0,3)->text()=="Newer local build");check(dialog.findChild<QLabel*>("pgn-components-status")->text().contains("newer than the published"));
 dialog.presentPublished({{"schema","PGN_AUTHENTICATED_COMPONENT_VIEW_V1"},{"publisher_authenticated",true},{"components",newerLocal}});check(dialog.findChild<QLabel*>("pgn-components-status")->text()=="All components are up to date.");
 dialog.presentUpdateFailure("HTTP error occurred with status 504");
 check(table->item(0,3)->text()=="Last check outdated"&&table->item(0,2)->text()=="3.0"&&!download->isEnabled());
 auto* retryStatus=dialog.findChild<QLabel*>("pgn-components-status");
 check(retryStatus->text().contains("GitHub update service")&&retryStatus->text().contains("Last successful check:"));
 check(retryStatus->toolTip().contains("GitHub update service"));
 dialog.presentPublished({{"schema","PGN_AUTHENTICATED_COMPONENT_VIEW_V1"},{"publisher_authenticated",true},{"components",newerLocal}});
 check(retryStatus->text()=="All components are up to date."&&retryStatus->toolTip().isEmpty());
 dialog.presentLocal({{"manifest_state","valid"},{"components",installed}});dialog.presentPublished({{"schema","PGN_AUTHENTICATED_COMPONENT_VIEW_V1"},{"publisher_authenticated",true},{"components",available}});
 dialog.presentProgress({{"schema","PGN_UPDATE_PROGRESS_V1"},{"stage","verify"},{"component","ui"},{"completed",2048},{"total",4096}});
 auto* progressCell=table->cellWidget(0,4);check(progressCell);
 auto* componentProgress=progressCell->findChild<QProgressBar*>("pgn-component-row-progress");check(componentProgress&&componentProgress->value()==2048&&componentProgress->maximum()==4096);
 dialog.presentProgress({{"schema","PGN_UPDATE_PROGRESS_V1"},{"stage","download"},{"component",""},{"completed",73400320},{"total",204265081}});
 auto* globalProgress=dialog.findChild<QProgressBar*>("pgn-components-progress");check(globalProgress&&globalProgress->value()==73400320&&globalProgress->maximum()==204265081);
 if(argc>1){QDir().mkpath(QString::fromLocal8Bit(argv[1]));check(dialog.grab().save(QString::fromLocal8Bit(argv[1])+"/component-updates.png"));}
 dialog.presentUpdateFailure("Cannot reach the update server. "+QString(500,'A'));
 app.processEvents();
 check(table->columnCount()==4&&!globalProgress->isVisible()&&!dialog.property("pgn_update_progress").isValid());
 auto* status=dialog.findChild<QLabel*>("pgn-components-status");
 check(status&&status->parentWidget()->mapTo(&dialog,QPoint{}).y()<
       download->mapTo(&dialog,QPoint{}).y());
 check(table->mapTo(&dialog,table->rect().bottomLeft()).y()<
       status->parentWidget()->mapTo(&dialog,QPoint{}).y());

 // An authenticated helper can return a valid terminal result and leave
 // Horizon open if the installation stops before committing.
 QTemporaryDir receipts;
 check(receipts.isValid());
 const auto resultPath=receipts.filePath("result.json");
 const auto deliver=[&](pgn::ui::ComponentVersionsDialog& target,const QJsonObject& value){
  target.setProperty("pgn_update_result_file",resultPath);
  QTimer* installPoll=nullptr;
  for(auto* timer:target.findChildren<QTimer*>())if(timer->interval()==500)installPoll=timer;
  check(installPoll);
  QFile file(resultPath);check(file.open(QIODevice::WriteOnly));
  const auto bytes=QJsonDocument(value).toJson(QJsonDocument::Compact);
  check(file.write(bytes)==bytes.size());file.close();
  check(QMetaObject::invokeMethod(installPoll,"timeout"));
 };
 const QJsonObject completeFailure{{"schema","PGN_UPDATE_INSTALL_RESULT_V1"},{"publisher_authenticated",true},
   {"installation_committed",false},{"installation_performed",false},{"process_control_performed",true}};
 dialog.presentProgress({{"schema","PGN_UPDATE_PROGRESS_V1"},{"stage","startup"},{"component",""},{"completed",0},{"total",0}});
 const auto startupStatus=status->text();
 auto malformed=completeFailure;malformed.remove("installation_performed");
 deliver(dialog,malformed);
 check(status->text()==startupStatus&&globalProgress->isVisible());
 malformed=completeFailure;malformed["process_control_performed"]="true";
 deliver(dialog,malformed);
 check(status->text()==startupStatus&&globalProgress->isVisible());
 malformed=completeFailure;malformed["installation_performed"]=true;
 deliver(dialog,malformed);
 check(status->text()==startupStatus&&globalProgress->isVisible());
 deliver(dialog,completeFailure);
 check(status->text().contains("Installation did not complete")&&!globalProgress->isVisible());
 check(!install->isVisible());
 check(dialog.findChild<QPushButton*>("pgn-components-check")->isEnabled());

 // An interrupted installation stays visible even when no update catalogue
 // can be fetched. The fixture uses a unique test-mode application directory.
 context.genesisHash=QString(64,'5');const auto store=pgn::update::updateStateDirectory(context.genesisHash)+"/installation";check(QDir().mkpath(store));
 QFile journal(store+"/transaction.json");check(journal.open(QIODevice::WriteOnly));journal.write(QJsonDocument(QJsonObject{{"schema","PGN_COHORT_ACTIVATION_V1"},{"state","starting_candidate"},{"restoration_completed",false}}).toJson());journal.close();
 pgn::ui::ComponentVersionsDialog recovery(context);recovery.show();recovery.presentLocal({{"manifest_state","valid"},{"components",installed}});recovery.presentPublished({{"schema","PGN_AUTHENTICATED_COMPONENT_VIEW_V1"},{"publisher_authenticated",true},{"components",available}});app.processEvents();
 auto* restore=recovery.findChild<QPushButton*>("pgn-components-recover");auto* blockedDownload=recovery.findChild<QPushButton*>("pgn-components-prepare");check(restore&&restore->isVisible()&&restore->isEnabled());check(!blockedDownload->isEnabled());
 deliver(recovery,completeFailure);
 const auto* recoveryStatus=recovery.findChild<QLabel*>("pgn-components-status");
 check(recoveryStatus&&recoveryStatus->text().contains("Restore the previous version before installing another update."));
 check(restore->isVisible()&&restore->isEnabled()&&!blockedDownload->isEnabled());
 auto* choices=recovery.findChild<QTableWidget*>("pgn-components-table");choices->item(0,0)->setCheckState(Qt::Unchecked);check(!blockedDownload->isEnabled());
 check(journal.remove());recovery.presentLocal({{"manifest_state","valid"},{"components",installed}});check(!restore->isVisible());
 QDir().rmdir(store);QDir().rmdir(QFileInfo(store).absolutePath());
 std::cout<<"PASS "<<checks<<" optional component selection/layout checks; no download or process control\n";
}
