#include "update/component_update_service.h"
#include "update/mining_update_resume.h"
#include "update/update_paths.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <QProcess>
#include <QUuid>
#include <QSysInfo>
#include <iostream>
#include <stdexcept>

int main(int argc,char** argv){
 QCoreApplication app(argc,argv);
 const auto helperName=QSysInfo::kernelType()=="winnt"?"horizon-updater.exe":"horizon-updater";
 if(QFileInfo(app.applicationFilePath()).fileName()==helperName){
  QFile input;input.open(stdin,QIODevice::ReadOnly);const auto request=QJsonDocument::fromJson(input.readAll()).object();const auto fixture=request.value("fixture").toString();
  if(fixture=="delay")QThread::sleep(30);
  if(fixture=="ongoing"||fixture=="repeated"){
   for(int i=1;i<=7;++i){
    const auto bytes=fixture=="repeated"?1:i;
    std::cerr<<"{\"schema\":\"PGN_UPDATE_PROGRESS_V1\",\"stage\":\"download\",\"component\":\"\",\"completed\":"<<bytes<<",\"total\":10}\n"<<std::flush;
    QThread::msleep(85);
   }
  }
  if(fixture=="oversized"){std::cout<<std::string(140000,'x');return 0;}
  if(fixture=="malformed"){std::cout<<"invalid JSON";return 0;}
  if(fixture=="failure"){std::cout<<"{\"error\":\"Fixture refused\"}";return 1;}
  if(fixture=="progress"){std::cerr<<"{\"schema\":\"PGN_UPDATE_PROGRESS_V1\",\"stage\":\"download\",\"component\":\"\",\"completed\":5,\"total\":10}\n"<<std::flush;}
  if(request.value("action")=="inspect_storage"||request.value("action")=="clean_storage"){
   QJsonObject result{{"schema","PGN_DIAGNOSTIC_STORAGE_V1"},{"cleaned",request.value("action")=="clean_storage"},{"process_control_performed",fixture=="storage-process"},{"cache",QJsonObject{{"path","fixture/cache"},{"total_bytes",100},{"reclaimable_bytes",50}}}};
   if(fixture=="storage-wrong-action")result.insert("cleaned",!result.value("cleaned").toBool());
   std::cout<<QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString();return 0;
  }
  const auto schema=fixture=="wrong-type"?"WRONG":request.value("action")=="check"?"PGN_AUTHENTICATED_COMPONENT_VIEW_V1":request.value("action")=="prepare"?"PGN_AUTHENTICATED_STAGE_RECEIPT_V1":"PGN_UPDATE_INSTALL_PREPARATION_V1";
  std::cout<<QJsonDocument(QJsonObject{{"schema",schema},{"publisher_authenticated",true},{"installation_performed",false}}).toJson(QJsonDocument::Compact).toStdString();return 0;
 }
 const auto testGenesis=QString(64,'5');
 const auto foreignGenesis=testGenesis.left(20)+QString(44,'6');
 app.setOrganizationName("PGN-Update-History-QA");
 if(app.arguments().size()==3&&app.arguments().at(1)=="--read-update-history"){
  app.setApplicationName(app.arguments().at(2));
  const auto restored=pgn::update::componentUpdateDiagnosticHistory(testGenesis);
  if(restored.isEmpty()||restored.size()>24||!pgn::update::componentUpdateDiagnosticHistory(foreignGenesis).isEmpty())return 2;
  bool success=false,cancelled=false,closed=false;
  for(const auto& item:restored){
   const auto entry=item.toObject();
   if(entry.value("event")=="progress"||entry.contains("error")||entry.contains("diagnostic_scope")||
      entry.value("started_at_utc").toString().isEmpty())return 3;
   if(entry.value("action")=="check"&&entry.value("event")=="completed"&&entry.value("publisher_verified").toBool())success=true;
   if(entry.value("reason")=="cancelled")cancelled=true;
   if(entry.value("reason")=="closed_during_update")closed=true;
  }
  return success&&cancelled&&closed?0:4;
 }
 const auto appName=QString("fixture-")+QUuid::createUuid().toString(QUuid::Id128);
 app.setApplicationName(appName);
 int checks=0;
 const auto require=[&](bool okay,const char* message){++checks;if(!okay)throw std::runtime_error(message);};
 try{
  require(pgn::update::componentUpdateFailureMessage("HTTP error occurred with status 504").contains("GitHub update service"),"GitHub outage was not identified");
  require(pgn::update::componentUpdateFailureMessage("HTTP 429").contains("429"),"Rate-limit detail lost");
  require(pgn::update::componentUpdateFailureMessage("getaddrinfo failed").contains("Internet connection"),"Connection failure has no actionable message");
  require(pgn::update::componentUpdateFailureMessage("Signature mismatch")=="Signature mismatch","Integrity error was relabelled as a network outage");
  QTemporaryDir temp;require(temp.isValid(),"Temporary directory unavailable");QDir().mkpath(temp.path()+"/UpdateRuntime");const auto helper=temp.path()+"/UpdateRuntime/"+helperName;require(QFile::copy(app.applicationFilePath(),helper),"Inert helper copy failed");QFile binary(helper);require(binary.open(QIODevice::ReadOnly),"Helper fixture unreadable");QCryptographicHash digest(QCryptographicHash::Sha256);require(digest.addData(&binary),"Fixture hash failed");binary.close();
  QJsonObject deployment{{"schema","PGN_UPDATE_DEPLOYMENT_V1"},{"network","testnet"},{"genesis_hash",QString(64,'5')},{"helper","UpdateRuntime/"+QString(helperName)},{"sha256",QString::fromLatin1(digest.result().toHex())}};
  QFile descriptor(temp.path()+"/UpdateRuntime/deployment.json");require(descriptor.open(QIODevice::WriteOnly),"Descriptor fixture unavailable");descriptor.write(QJsonDocument(deployment).toJson());descriptor.close();
  auto resolved=pgn::update::componentUpdateDeployment(temp.path(),true);require(resolved.helper==helper,"Wrong helper selected");bool rejected=false;try{pgn::update::componentUpdateDeployment(temp.path(),false);}catch(const std::exception&){rejected=true;}require(rejected,"Wrong network was admitted");
  const auto activation=pgn::update::currentUpdateActivation(temp.path(),temp.path()+"/user-data");
  require(activation.value("ui_pid").toInteger()==app.applicationPid(),"Activation process identity differs");
  require(activation.value("application_root")==temp.path()&&activation.value("data_root")==temp.path()+"/user-data","Activation changed the application/data scope");
#ifdef Q_OS_LINUX
  require(activation.value("schema")=="PGN_LINUX_UPDATE_ACTIVATION_V1","Wrong Linux activation schema");
  QFile boot("/proc/sys/kernel/random/boot_id");require(boot.open(QIODevice::ReadOnly),"Boot identity unavailable");
  require(activation.value("ui_birth").toString().startsWith(QString::fromLatin1(boot.readAll().trimmed())+":"),"Linux activation lost boot identity");
#endif
  require(pgn::update::componentVersionComparison("1.2","1.2.0").value_or(1)==0,"Padded version comparison differs");
  require(pgn::update::componentVersionComparison("1.9","1.10").value_or(0)<0,"Numeric versions compared as text");
  require(!pgn::update::componentVersionComparison("custom","1.2"),"Unknown version became current");
  const auto contextRequest=pgn::update::componentUpdateRequest(resolved,QJsonObject{{"components",QJsonArray{QJsonObject{{"id","ui"},{"state","matched_local_manifest"},{"version","1.0"}},QJsonObject{{"id","cpu"},{"state","unidentified"},{"version","custom"}}}}},temp.path(),temp.path()+"/user-data","check",{"ui"});
  require(contextRequest.value("context").toObject().value("installed").toArray().isEmpty(),"Read-only catalogue comparison was blocked by a newer local build");
  const auto installContext=pgn::update::componentUpdateRequest(resolved,QJsonObject{{"components",QJsonArray{QJsonObject{{"id","ui"},{"state","matched_local_manifest"},{"version","1.0"}},QJsonObject{{"id","cpu"},{"state","unidentified"},{"version","custom"}}}}},temp.path(),temp.path()+"/user-data","prepare_install",{"ui"});
  require(installContext.value("context").toObject().value("installed").toArray().size()==1&&installContext.value("context").toObject().value("installed").toArray()[0].toObject().value("version")=="1.0","Installation lost its real verified anti-downgrade floor");
  require(contextRequest.value("context").toObject().value("protected_roots").toArray().contains(temp.path()+"/user-data"),"User data was not protected from staging");
  const auto recovery=pgn::update::componentUpdateRequest(resolved,{},temp.path(),temp.path()+"/user-data","recover",{});
  require(!recovery.contains("selected")&&!recovery.contains("activation")&&!recovery.contains("base_cohort"),"Recovery invented a new update selection/activation");
  require(recovery.value("installation_store").toString().endsWith("/installation"),"Recovery store is absent");
  pgn::update::ComponentUpdateService service(resolved);int results=0,errors=0;QString lastError;service.onResult=[&](const QJsonObject&){++results;};service.onError=[&](const QString& error){++errors;lastError=error;};
  const auto wait=[&]{QElapsedTimer clock;clock.start();while(service.busy()&&clock.elapsed()<10000){app.processEvents();QThread::msleep(5);}require(!service.busy(),"Helper did not settle");};
  const auto request=[](const QString& fixture=QString{}){return QJsonObject{{"action","check"},{"fixture",fixture}};};
  require(service.start(request()),"Valid helper request refused");require(!service.start(request()),"Concurrent operation was admitted");wait();require(results==1&&errors==0,"Successful helper response lost");
  {int observed=0;service.onProgress=[&](const QJsonObject& value){if(value.value("completed").toInt()==5&&value.value("total").toInt()==10)++observed;};require(service.start(request("progress")),"Progress fixture refused");wait();require(observed==1,"Measured progress did not cross the actual child-process transport");service.onProgress={};--results;}
  {QJsonObject value{{"schema","PGN_UPDATE_PROGRESS_V1"},{"stage","download"},{"component","gpu"},{"completed",5},{"total",10}};require(pgn::update::validComponentUpdateProgress(value),"Valid progress rejected");value.insert("completed",11);require(!pgn::update::validComponentUpdateProgress(value),"Progress beyond total admitted");value.insert("completed",1.5);require(!pgn::update::validComponentUpdateProgress(value),"Fractional byte count admitted");value.insert("completed",1);value.insert("path","private");require(!pgn::update::validComponentUpdateProgress(value),"Unbounded private progress field admitted");}
  {
   auto* watchdog=service.findChild<QTimer*>();
   require(watchdog,"Missing helper watchdog");
   const auto exercise=[&](const QString& fixture){
    bool intervalSet=false;
    service.onProgress=[&](const QJsonObject&){
     if(!intervalSet){
      intervalSet=true;watchdog->start(350);
     }
    };
    require(service.start(QJsonObject{{"action","prepare"},{"fixture",fixture}}),"Slow download fixture refused");
    wait();service.onProgress={};
    require(intervalSet,"Slow download did not produce valid progress");
   };
   const auto beforeGood=results;
   exercise("ongoing");require(results==beforeGood+1,"An advancing download expired despite confirmed byte progress");--results;
   const auto beforeBad=errors;
   exercise("repeated");require(errors==beforeBad+1&&lastError.contains("timed out",Qt::CaseInsensitive),"Repeated counts renewed the idle watchdog");
  }
  for(const auto& fixture:{QString("failure"),QString("malformed"),QString("wrong-type"),QString("oversized")}){const auto before=errors;require(service.start(request(fixture)),"Fixture request refused");wait();require(errors==before+1&&!lastError.isEmpty(),"Bad helper response was accepted or completed twice");}
  require(service.start(request("delay")),"Cancellation fixture refused");app.processEvents();service.cancel();wait();require(lastError.contains("cancelled"),"Cancellation was not distinct");
  require(service.start(request()),"Request after cancellation refused");wait();require(results==2,"Cancellation poisoned the next request");
  service.onStatus=[&](const QString&){service.cancel();};require(service.start(request()),"Reentrant cancellation refused");wait();service.onStatus={};require(lastError.contains("cancelled"),"Status callback cancellation launched an unintended operation");
  require(!service.start(QJsonObject{{"action","execute"}}),"Arbitrary execution operation admitted");
  {pgn::update::ComponentUpdateService missing({temp.path()+"/missing.exe",{}});int failed=0;missing.onError=[&](const QString&){++failed;};require(missing.start(request()),"Failed-start fixture refused");QElapsedTimer clock;clock.start();while(missing.busy()&&clock.elapsed()<5000){app.processEvents();QThread::msleep(5);}require(!missing.busy()&&failed==1,"Failed start did not settle exactly once");}
  {
   const QString data=temp.path()+"/resume",genesis(64,'a');require(QDir().mkpath(data),"Resume fixture unavailable");
   const QJsonArray devices{QJsonObject{{"kind","gpu"},{"device","GPU/first"}},QJsonObject{{"kind","gpu"},{"device","GPU/second"}},QJsonObject{{"kind","cpu"},{"device","CPU/local/0"},{"threads",137}}};
   pgn::update::saveMiningUpdateResume(data,genesis,devices,1000);
   require(pgn::update::takeMiningUpdateResume(data,genesis,true,1001)==devices,"Update lost devices or exact CPU thread count");
   require(pgn::update::takeMiningUpdateResume(data,genesis,true,1002).isEmpty(),"Update state replayed a second time");
   pgn::update::saveMiningUpdateResume(data,genesis,devices,1000);
   require(pgn::update::takeMiningUpdateResume(data,genesis,false,1001).isEmpty(),"Normal launch started computation");
   require(!QFile::exists(pgn::update::miningResumePath(data)),"Normal launch left a latent update restart");
   const auto refused=[&](const auto& operation){try{operation();return false;}catch(const std::exception&){return true;}};
   for(const auto now:{999LL,1901LL}){pgn::update::saveMiningUpdateResume(data,genesis,devices,1000);require(refused([&]{pgn::update::takeMiningUpdateResume(data,genesis,true,now);}),"Future or expired resume admitted");require(!QFile::exists(pgn::update::miningResumePath(data)),"Expired state remained replayable");}
   pgn::update::saveMiningUpdateResume(data,genesis,devices,1000);
   require(refused([&]{pgn::update::takeMiningUpdateResume(data,QString(64,'b'),true,1001);}),"Cross-network resume admitted");
   auto duplicate=devices;duplicate.append(devices.first());require(!pgn::update::validMiningResumeDevices(duplicate),"Duplicate device resume admitted");
   for(const auto threads:{0,257})require(!pgn::update::validMiningResumeDevices({QJsonObject{{"kind","cpu"},{"device","CPU/local/0"},{"threads",threads}}}),"Unsafe thread count admitted");
   pgn::update::saveMiningUpdateResume(data,genesis,{},1000);require(pgn::update::takeMiningUpdateResume(data,genesis,true,1001).isEmpty(),"All-OFF update enabled resources");
   require(refused([&]{pgn::update::saveMiningUpdateResume(data+"/missing-parent",genesis,devices,1000);}),"Failed state write was treated as successful");
  }
  for(const auto& action:{QString("inspect_storage"),QString("clean_storage")}){
   const auto before=results;require(service.start(QJsonObject{{"action",action}}),"Offline maintenance refused");wait();require(results==before+1,"Local storage result incorrectly required online publisher authentication");
  }
  for(const auto& fixture:{QString("storage-process"),QString("storage-wrong-action")}){
   const auto before=errors;require(service.start(QJsonObject{{"action","inspect_storage"},{"fixture",fixture}}),"Invalid-storage fixture refused before validation");wait();require(errors==before+1,"Unexpected storage process control or operation accepted");
  }
  {
   pgn::update::ComponentUpdateService pending(resolved);
   require(pending.start(request("delay")),"Interrupted-check fixture refused");
  }
  QProcess reader;
  reader.start(app.applicationFilePath(),{"--read-update-history",appName});
  require(reader.waitForFinished(10000)&&reader.exitStatus()==QProcess::NormalExit&&reader.exitCode()==0,
          "A fresh process failed to recover the bounded own-chain update history");
  require(QFile::remove(pgn::update::updateStateDirectory(testGenesis)+"/update-diagnostics.json"),
          "Could not remove private updater diagnostic fixture");
  std::cout<<"PASS "<<checks<<" updater transport/lifecycle checks; inert helper only, no network/GPU/wallet/installed code touched\n";return 0;
 }catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
}
