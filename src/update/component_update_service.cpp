#include "update/component_update_service.h"
#include "update/update_paths.h"
#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonArray>
#include <QStandardPaths>
#include <QSysInfo>
#include <QVersionNumber>
#include <QSaveFile>
#include <QUuid>
#include <QProcess>
#include <QRegularExpression>
#include <QTimer>
#include <QDateTime>
#include <QMutex>
#include <QSet>
#include <memory>
#include <stdexcept>
#include <utility>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace pgn::update {
namespace {
constexpr qsizetype MessageLimit=128*1024,ErrorLimit=16*1024;

QString detachedResultOutputPath(const QString& resultPath)
{
    return QFileInfo(resultPath).absolutePath()+"/result-stdout.json";
}

QJsonObject readBoundedResult(const QString& path)
{
    const QFileInfo entry(path);
    if(!entry.isFile()||entry.isSymLink())return {};
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)||file.size()>MessageLimit)return {};
    const auto raw=file.read(MessageLimit+1);
    if(raw.size()>MessageLimit)return {};
    return QJsonDocument::fromJson(raw).object();
}

bool terminalInstallResult(const QJsonObject& value)
{
    const auto schema=value.value("schema").toString();
    const auto committed=value.value("installation_committed");
    const auto performed=value.value("installation_performed");
    if(schema=="PGN_UPDATE_INSTALL_RESULT_V1"){
        return committed.isBool()&&performed==committed
            &&value.value("publisher_authenticated")==QJsonValue(true)
            &&value.value("process_control_performed")==QJsonValue(true);
    }
    if(schema=="PGN_UPDATE_RECOVERY_RESULT_V1"){
        return committed==QJsonValue(false)&&value.value("recovery_performed")==QJsonValue(true)
            &&value.value("previous_inventory_verified")==QJsonValue(true);
    }
    return schema=="PGN_UPDATE_HELPER_ERROR_V1"&&committed==QJsonValue(false)
        &&performed==QJsonValue(false)&&value.value("error").isString()
        &&!value.value("error_type").toString().isEmpty();
}
void require(bool value,const char* text){if(!value)throw std::runtime_error(text);}
QMutex diagnosticMutex;
QJsonArray diagnosticHistory;
// Foreground preparations and idle retirement share one installation writer.
// These services run on the UI thread and are scoped to their update genesis.
QHash<QString,QSet<ComponentUpdateService*>> foregroundPreparations;
QHash<QString,QPointer<ComponentUpdateService>> idleRetirement;

constexpr qsizetype SavedDiagnosticLimit=16*1024;
constexpr qsizetype SavedDiagnosticCount=24;

QString savedDiagnosticPath(const QString& genesis){
    const auto directory=updateStateDirectory(genesis);
    return directory.isEmpty()?QString{}:directory+"/update-diagnostics.json";
}

QJsonArray readSavedDiagnostics(const QString& path,const QString& genesis,bool* foreignScope=nullptr){
    if(path.isEmpty()||QFileInfo(path).isSymLink())return {};
    QFile file(path);
    if(!file.open(QIODevice::ReadOnly)||file.size()>SavedDiagnosticLimit)return {};
    const auto snapshot=QJsonDocument::fromJson(file.read(SavedDiagnosticLimit+1)).object();
    const auto entries=snapshot.value("operations").toArray();
    if(snapshot.value("schema")!="PGN_UPDATE_DIAGNOSTICS_V1"||entries.size()>SavedDiagnosticCount)return {};
    if(snapshot.value("genesis")!=genesis){
        if(foreignScope)*foreignScope=true;
        return {};
    }
    return entries;
}

