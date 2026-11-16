#pragma once
#include "ui/social_access_client.h"
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QTimer>
// Qualification only. Product callbacks remain asynchronous throughout.
template<class Bridge> void httpClientProbe(Bridge& bridge,const QJsonObject& cfg,const QString& folder){
 using namespace pgn;using namespace pgn::social;
 QObject lifetime;int signatures=0,ticks=0;QTimer pulse;pulse.setInterval(2);QObject::connect(&pulse,&QTimer::timeout,&lifetime,[&]{++ticks;});pulse.start();
 auto signer=[&](QByteArray wire,auto done){++signatures;auto* p=new QProcess(&lifetime);p->setProgram(bridge.python);p->setArguments({bridge.driver,bridge.config});
  QObject::connect(p,&QProcess::started,p,[p,wire,&bridge]{p->write(QJsonDocument(QJsonObject{{"action","sign_only"},{"actor",bridge.actor},{"wire",QString::fromUtf8(wire)}}).toJson(QJsonDocument::Compact));p->closeWriteChannel();});
  QObject::connect(p,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),p,[p,done](int code,QProcess::ExitStatus){const auto v=QJsonDocument::fromJson(p->readAllStandardOutput()).object();done(v["signature"].toString(),code==0&&v["ok"]==true?QString{}:v["reason"].toString());p->deleteLater();});p->start();
 };
 const QUrl endpoint("http://127.0.0.1:"+QString::number(cfg["http_port"].toInt())+"/v1/access");const auto journal=folder+"/pending-client.json";
 const auto wait=[&](bool& done){QElapsedTimer t;t.start();while(!done&&t.elapsed()<30000){QCoreApplication::processEvents(QEventLoop::AllEvents,10);QThread::msleep(2);}require(done,"Asynchronous actual HTTP completed");};
 auto read=[&](ui::SocialAccessClient& client){bool done=false;AccessSnapshot snapshot;QString failure;client.read({bridge.identity,"global",{},{}},[&](AccessSnapshot s,QString e){snapshot=s;failure=e;done=true;});wait(done);if(!failure.isEmpty())std::cerr<<failure.toStdString()<<'\n';require(failure.isEmpty(),"Actual product HTTP access snapshot");return snapshot;};
 auto write=[&](ui::SocialAccessClient& client,AccessRequest r){bool done=false;AccessResult result;client.manage(r,[&](AccessResult v){result=v;done=true;});wait(done);require(matchingResult(result,r),"Product HTTP action reply identity");return result;};
 AccessRequest request;request.identity=bridge.identity;request.community=bridge.native.community;request.channel="global";request.action=AccessAction::CreateRole;request.name="HTTP client retry";request.position=11;request.permissions={"react"};request.requestId=QUuid::createUuid().toString(QUuid::WithoutBraces);
 {
  ui::SocialAccessClient client(bridge.identity,bridge.native,endpoint,journal,signer,&lifetime,true);const auto snapshot=read(client);request.revision=snapshot.revision;request.sessionReference=snapshot.session.reference;
  const auto first=write(client,request);
  if(bridge.actor=="guest"){require(first.outcome==AccessOutcome::Denied&&!QFile::exists(journal),"Guest denied by actual backend and terminal retry retired");return;}
  require(first.outcome==AccessOutcome::Unavailable&&QFile::exists(journal),"Lost committed reply retains durable exact envelope");
 }
 const auto before=signatures;
 ui::SocialAccessClient restarted(bridge.identity,bridge.native,endpoint,journal,signer,&lifetime,true);
 request.requestId=QUuid::createUuid().toString(QUuid::WithoutBraces);const auto retried=write(restarted,request);require(retried.outcome==AccessOutcome::Accepted&&signatures==before&&!QFile::exists(journal),"Restart retries identical signed bytes despite new UI UUID, with no new signature");
 const auto snapshot=read(restarted);QString role;int matches=0;for(const auto& r:snapshot.roles)if(r.name==request.name){role=r.id;++matches;}require(matches==1,"Lost reply did not create duplicate role");
 request.action=AccessAction::DeleteRole;request.role=role;request.revision=snapshot.revision;request.requestId=QUuid::createUuid().toString(QUuid::WithoutBraces);require(write(restarted,request).outcome==AccessOutcome::Accepted,"Actual asynchronous role cleanup");require(ticks>20,"Native signing and HTTP did not block UI event processing");
 bool refused=false;ui::SocialAccessClient insecure(bridge.identity,bridge.native,endpoint,folder+"/unused.json",signer,&lifetime);const auto count=signatures;insecure.read({bridge.identity,"global",{},{}},[&](auto,QString error){refused=!error.isEmpty();});require(refused&&signatures==count,"Public adapter refuses unencrypted endpoint before signing");
 std::cout<<"PASS actual asynchronous HTTP: bounded snapshot, durable lost-reply restart, no duplicate write, fresh Native proof, responsive event loop\n";
}
