#include "ui/social_access_dialog.h"
#include "social/social_access_wire.h"
#include "ui/social_remote_access_signer.h"
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QTabWidget>
#include <QUuid>
#include <QTemporaryDir>
#include <iostream>
using namespace pgn;using namespace pgn::social;
static int checks=0;
static void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);++checks;}
static QStringList strings(QJsonValue v){QStringList a;for(auto x:v.toArray())a<<x.toString();return a;}
static QJsonArray array(QStringList a){return QJsonArray::fromStringList(a);}
static const QStringList actions={"create_role","edit_role","delete_role","move_role","assign_role","remove_role","kick_member","ban_member","set_overwrite","remove_content","pin_content","react_content"};
struct Bridge{
 QString python,driver,config,actor="wizz";Identity identity;AccessNativeContext native;int reads{},writes{};
 QJsonObject call(QJsonObject request){
  request["actor"]=actor;QProcess process;process.start(python,{driver,config});require(process.waitForStarted(5000),"QA bridge start");process.write(QJsonDocument(request).toJson(QJsonDocument::Compact));process.closeWriteChannel();require(process.waitForFinished(25000),"QA bridge timeout");
  const auto raw=process.readAllStandardOutput();if(process.exitCode()!=0)std::cerr<<process.readAllStandardError().toStdString();require(process.exitCode()==0,"QA bridge exit");QJsonParseError error;const auto doc=QJsonDocument::fromJson(raw,&error);require(error.error==QJsonParseError::NoError&&doc.isObject(),"QA bridge JSON");return doc.object();
 }
 AccessSnapshot parse(QJsonObject v){
  AccessSnapshot s;s.identity=identity;s.authenticated=v["authenticated"].toBool();s.community=v["community"].toString();s.revision=v["revision"].toString();s.source=v["source"].toString();s.observedUtc=QDateTime::fromMSecsSinceEpoch(v["observed_ms"].toInteger(),Qt::UTC);s.moderatorRoleId=v["moderator_role_id"].toString();s.permissionCatalog=strings(v["permissions"]);s.grantablePermissions=strings(v["grantable"]);
  require(v["identity"].toString()==identity.subject&&v["genesis"].toString()==identity.genesis,"Native snapshot binding");
  for(auto x:v["roles"].toArray()){auto o=x.toObject();s.roles.append({o["id"].toString(),o["name"].toString(),o["position"].toInt(),strings(o["permissions"]),o["managed"].toBool()});}
  for(auto x:v["members"].toArray()){auto o=x.toObject();s.members.append({o["id"].toString(),o["name"].toString(),strings(o["roles"])});}
  for(auto x:v["overwrites"].toArray()){auto o=x.toObject();s.overwrites.append({o["channel"].toString(),o["target"].toString(),o["member"].toBool(),strings(o["allow"]),strings(o["deny"])});}
  for(auto x:v["availability"].toArray()){auto o=x.toObject();int i=actions.indexOf(o["action"].toString());require(i>=0,"Known availability action");Availability a;a.action=static_cast<AccessAction>(i);a.target=o["target"].toString();a.role=o["role"].toString();a.channel=o["channel"].toString();a.allowed=o["allowed"].toBool();a.reason=o["reason"].toString();s.availability<<a;}
  auto a=v["session"].toObject();s.session.identity=identity;s.session.state=AuthState::Verified;s.session.reference=a["reference"].toString();s.session.verificationRef=a["verification_ref"].toString();s.session.revocationRevision=a["revocation_revision"].toString();s.session.device=a["device"].toString();s.session.expiresUtc=QDateTime::fromMSecsSinceEpoch(a["expires_ms"].toInteger(),Qt::UTC);for(auto x:a["devices"].toArray()){auto o=x.toObject();s.session.devices<<AuthDevice{o["id"].toString(),o["label"].toString(),o["revoked"].toBool()};}
  require(validateAccess(s,identity).isEmpty(),"Valid authenticated access snapshot");return s;
 }
 AccessSnapshot read(AccessReadRequest r){++reads;const auto wire=accessReadWire(r,native);require(!wire.isEmpty(),"Bounded Native read wire");auto v=call({{"action","read_access"},{"wire",QString::fromUtf8(wire)}});require(v["ok"].toBool(),"Native read accepted");return parse(v["value"].toObject());}
 Services services(){Services s;
  s.readAccess=[this](AccessReadRequest r,auto done){try{done(read(r),{});}catch(const std::exception& e){done({},QString::fromUtf8(e.what()));}};
  s.manage=[this](AccessRequest r,auto done){++writes;const auto wire=accessCommandWire(r,native);require(!wire.isEmpty(),"Bounded Native action wire");auto v=call({{"action",actions[static_cast<int>(r.action)]},{"wire",QString::fromUtf8(wire)}});AccessResult result;result.identity=r.identity;result.community=r.community;result.channel=r.channel;result.requestId=r.requestId;result.outcome=v["ok"].toBool()?AccessOutcome::Accepted:v["error"].toString()=="Conflict"?AccessOutcome::Conflict:AccessOutcome::Denied;result.reason=v["reason"].toString();result.revision=QString::number(v["value"].toObject()["revision"].toInt());done(result);
  };return s;
 }
};
static QPushButton* button(QWidget& w,const char* id){auto* b=w.findChild<QPushButton*>(id);require(b!=nullptr,"Find button");return b;}
static void click(QWidget& w,const char* id){auto* b=button(w,id);if(!b->isEnabled())std::cerr<<id<<": "<<b->toolTip().toStdString()<<"\n";require(b->isEnabled(),"Action enabled by service");b->click();QApplication::processEvents();}
static void select(QListWidget* list,QString name){int found=-1;for(int i=0;i<list->count();++i)if(list->item(i)->text()==name)found=i;require(found>=0,"Named item visible");list->setCurrentRow(found);QApplication::processEvents();}
static void signerScope(const Bridge& bridge){
 const auto wire=accessReadWire({bridge.identity,"global",{},{}},bridge.native);require(accessSigningError(wire,bridge.identity,bridge.native.script,bridge.native.community).isEmpty(),"Exact Native access signing context");
 require(!accessSigningError("PGN/SOCIAL/IDENTITY-LINK/OPERATOR-CONSENT/2\n{}",bridge.identity,bridge.native.script,bridge.native.community).isEmpty(),"Name consent cannot use access signer");
 auto other=bridge.identity;other.subject="0014"+QString(40,'0');require(!accessSigningError(wire,other,bridge.native.script,bridge.native.community).isEmpty(),"Another Social context cannot use access signer");
 require(!accessSigningError(wire,bridge.identity,bridge.native.script,QString(64,'0')).isEmpty(),"Another community cannot use access signer");
 auto body=QJsonDocument::fromJson(wire.mid(wire.indexOf('\n')+1)).object();body["created_ms"]=QDateTime::currentMSecsSinceEpoch()-180000;require(!accessSigningError(QByteArray("PGN/SOCIAL/ACCESS/1\n")+QJsonDocument(body).toJson(QJsonDocument::Compact),bridge.identity,bridge.native.script,bridge.native.community).isEmpty(),"Expired command is not re-signed");
 QTemporaryDir temp;require(temp.isValid(),"Private signer-plan fixtures");auto path=[&](QString n){return temp.path()+"/"+n;};for(const auto& n:{"ssh.exe","key"}){QFile f(path(n));require(f.open(QIODevice::WriteOnly),"Create inert profile fixture");f.write("fixture");}
 QJsonArray args{"C:/QA/host.exe","--network","--genesis",bridge.identity.genesis,"--wallet","qa-wallet","--cookie","C:/QA/node/testnet/.cookie","--rpc-port","58639","--miner",bridge.native.address};
 const QJsonObject profile{{"program",path("ssh.exe")},{"hardware_endpoint",QJsonObject{{"host","127.0.0.1"},{"user","qa"},{"key_file",path("key")},{"host_key_alias","pgn-qa"},{"port",22}}},{"network_arguments",args},{"miner_address",bridge.native.address}};
 const auto plan=ui::remoteAccessConsentPlan(QJsonDocument(profile).toJson(QJsonDocument::Compact),bridge.identity,bridge.native.community,bridge.native.script,wire);if(!plan.error.isEmpty())std::cerr<<plan.error.toStdString()<<std::endl;require(plan.error.isEmpty(),"Scoped access SSH plan");require(plan.arguments.contains("StrictHostKeyChecking=yes")&&plan.arguments.contains("HostKeyAlias=pgn-qa"),"Pinned SSH host verification");require(plan.input.contains("PGN/SOCIAL/ACCESS/1"),"Only access purpose in signer input");
 require(!ui::remoteAccessConsentPlan(QJsonDocument(profile).toJson(QJsonDocument::Compact),bridge.identity,QString(64,'0'),bridge.native.script,wire).error.isEmpty(),"Wrong community refused before process start");
}
int main(int argc,char** argv){QApplication app(argc,argv);try{
 if(argc==3&&QString::fromLocal8Bit(argv[1])=="--signer-powershell"){QFile out(QString::fromLocal8Bit(argv[2]));require(out.open(QIODevice::WriteOnly),"Signer source output");out.write(ui::nativeAccessSignerPowerShell());return 0;} require(argc==6,"QA args python driver config captures actor");Bridge bridge{QString::fromLocal8Bit(argv[1]),QString::fromLocal8Bit(argv[2]),QString::fromLocal8Bit(argv[3])};bridge.actor=argv[5];QFile config(bridge.config);require(config.open(QIODevice::ReadOnly),"Read QA config");auto cfg=QJsonDocument::fromJson(config.readAll()).object();bridge.identity={cfg["genesis"].toString(),cfg["actors"].toObject()[bridge.actor].toObject()["client"].toString()};auto native=cfg["actors"].toObject()[bridge.actor].toObject();bridge.native={native["script"].toString(),native["address"].toString(),cfg["community"].toString()};QString captures=argv[4];QDir().mkpath(captures);
 signerScope(bridge);auto initial=bridge.read({bridge.identity,"global",{}, {}});ui::SocialAccessDialog dialog(bridge.identity,initial,bridge.services(),"global",{});dialog.show();app.processEvents();
 auto* roles=dialog.findChild<QListWidget*>("pgn-social-role-list");auto* members=dialog.findChild<QListWidget*>("pgn-social-member-list");auto* permissions=dialog.findChild<QListWidget*>("pgn-social-role-permissions");auto* name=dialog.findChild<QLineEdit*>("pgn-social-role-name");auto* position=dialog.findChild<QSpinBox*>();require(roles&&members&&permissions&&name&&position,"Real Roles dialog controls");
 if(bridge.actor=="guest"){
  require(!button(dialog,"pgn-social-new-role")->isEnabled(),"Guest cannot create role");require(!button(dialog,"pgn-social-delete-role")->isEnabled(),"Guest cannot delete role");require(!name->isEnabled(),"Guest read-only role editor");dialog.grab().save(captures+"/guest-read-only.png");
 }else{
  select(roles,"Moderator");require(!button(dialog,"pgn-social-delete-role")->isEnabled(),"Managed moderator cannot be deleted");
  click(dialog,"pgn-social-new-role");name->setText("Support");position->setValue(10);for(int i=0;i<permissions->count();++i)if(permissions->item(i)->data(Qt::UserRole)=="react")permissions->item(i)->setCheckState(Qt::Checked);
  const auto before=bridge.reads;click(dialog,"pgn-social-save-role");require(bridge.reads==before+1,"Accepted action automatically refreshes roles");select(roles,"Support");require(position->value()==10,"Created role position persists");
  name->setText("Support équipe Ω");position->setValue(12);click(dialog,"pgn-social-save-role");select(roles,"Support équipe Ω");require(position->value()==12,"Edit sends current position");
  const QString role=roles->currentItem()->data(Qt::UserRole).toString();dialog.grab().save(captures+"/roles-edited.png");
  select(members,"Guest fixture");auto* combo=dialog.findChild<QComboBox*>("pgn-social-member-role");require(combo!=nullptr,"Role assignment control");combo->setCurrentIndex(combo->findData(role));click(dialog,"pgn-social-assign-role");
  auto check=bridge.read({bridge.identity,"global",cfg["actors"].toObject()["guest"].toObject()["account"].toString(),role});bool assigned=false;for(const auto& m:check.members)if(m.name=="Guest fixture")assigned=m.roleIds.contains(role);require(assigned,"Real signed role assignment persisted after backend process restart");
  auto* tabs=dialog.findChild<QTabWidget*>();tabs->setCurrentIndex(1);dialog.grab().save(captures+"/member-assigned.png");
  click(dialog,"pgn-social-remove-role");check=bridge.read({bridge.identity,"global",cfg["actors"].toObject()["guest"].toObject()["account"].toString(),role});for(const auto& m:check.members)if(m.name=="Guest fixture")require(!m.roleIds.contains(role),"Role removal persists");
  tabs->setCurrentIndex(0);select(roles,"Support équipe Ω");click(dialog,"pgn-social-delete-role");bool found=false;for(int i=0;i<roles->count();++i)found|=roles->item(i)->text()=="Support équipe Ω";require(!found,"Deleted role disappears automatically");
 }
 require(!dialog.findChild<QLabel*>("pgn-social-access-status")->text().contains("unavailable",Qt::CaseInsensitive),"No unavailable service state");
 QJsonObject result{{"PASS",true},{"checks",checks},{"reads",bridge.reads},{"writes",bridge.writes},{"actor",bridge.actor},{"actual_native_proofs",true}};std::cout<<QJsonDocument(result).toJson(QJsonDocument::Compact).toStdString()<<std::endl;return 0;
 }catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<std::endl;return 1;}}