void saveDiagnosticOperation(const QString& genesis,QJsonObject event){
    const auto path=savedDiagnosticPath(genesis);
    if(path.isEmpty()||!QDir().mkpath(QFileInfo(path).absolutePath())||QFileInfo(path).isSymLink())return;
    bool foreignScope=false;
    auto entries=readSavedDiagnostics(path,genesis,&foreignScope);
    if(foreignScope)return;
    for(qsizetype i=0;i<entries.size();++i){
        if(entries[i].toObject().value("operation")!=event.value("operation"))continue;
        const auto old=entries[i].toObject();
        if(old.value("event")=="started")event.insert("started_at_utc",old.value("observed_utc"));
        entries.removeAt(i);
        break;
    }
    entries.append(event);
    while(entries.size()>SavedDiagnosticCount)entries.removeFirst();
    const auto bytes=QJsonDocument(QJsonObject{{"schema","PGN_UPDATE_DIAGNOSTICS_V1"},{"genesis",genesis},{"operations",entries}}).toJson(QJsonDocument::Compact);
    if(bytes.size()>SavedDiagnosticLimit)return;
    QSaveFile file(path);
    if(file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size())file.commit();
}

void recordDiagnostic(const QString& operation,const QString& action,QJsonObject event,const QString& genesis={}){
    event.insert("operation",operation);
    event.insert("action",action);
    event.insert("observed_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
    const QMutexLocker locker(&diagnosticMutex);
    if(!genesis.isEmpty()&&event.value("event")!="progress")saveDiagnosticOperation(genesis,event);
    event.insert("diagnostic_scope",genesis);
    const auto prior=diagnosticHistory.isEmpty()?QJsonObject{}:diagnosticHistory.last().toObject();
    if(event.value("event")=="progress"&&prior.value("event")=="progress"&&
       prior.value("operation")==operation&&prior.value("stage")==event.value("stage")&&
       prior.value("component")==event.value("component")){
        diagnosticHistory.replace(diagnosticHistory.size()-1,event);
    }else{
        diagnosticHistory.append(event);
        while(diagnosticHistory.size()>48)diagnosticHistory.removeFirst();
    }
}

QString diagnosticFailure(const QString& error){
    if(error.contains("disk space",Qt::CaseInsensitive))return "disk_space";
    if(error.contains("could not be written",Qt::CaseInsensitive))return "file_write";
    if(error.contains("publisher identity",Qt::CaseInsensitive))return "publisher_unverified";
    if(error.contains("interrupted",Qt::CaseInsensitive)||error.contains("update server",Qt::CaseInsensitive))return "transfer";
    if(error.contains("result",Qt::CaseInsensitive)||error.contains("response",Qt::CaseInsensitive))return "helper_response";
    return "helper_failure";
}
}
QJsonArray componentUpdateDiagnosticHistory(const QString& genesis){
    const QMutexLocker locker(&diagnosticMutex);
    const auto saved=readSavedDiagnostics(savedDiagnosticPath(genesis),genesis);
    QSet<QString> active;
    QJsonArray current;
    for(const auto& value:diagnosticHistory){
        auto event=value.toObject();
        if(!genesis.isEmpty()&&event.value("diagnostic_scope")!=genesis)continue;
        event.remove("diagnostic_scope");
        active.insert(event.value("operation").toString());
        current.append(event);
    }
    QJsonArray combined;
    for(const auto& value:saved){
        const auto event=value.toObject();
        const auto operation=event.value("operation").toString();
        const auto action=event.value("action").toString();
        const auto goal=event.value("event").toString();
        const auto at=event.value("observed_utc").toString();
        if(QUuid(operation).isNull()||active.contains(operation)||!QDateTime::fromString(at,Qt::ISODateWithMs).isValid())continue;
        if(action!="check"&&action!="prepare"&&action!="prepare_install"&&action!="inspect_storage"&&action!="clean_storage")continue;
        if(goal!="started"&&goal!="completed"&&goal!="failed")continue;
        QJsonObject entry{{"operation",operation},{"action",action},{"event",goal},{"observed_utc",at}};
        const auto started=event.value("started_at_utc").toString();
        if(QDateTime::fromString(started,Qt::ISODateWithMs).isValid())entry.insert("started_at_utc",started);
        const auto reason=event.value("reason").toString();
        static const QSet<QString> knownReasons{"cancelled","timed_out","disk_space","file_write","publisher_unverified",
            "transfer","helper_response","helper_failure","closed_during_update"};
        if(knownReasons.contains(reason))entry.insert("reason",reason);
        if(event.value("sequence").isDouble())entry.insert("sequence",event.value("sequence").toInteger());
        if(event.value("publisher_verified").isBool())entry.insert("publisher_verified",event.value("publisher_verified"));
        combined.append(entry);
    }
    for(const auto& value:current)combined.append(value);
    while(combined.size()>48)combined.removeFirst();
    return combined;
}
ComponentUpdateDeployment componentUpdateDeployment(const QString& applicationDirectory,bool testnet){
    const auto root=QFileInfo(applicationDirectory).canonicalFilePath();
    require(!root.isEmpty(),"Installed application directory is unavailable");
    QFile descriptor(root+"/UpdateRuntime/deployment.json");
    require(descriptor.open(QIODevice::ReadOnly)&&descriptor.size()<=8192,"Update deployment is unavailable");
    QJsonParseError error;
    const auto doc=QJsonDocument::fromJson(descriptor.readAll(),&error);
    const auto config=doc.object();
    require(error.error==QJsonParseError::NoError&&config.value("schema")=="PGN_UPDATE_DEPLOYMENT_V1","Update deployment is invalid");
    require(config.value("network").toString()==(testnet?"testnet":"main"),"Update deployment belongs to another network");
    require(QRegularExpression("^[0-9a-f]{64}$").match(config.value("genesis_hash").toString()).hasMatch(),"Update genesis is invalid");
    const auto helperName=QSysInfo::kernelType()=="winnt"?"UpdateRuntime/horizon-updater.exe":"UpdateRuntime/horizon-updater";
    require(config.value("helper")==helperName,"Unexpected update helper path");
    const auto helper=QFileInfo(root+"/"+config.value("helper").toString()).canonicalFilePath();
    require(helper.startsWith(root+"/",Qt::CaseInsensitive),"Update helper is outside its deployment");
    QFile file(helper);
    require(file.open(QIODevice::ReadOnly)&&file.size()>0&&file.size()<=64*1024*1024,"Update helper is unavailable");
    QCryptographicHash digest(QCryptographicHash::Sha256);
    require(digest.addData(&file)&&QString::fromLatin1(digest.result().toHex())==config.value("sha256").toString(),"Update helper fingerprint differs");
    return {helper,config};
}

