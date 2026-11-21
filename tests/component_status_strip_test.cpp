#include "ui/component_status_strip.h"
#include <QApplication>
#include <QEventLoop>
#include <QTimer>
#include <QDir>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QSaveFile>
#include <QElapsedTimer>
#include <QProgressBar>
#include <QTableWidget>
#include <QThread>
#include <iostream>
int main(int argc,char** argv){
 QApplication app(argc,argv);pgn::ui::ComponentVersionsContext context;context.fetchOnOpen=false;
 if(argc>=3&&QString::fromLocal8Bit(argv[1])=="--profile-rebind"){
  const QString root=QString::fromLocal8Bit(argv[2]);QTemporaryDir runtime;if(!runtime.isValid())return 30;
  context.manifestPath=root+"/components.json";context.coreProfilePath=runtime.filePath("wallet-core.json");
  const QString core=root+"/Core/pgn-core"
#ifdef Q_OS_WIN
  ".exe"
#endif
  ;
  const auto bind=[&](const QString& image){QSaveFile file(context.coreProfilePath);if(!file.open(QIODevice::WriteOnly))return false;file.write(QJsonDocument(QJsonObject{{"executable",image},{"genesis_hash",QString(64,'a')}}).toJson());return file.commit();};
  pgn::ui::ComponentStatusStrip strip(context);
  const auto row=[&]{for(const auto& x:strip.property("pgn_component_local_snapshot").toJsonObject()["components"].toArray())if(x.toObject()["id"]=="core")return x.toObject();return QJsonObject{};};
  const auto wait=[&](const QString& state){QElapsedTimer clock;clock.start();while(row()["state"]!=state&&clock.elapsed()<3500){app.processEvents();QThread::msleep(5);}return row()["state"]==state;};
  if(!wait("unidentified")||!bind(core)||!wait("matched_local_manifest")||!row()["primary_bound"].toBool())return 32;
  if(pgn::ui::inspectLocalComponentVersions(context)["components"].toArray()[1].toObject()["state"]!="matched_local_manifest")return 33;
  if(!bind(root+"/missing-core")||!wait("unidentified"))return 34;
  if(!QFile::remove(context.coreProfilePath)||!bind(core)||!wait("matched_local_manifest"))return 35;
  std::cout<<"PASS atomic profile rebind: absent/valid/invalid/recreated; fresh primary hash binding without restart\n";return 0;
 }
 if(argc>=3&&QString::fromLocal8Bit(argv[1])=="--installed-gpu-updates"){
  const auto expected=QString::fromLocal8Bit(argv[2]);
  const auto root=QCoreApplication::applicationDirPath();
  auto installed=pgn::ui::walletComponentVersionsContext(root,root+"/test-runtime",true);
  installed.fetchOnOpen=false;
  const auto manifest=pgn::ui::inspectLocalComponentVersions(installed,pgn::ui::ComponentInventory::Manifest);
  const auto files=pgn::ui::inspectLocalComponentVersions(installed);
  const auto localGpu=[](const QJsonObject& snapshot){
   for(const auto& entry:snapshot.value("components").toArray())if(entry.toObject().value("id")=="gpu")return entry.toObject();
   return QJsonObject{};
  };
  if(localGpu(manifest).value("state")!="reported_local_manifest"||localGpu(manifest).value("version").toString()!=expected)return 40;
  if(localGpu(files).value("state")!="matched_local_manifest"||!localGpu(files).value("primary_bound").toBool())return 41;
  pgn::ui::ComponentVersionsDialog dialog(installed);
  dialog.presentLocal(manifest);
  dialog.presentProgress({{"schema","PGN_UPDATE_PROGRESS_V1"},{"stage","metadata"},{"component",""},{"completed",0},{"total",0}});
  dialog.presentPublished({{"schema","PGN_AUTHENTICATED_COMPONENT_VIEW_V1"},{"publisher_authenticated",true},
     {"components",QJsonArray{QJsonObject{{"id","gpu"},{"version",expected}}}}});
  auto* table=dialog.findChild<QTableWidget*>("pgn-components-table");
  auto* progress=dialog.findChild<QProgressBar*>();
  if(!table||table->item(2,1)->text()!=expected||table->item(2,3)->text()!="Up to date"||!progress||!progress->isHidden())return 42;
  std::cout<<"PASS installed GPU manifest, executable binding and Updates row\n";return 0;
 }
 if(argc>=3&&QString::fromLocal8Bit(argv[1])=="--inspect-installed"){
  const auto product=QString::fromLocal8Bit(argv[2]);
  const auto resolved=pgn::ui::walletComponentVersionsContext(product,product+"/data/testnet/ui-runtime",true);
  const auto value=pgn::ui::inspectLocalComponentVersions(resolved);
  for(const auto& entry:value.value("components").toArray())if(entry.toObject().value("id")=="cpu"){
   std::cout<<QJsonDocument(entry.toObject()).toJson(QJsonDocument::Compact).constData()<<"\n";return 0;
  }
  return 12;
 }
 if(argc>=3&&QString::fromLocal8Bit(argv[1])=="--inspect"){
  const auto root=QString::fromLocal8Bit(argv[2]);context.manifestPath=root+"/components.json";
  context.codeRoots={{"ui",root},{"core",argc>3?QString::fromLocal8Bit(argv[3]):root+"/Core"},{"gpu",root+"/EngineRuntime"}};
  context.primaryPrograms={{"ui",root+"/PGN-Horizon.exe"},{"core",context.codeRoots.value("core")+"/pgn-core.exe"}};
  const auto value=pgn::ui::inspectLocalComponentVersions(context);std::cout<<QJsonDocument(value).toJson(QJsonDocument::Compact).constData()<<"\n";
  for(const auto& entry:value.value("components").toArray()){const auto row=entry.toObject();if(row.value("id")!="cpu"&&row.value("state")!="matched_local_manifest")return 6;}
  return 0;
 }
 QWidget window;auto* box=new QHBoxLayout(&window);auto* network=new QLabel("TESTNET · Gold blocks 0 · 1 peer");network->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);box->addWidget(network,1);
 auto* status=new pgn::ui::ComponentStatusStrip(context,&window);box->addWidget(status);
 QEventLoop loop;QTimer::singleShot(80,&loop,&QEventLoop::quit);loop.exec();
 status->presentLocal({{"components",QJsonArray{QJsonObject{{"id","ui"},{"state","matched_local_manifest"},{"version","1.2.3"},{"code_root","verified-code"}},QJsonObject{{"id","core"},{"state","unidentified"},{"code_root","existing-code"}},QJsonObject{{"id","cpu"},{"state","unidentified"},{"code_root",""}}}}});
 const auto updates=status->findChild<QToolButton*>("pgn-component-update-status");
 if(!updates||status->findChildren<QToolButton*>().size()!=1||
    !updates->toolTip().contains("UI · 1.2.3")||!updates->toolTip().contains("CORE · unverified")||
    !updates->toolTip().contains("CPU · not configured"))return 1;
 status->presentPublished({{"component_versions_available",false},{"packages",QJsonArray{QJsonObject{{"version","99.0"}}}}});
 if(updates->text()!="Updates · versions unavailable")return 2;
 QJsonObject signedCatalog{{"schema","PGN_AUTHENTICATED_COMPONENT_VIEW_V1"},{"publisher_authenticated",true},
    {"components",QJsonArray{QJsonObject{{"id","ui"},{"version","1.2.4"}}}}};
 status->presentPublished(signedCatalog);
 if(updates->text()!="Updates · 1 available"||updates->property("state")!="update")return 7;
 signedCatalog.insert("components",QJsonArray{QJsonObject{{"id","ui"},{"version","1.2.3"}}});status->presentPublished(signedCatalog);
 if(updates->text()!="Updates · partially verified")return 8;
 status->presentPublished({{"component_versions_available",false}});
 if(updates->text()!="Updates · partially verified")return 9;
 QJsonArray installed,available;
 for(const auto& pair:QList<QPair<QString,QString>>{{"ui","2026.11.6.25"},{"core","2026.11.6.2"},{"gpu","2026.11.6.6"},{"cpu","2026.11.6.389"}}){
  installed.append(QJsonObject{{"id",pair.first},{"state","matched_local_manifest"},{"version",pair.second},{"code_root","verified-code"}});
  available.append(QJsonObject{{"id",pair.first},{"version",pair.second}});
 }
 status->presentLocal({{"components",installed}});signedCatalog.insert("components",available);status->presentPublished(signedCatalog);
 if(updates->text()!="Updates · up to date"||updates->property("state")!="current")return 10;
 for(const auto& item:installed){
  const auto row=item.toObject();
  if(!updates->toolTip().contains(row.value("id").toString().toUpper()+" · "+row.value("version").toString()))return 11;
 }
 status->presentUpdateChecking();
 if(updates->text()!="Updates · checking"||updates->property("state")!="checking")return 12;
 status->presentUpdateFailure("Controlled offline check");
 if(updates->text()!="Updates · check failed"||updates->property("state")!="stale"||!updates->toolTip().contains("Controlled offline check"))return 13;
 status->presentUpdateFailure("GitHub update service is temporarily unavailable (HTTP 504)");
 if(updates->text()!="Updates · GitHub unavailable"||updates->property("state")!="stale")return 17;
 status->presentPublished(signedCatalog);
 if(updates->property("state")!="current")return 14;
 status->presentLocal({{"components",installed}});
 if(updates->property("state")!="current")return 15;
 auto newer=installed;auto newerUI=newer[0].toObject();newerUI.insert("version","2026.11.6.26");newer[0]=newerUI;
 status->presentLocal({{"components",newer}});
 if(updates->text()!="Updates · partially verified"||!updates->toolTip().contains("UI · 2026.11.6.26"))return 16;
 status->presentLocal({{"components",installed}});status->presentPublished(signedCatalog);
 for(int width:{800,1280,1920}){
  window.resize(width,60);window.show();app.processEvents();
  if(!status->rect().contains(updates->geometry()))return 3;
  if(status->geometry().right()>=window.width()||network->width()<180)return 4;
 }
 updates->click();app.processEvents();
 if(!status->findChild<QDialog*>("pgn-component-versions"))return 18;
 if(argc>1){QDir().mkpath(QString::fromLocal8Bit(argv[1]));window.resize(1280,60);app.processEvents();if(!window.grab().save(QString::fromLocal8Bit(argv[1])+"/component-footer.png"))return 5;}
 std::cout<<"PASS: single update button, four installed versions, same component dialog, checked/outdated/error states and 800/1280/1920 layout.\n";
}