std::optional<int> componentVersionComparison(QString installed,QString available){
    const QRegularExpression numeric("^(?:0|[1-9][0-9]{0,8})(?:[.-](?:0|[1-9][0-9]{0,8})){0,7}$");
    if(!numeric.match(installed).hasMatch()||!numeric.match(available).hasMatch())return {};
    installed.replace('-','.');available.replace('-','.');
    return QVersionNumber::compare(QVersionNumber::fromString(installed).normalized(),QVersionNumber::fromString(available).normalized());
}
QString componentUpdateFailureMessage(const QString& detail){
 const auto value=detail.left(512);
 if(value.startsWith("Cannot reach the update server.")||
    value.startsWith("GitHub update service is temporarily unavailable"))return value;
 const auto http=QRegularExpression("\\b(?:status|HTTP)\\D{0,32}(5[0-9]{2}|429)\\b",QRegularExpression::CaseInsensitiveOption).match(value);
 if(http.hasMatch())return "GitHub update service is temporarily unavailable (HTTP "+http.captured(1)+"). Version status could not be checked. Try again later.";
 if(QRegularExpression("getaddrinfo|name.?resolution|connection.*(?:failed|refused|reset)|timed? ?out|network.*unreachable|WinError (?:10060|10061|11001)",QRegularExpression::CaseInsensitiveOption).match(value).hasMatch())
  return "Cannot reach the update server. Check your Internet connection and try again. Version status is unconfirmed. Details: "+value;
 if(value=="Update deployment is unavailable")return "Update setup is incomplete in this application folder. Download and extract the corrected complete Horizon package.";
 return value;
}
bool validComponentUpdateProgress(const QJsonObject& value){
 const auto done=value.value("completed"),total=value.value("total");
 return value.size()==5&&value.value("schema")=="PGN_UPDATE_PROGRESS_V1"
  &&QStringList{"metadata","download","verify","prepare","drain","startup","committed"}.contains(value.value("stage").toString())
  &&QStringList{"","ui","core","gpu","cpu"}.contains(value.value("component").toString())
  &&done.isDouble()&&total.isDouble()&&done.toDouble()==done.toInteger(-1)&&total.toDouble()==total.toInteger(-1)
  &&done.toInteger(-1)>=0&&total.toInteger(-1)>=done.toInteger()&&total.toDouble()<9007199254740992.;
}
QJsonObject componentUpdateRequest(const ComponentUpdateDeployment& deployment,const QJsonObject& snapshot,
    const QString& applicationDirectory,const QString& mutableDataRoot,const QString& action,const QStringList& selected){
    const auto config=deployment.context;
    QJsonArray installed,selection;
    // Catalogue discovery is read-only and must also describe an older public
    // build. Preparation/installation retain the actual verified version floor.
    for(const auto& value:(action=="check"||action=="inspect_storage"||action=="clean_storage")?QJsonArray{}:snapshot.value("components").toArray()){
        const auto item=value.toObject();
        const auto status=item.value("state");
        if(status!="matched_local_manifest"
#ifdef Q_OS_WIN
            &&status!="reported_local_manifest"
#endif
            )continue;
        installed.append(QJsonObject{{"id",item.value("id")},{"version",item.value("version")},{"abi",config.value("abi")}});
    }
    for(const auto& id:selected)selection.append(id);
    const auto state=updateStateDirectory(config.value("genesis_hash").toString());
    require(!state.isEmpty(),"Update storage is unavailable for this network");
    QJsonObject context{{"schema","PGN_COMPONENT_STAGE_CONTEXT_V1"},{"network",config.value("network")},
        {"genesis_hash",config.value("genesis_hash")},{"platform",QSysInfo::kernelType()=="winnt"?"windows-x86_64":"linux-x86_64"},
        {"compatibility_set",config.value("compatibility_set")},{"minimum_sequence",1},
        {"installed",installed},{"protected_roots",QJsonArray{applicationDirectory,mutableDataRoot}}};
    QJsonObject request{{"schema","PGN_UPDATE_REQUEST_V1"},{"action",action},{"context",context},{"selected",selection},{"cache",state+"/cache"}};
    if(action=="prepare_install"||action=="install"){request.insert("base_cohort",applicationDirectory);request.insert("installation_store",state+"/installation");}
    if(action=="recover"){request.remove("selected");request.insert("installation_store",state+"/installation");}
    if(action=="retire_installations")request.insert("base_cohort",applicationDirectory);
    return request;
}

bool componentUpdateNeedsRecovery(const QString& genesis){
    const auto state=updateStateDirectory(genesis);if(state.isEmpty())return false;
    QFile file(state+"/installation/transaction.json");
    if(!file.exists())return false;
    if(!file.open(QIODevice::ReadOnly)||file.size()>256*1024)return true;
    QJsonParseError error;const auto doc=QJsonDocument::fromJson(file.readAll(),&error);const auto value=doc.object();
    if(error.error!=QJsonParseError::NoError||value.value("schema")!="PGN_COHORT_ACTIVATION_V1")return true;
    const auto status=value.value("state").toString();
    return status!="committed"&&!(value.value("restoration_completed").toBool()&&
        QStringList{"shutdown_failed","shutdown_refused","startup_failed","activation_failed","recovered_previous"}.contains(status));
}

QJsonObject currentUpdateActivation(const QString& applicationRoot,const QString& mutableDataRoot){
#ifdef Q_OS_WIN
    FILETIME created{},exited{},kernel{},user{};
    require(GetProcessTimes(GetCurrentProcess(),&created,&exited,&kernel,&user),"Current process identity is unavailable");
    const quint64 birth=(quint64(created.dwHighDateTime)<<32)|created.dwLowDateTime;
    return {{"schema","PGN_WINDOWS_UPDATE_ACTIVATION_V1"},{"ui_pid",QCoreApplication::applicationPid()},
        {"ui_birth",QString::number(birth)},{"application_root",QDir::cleanPath(applicationRoot)},
        {"data_root",QDir::cleanPath(mutableDataRoot)}};
#elif defined(Q_OS_LINUX)
    QFile stat("/proc/self/stat"),boot("/proc/sys/kernel/random/boot_id");
    require(stat.open(QIODevice::ReadOnly)&&boot.open(QIODevice::ReadOnly),"Current process identity is unavailable");
    const auto raw=stat.readAll();const auto fields=raw.mid(raw.lastIndexOf(") ")+2).simplified().split(' ');
    const auto bootId=QString::fromLatin1(boot.readAll().trimmed());
    require(fields.size()>19&&QRegularExpression("^[0-9]+$").match(QString::fromLatin1(fields.at(19))).hasMatch()
        &&QRegularExpression("^[0-9a-f-]{36}$").match(bootId).hasMatch(),"Current process identity is invalid");
    return {{"schema","PGN_LINUX_UPDATE_ACTIVATION_V1"},{"ui_pid",QCoreApplication::applicationPid()},
        {"ui_birth",bootId+":"+QString::fromLatin1(fields.at(19))},{"application_root",QDir::cleanPath(applicationRoot)},
        {"data_root",QDir::cleanPath(mutableDataRoot)}};
#else
    Q_UNUSED(applicationRoot);Q_UNUSED(mutableDataRoot);
    throw std::runtime_error("Integrated installation is unavailable on this platform.");
#endif
}

#ifdef Q_OS_WIN
static HANDLE startWindowsInstall(const QString& program,const QString& requestPath,const QString& resultPath)
{
    SECURITY_ATTRIBUTES inheritable{sizeof(SECURITY_ATTRIBUTES),nullptr,TRUE};
    auto open=[&](const std::wstring& path,DWORD access,DWORD share,DWORD creation){
        HANDLE handle=CreateFileW(path.c_str(),access,share,&inheritable,creation,FILE_ATTRIBUTE_NORMAL,nullptr);
        return std::unique_ptr<void,decltype(&CloseHandle)>(handle==INVALID_HANDLE_VALUE?nullptr:handle,&CloseHandle);
    };
    const auto null=open(L"NUL",GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,OPEN_EXISTING);
    auto outputPath=QDir::toNativeSeparators(QFileInfo(detachedResultOutputPath(resultPath)).absoluteFilePath());
    if(!outputPath.startsWith("\\\\?\\"))outputPath=outputPath.startsWith("\\\\")
        ?"\\\\?\\UNC\\"+outputPath.mid(2):"\\\\?\\"+outputPath;
    const auto output=open(outputPath.toStdWString(),GENERIC_WRITE,FILE_SHARE_READ,CREATE_NEW);
    require(null&&output,"Cannot open the updater's owned status streams");

    SIZE_T size=0;
    InitializeProcThreadAttributeList(nullptr,1,0,&size);
    require(size>0,"Cannot size the installer startup attributes");
    QByteArray storage(size,0);
    auto* attributes=reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    require(InitializeProcThreadAttributeList(attributes,1,0,&size),"Cannot initialize the installer startup attributes");
    const std::unique_ptr<_PROC_THREAD_ATTRIBUTE_LIST,decltype(&DeleteProcThreadAttributeList)>
        attributesOwner(attributes,&DeleteProcThreadAttributeList);
    HANDLE inherited[]{null.get(),output.get()};
    require(UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherited,sizeof(inherited),nullptr,nullptr),
            "Cannot isolate the installer status streams");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb=sizeof(startup);
    startup.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput=null.get();
    startup.StartupInfo.hStdError=null.get();
    startup.StartupInfo.hStdOutput=output.get();
    startup.lpAttributeList=attributes;
    const auto quote=[](const QString& path){return (QStringLiteral("\"")+QDir::toNativeSeparators(path)+QStringLiteral("\"")).toStdWString();};
    const auto application=QDir::toNativeSeparators(program).toStdWString();
    const auto directory=QDir::toNativeSeparators(QFileInfo(program).absolutePath()).toStdWString();
    auto command=quote(program)+L" --request-file "+quote(requestPath)+L" --result-file "+quote(resultPath);
    PROCESS_INFORMATION process{};
    require(CreateProcessW(application.c_str(),command.data(),nullptr,nullptr,TRUE,
                EXTENDED_STARTUPINFO_PRESENT|CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS,
                nullptr,directory.c_str(),&startup.StartupInfo,&process),
            "The installation helper could not start; installed components are unchanged");
    CloseHandle(process.hThread);
    return process.hProcess;
}
#endif

void DetachedComponentInstall::closeProcess() noexcept
{
#ifdef Q_OS_WIN
    if(process_)CloseHandle(process_);
#endif
    process_=nullptr;
}

DetachedComponentInstall::~DetachedComponentInstall()
{
    closeProcess();
}

DetachedComponentInstall::DetachedComponentInstall(DetachedComponentInstall&& other) noexcept
    :resultPath(std::move(other.resultPath)),process_(std::exchange(other.process_,nullptr))
{
}

DetachedComponentInstall& DetachedComponentInstall::operator=(DetachedComponentInstall&& other) noexcept
{
    if(this!=&other){
        closeProcess();
        resultPath=std::move(other.resultPath);
        process_=std::exchange(other.process_,nullptr);
    }
    return *this;
}

bool DetachedComponentInstall::hasExited() const
{
#ifdef Q_OS_WIN
    return process_&&WaitForSingleObject(process_,0)==WAIT_OBJECT_0;
#else
    return false;
#endif
}

DetachedComponentInstall startDetachedComponentInstall(const ComponentUpdateDeployment& deployment,const QJsonObject& request){
    require(request.value("schema")=="PGN_UPDATE_REQUEST_V1"&&(request.value("action")=="install"||request.value("action")=="recover"),"Explicit installation or recovery request required");
    require(deployment.context.value("owned_update_protocol").toInt()==1,"Installed client has no ordered update protocol");
    const auto encoded=QJsonDocument(request).toJson(QJsonDocument::Compact);
    require(encoded.size()<=MessageLimit,"Installation request exceeds its bound");
    const auto cache=request.value("cache").toString();require(QFileInfo(cache).isAbsolute(),"Absolute update cache required");
    const auto session=cache+"/sessions/"+QUuid::createUuid().toString(QUuid::Id128);
    require(QDir().mkpath(session),"Cannot create the owned update session");
    QSaveFile file(session+"/request.json");require(file.open(QIODevice::WriteOnly)&&file.write(encoded)==encoded.size()&&file.commit(),"Cannot persist the installation request");
    DetachedComponentInstall install;
    install.resultPath=session+"/result.json";
#ifdef Q_OS_WIN
    install.process_=startWindowsInstall(deployment.helper,session+"/request.json",install.resultPath);
#else
    QProcess helper;
    helper.setProgram(deployment.helper);
    helper.setArguments({"--request-file",session+"/request.json","--result-file",install.resultPath});
    helper.setWorkingDirectory(QFileInfo(deployment.helper).absolutePath());
    helper.setStandardInputFile(QProcess::nullDevice());
    helper.setStandardOutputFile(detachedResultOutputPath(install.resultPath));
    helper.setStandardErrorFile(QProcess::nullDevice());
    require(helper.startDetached(),"The installation helper could not start; installed components are unchanged");
#endif
    return install;
}

QJsonObject readDetachedComponentInstallResult(const QString& resultPath)
{
    if(resultPath.isEmpty())return {};
    const auto primary=readBoundedResult(resultPath);
    if(terminalInstallResult(primary))return primary;
    // The helper prints its final result even if replacing result.json failed.
    const auto alternate=readBoundedResult(detachedResultOutputPath(resultPath));
    if(terminalInstallResult(alternate))return alternate;
    return validComponentUpdateProgress(primary)?primary:QJsonObject{};
}

ComponentUpdateService::ComponentUpdateService(ComponentUpdateDeployment deployment,QObject* parent)
    :QObject(parent),deployment_(std::move(deployment)),process_(new QProcess(this)),timer_(new QTimer(this)){
    timer_->setSingleShot(true);
#ifdef Q_OS_WIN
    process_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args){args->flags|=CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS;});
#endif
    connect(process_,&QProcess::readyReadStandardOutput,this,[this]{
        output_+=process_->readAllStandardOutput();
        if(output_.size()>MessageLimit){oversized_=true;process_->kill();}
    });
    connect(process_,&QProcess::readyReadStandardError,this,[this]{
        const auto chunk=process_->readAllStandardError();errors_+=chunk;if(errors_.size()>ErrorLimit)errors_=errors_.right(ErrorLimit);
        progressBuffer_+=chunk;if(progressBuffer_.size()>ErrorLimit){progressBuffer_.clear();return;}
        while(progressBuffer_.contains('\n')){const auto end=progressBuffer_.indexOf('\n');const auto line=progressBuffer_.left(end);progressBuffer_.remove(0,end+1);if(line.size()>4096)continue;
            const auto value=QJsonDocument::fromJson(line).object();
            if(!validComponentUpdateProgress(value))continue;
            recordDiagnostic(diagnosticOperation_,action_,{{"event","progress"},{"stage",value.value("stage")},
                {"component",value.value("component")},{"completed",value.value("completed")},{"total",value.value("total")}},
                deployment_.context.value("genesis_hash").toString());
            if(!settled_&&(action_=="prepare"||action_=="prepare_install")&&value.value("stage")=="download"){
                auto& highWater=downloadHighWater_[value.value("component").toString()];
                const auto total=value.value("total").toInteger();
                const auto completed=value.value("completed").toInteger();
                if((total>highWater.first&&completed>0)||(total==highWater.first&&completed>highWater.second)){
                    highWater={total,completed};
                    timer_->start();
                }
            }
            if(onProgress){const QPointer<ComponentUpdateService> alive(this);onProgress(value);if(!alive)return;}
        }
    });
    connect(process_,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int,QProcess::ExitStatus){complete();});
    connect(process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){if(error==QProcess::FailedToStart)complete();});
    connect(timer_,&QTimer::timeout,this,[this]{timedOut_=true;process_->kill();});
}
void ComponentUpdateService::releaseSchedulingSlot(){
    const auto scope=deployment_.context.value("genesis_hash").toString();
    if(idleRetirement.value(scope)==this)idleRetirement.remove(scope);
    auto foreground=foregroundPreparations.find(scope);
    if(foreground!=foregroundPreparations.end()){
        foreground->remove(this);
        if(foreground->isEmpty())foregroundPreparations.erase(foreground);
    }
}
ComponentUpdateService::~ComponentUpdateService(){
    releaseSchedulingSlot();
    if(!settled_)recordDiagnostic(diagnosticOperation_,action_,{{"event","failed"},{"reason","closed_during_update"}},
                                 deployment_.context.value("genesis_hash").toString());
    settled_=true;timer_->stop();
    if(process_->state()!=QProcess::NotRunning){process_->kill();process_->waitForFinished(1000);}
}
bool ComponentUpdateService::busy()const{return !settled_;}
bool ComponentUpdateService::start(const QJsonObject& request){
    if(busy())return false;
    const auto action=request.value("action").toString();
    if(action!="check"&&action!="prepare"&&action!="prepare_install"&&action!="inspect_storage"&&action!="clean_storage"&&action!="retire_installations")return false;
    const auto data=QJsonDocument(request).toJson(QJsonDocument::Compact);
    if(data.size()>MessageLimit)return false;
    const auto scope=deployment_.context.value("genesis_hash").toString();
    const bool retirement=action=="retire_installations";
    if(retirement&&(foregroundPreparations.contains(scope)||idleRetirement.value(scope)))return false;
    output_.clear();errors_.clear();progressBuffer_.clear();downloadHighWater_.clear();settled_=false;cancelled_=oversized_=timedOut_=false;action_=action;
    diagnosticOperation_=QUuid::createUuid().toString(QUuid::WithoutBraces);
    recordDiagnostic(diagnosticOperation_,action_,{{"event","started"}},deployment_.context.value("genesis_hash").toString());
    process_->setProgram(deployment_.helper);process_->setArguments({});process_->setWorkingDirectory(QFileInfo(deployment_.helper).absolutePath());
    disconnect(startedConnection_);
    startedConnection_=connect(process_,&QProcess::started,this,[this,data]{disconnect(startedConnection_);process_->write(data);process_->closeWriteChannel();});
    const QPointer<ComponentUpdateService> guard(this);
    if(retirement)idleRetirement.insert(scope,this);
    const bool preparation=action=="prepare"||action=="prepare_install";
    if(preparation)foregroundPreparations[scope].insert(this);
    if(onStatus)onStatus(action=="retire_installations"?"Cleaning obsolete installations…":action=="inspect_storage"?"Inspecting unused update files…":action=="clean_storage"?"Cleaning verified unused files…":action=="check"?"Checking signed component versions…":"Downloading and verifying the selected components…");
    if(!guard)return true;
    const auto launch=[guard,operation=diagnosticOperation_,action]{
        if(!guard||guard->settled_||guard->diagnosticOperation_!=operation||
            guard->process_->state()!=QProcess::NotRunning)return;
        if(guard->cancelled_){guard->complete();return;}
        guard->timer_->start(action=="check"||action=="inspect_storage"?60000:600000);
        guard->process_->start();
    };
    const auto cleanup=preparation&&!cancelled_?idleRetirement.value(scope):QPointer<ComponentUpdateService>{};
    if(cleanup&&cleanup->process_->state()!=QProcess::NotRunning){
        // Wait for the OS to drain the interrupted cleanup before enumerating
        // its store. No waiting or disk work is done on the UI thread.
        connect(cleanup->process_,&QProcess::stateChanged,this,[this,launch](QProcess::ProcessState state){
            if(state==QProcess::NotRunning)QTimer::singleShot(0,this,launch);
        });
        cleanup->cancel();
    }else{
        if(cleanup)cleanup->cancel();
        launch();
    }
    return true;
}
void ComponentUpdateService::cancel(){if(busy()){cancelled_=true;process_->kill();}}
void ComponentUpdateService::complete(){
    if(settled_)return;settled_=true;timer_->stop();disconnect(startedConnection_);
    releaseSchedulingSlot();
    output_+=process_->readAllStandardOutput();
    QString error;
    if(cancelled_)error="Update preparation cancelled. Installed components are unchanged.";
    else if(timedOut_)error="The update operation timed out. Installed components are unchanged.";
    else if(oversized_||output_.size()>MessageLimit)error="The update helper response exceeded its bound.";
    QJsonParseError parseError;const auto document=QJsonDocument::fromJson(output_,&parseError);const auto result=document.object();
    if(error.isEmpty()&&(parseError.error!=QJsonParseError::NoError||!document.isObject()))error="The update helper returned no valid result.";
    if(error.isEmpty()&&(process_->exitStatus()!=QProcess::NormalExit||process_->exitCode()!=0))error=result.value("error").toString("The update operation failed.").left(512);
    const bool storage=action_=="inspect_storage"||action_=="clean_storage";
    const bool retirement=action_=="retire_installations";
    if(error.isEmpty()&&!storage&&!retirement&&!result.value("publisher_authenticated").toBool())error="The update result has no verified publisher identity.";
    const auto expected=retirement?QString("PGN_UPDATE_RETIREMENT_RESULT_V1"):storage?QString("PGN_DIAGNOSTIC_STORAGE_V1"):action_=="check"?QString("PGN_AUTHENTICATED_COMPONENT_VIEW_V1"):action_=="prepare"?QString("PGN_AUTHENTICATED_STAGE_RECEIPT_V1"):QString("PGN_UPDATE_INSTALL_PREPARATION_V1");
    if(error.isEmpty()&&result.value("schema").toString()!=expected)error="The update helper returned an unexpected result type.";
    if(error.isEmpty()&&retirement&&result.value("process_control_performed")!=QJsonValue(false))error="The updater returned an invalid retirement result.";
    if(error.isEmpty()&&storage&&(!result.value("process_control_performed").isBool()||result.value("process_control_performed").toBool()
        ||!result.value("cleaned").isBool()||result.value("cleaned").toBool()!=(action_=="clean_storage")||!result.value("cache").isObject()))error="The storage helper returned an invalid maintenance result.";
    if(!error.isEmpty()){
        recordDiagnostic(diagnosticOperation_,action_,{{"event","failed"},
            {"reason",cancelled_?QString("cancelled"):timedOut_?QString("timed_out"):diagnosticFailure(error)}},
            deployment_.context.value("genesis_hash").toString());
        if(onError)onError(componentUpdateFailureMessage(error));return;
    }
    recordDiagnostic(diagnosticOperation_,action_,{{"event","completed"},{"sequence",result.value("sequence")},
        {"publisher_verified",result.value("publisher_authenticated")}},deployment_.context.value("genesis_hash").toString());
    auto observed=result;if(action_=="check")observed.insert("checked_at_utc",QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    if(onResult)onResult(observed);
}
}
