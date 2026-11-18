#include "wallet/core_rpc_stream.h"
#include "wallet/core_rpc.h"
#include <QCoreApplication>
#include <QEvent>
#include <QEventLoop>
#include <QHostAddress>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <iostream>
#include <vector>
#include <stdexcept>
using namespace pgn::wallet;
static void need(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}

static void exchange(qsizetype bodySize,qsizetype limit,bool keepOpen,bool expectedOversize){
 QTcpServer server;need(server.listen(QHostAddress::LocalHost,0),"Own HTTP fixture unavailable");
 const QByteArray body=QByteArray("{\"jsonrpc\":\"2.0\",\"id\":\"unique\",\"result\":\"")+QByteArray(bodySize,'x')+"\"}";
 QObject::connect(&server,&QTcpServer::newConnection,&server,[&]{
  auto* socket=server.nextPendingConnection();
  QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
  QObject::connect(socket,&QTcpSocket::readyRead,socket,[socket,body,keepOpen]{
   socket->readAll();if(socket->property("served").toBool())return;socket->setProperty("served",true);
   const auto length=body.size()+(keepOpen?100000:0);
   socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(length)+"\r\n\r\n");
   socket->write(body);if(!keepOpen)socket->disconnectFromHost();
  });
 });
 QNetworkAccessManager network;QNetworkRequest request(QUrl("http://127.0.0.1:"+QString::number(server.serverPort())+"/"));
 auto* reply=network.get(request);QEventLoop loop;QTimer deadline;deadline.setSingleShot(true);
 QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);deadline.start(5000);
 int calls=0;bool oversized=false;QByteArray received;
 readBoundedCoreRpcReply(reply,&loop,limit,[&](QByteArray bytes,bool tooLarge){++calls;received=std::move(bytes);oversized=tooLarge;reply->deleteLater();loop.quit();});
 loop.exec();need(deadline.isActive(),"Bounded read waited for a stalled or overflowing server");deadline.stop();
 need(calls==1&&oversized==expectedOversize,"RPC completion must settle once with the right size status");
 if(expectedOversize)need(received.size()==limit+1,"Overflow stopped at the first excess byte");
 else need(received==body,"Valid response bytes changed or were truncated");
 QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);QCoreApplication::processEvents();
 need(calls==1,"Aborting a reply delivered duplicate completion");
}

static void slowInspection(bool dedicated){
 QTcpServer server;
 need(server.listen(QHostAddress::LocalHost,0),"Own slow HTTP fixture unavailable");
 CoreRpcChannels channels;
 QEventLoop loop;
 QTimer deadline;
 deadline.setSingleShot(true);
 QObject::connect(&deadline,&QTimer::timeout,&loop,&QEventLoop::quit);
 int outstanding=0;
 int inspected=0;
 int completions=0;
 bool succeeded=false;
 const QString url="http://127.0.0.1:"+QString::number(server.serverPort());
 const auto inspect=[&]{
  QNetworkRequest request{QUrl(url+"/inspection")};
  auto* reply=channels.post(request,"{}",dedicated?CoreInspectionTimeoutMs:3000,
      dedicated?CoreRpcChannel::Inspection:CoreRpcChannel::General);
  readBoundedCoreRpcReply(reply,&loop,65536,[&,reply](QByteArray bytes,bool oversized){
   ++completions;
   succeeded=!oversized&&decodeCoreRpcResponse(bytes,"health","getblockchaininfo").error.isEmpty();
   reply->deleteLater();
   loop.quit();
  });
 };
 QObject::connect(&server,&QTcpServer::newConnection,&server,[&]{
  auto* socket=server.nextPendingConnection();
  QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
  QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket]{
   if(socket->property("served").toBool()){socket->readAll();return;}
   auto received=socket->property("received").toByteArray()+socket->readAll();
   if(!received.contains("\r\n")){socket->setProperty("received",received);return;}
   socket->setProperty("served",true);
   if(received.startsWith("POST /inspection ")){
    ++inspected;
    QTimer::singleShot(4500,socket,[socket]{
     const QByteArray body=R"({"jsonrpc":"2.0","id":"health","result":{"blocks":4245}})";
     socket->write("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n");
     socket->write(body);
     socket->disconnectFromHost();
    });
    return;
   }
   // Keep every ordinary connection busy until the inspection has finished.
   if(++outstanding==6)inspect();
  });
 });
 std::vector<QNetworkReply*> blocked;
 if(dedicated){
  for(int n=0;n<6;++n)blocked.push_back(channels.post(QNetworkRequest{QUrl(url+"/ordinary")},"{}",60000));
 }else inspect();
 deadline.start(10000);
 loop.exec();
 need(deadline.isActive(),"Inspection did not finish within the fixture deadline");
 need(inspected==1&&completions==1,"Slow inspection was not dispatched and settled exactly once");
 if(dedicated){
  need(outstanding==6&&succeeded,"Slow inspection was delayed behind ordinary wallet traffic");
 }else{
  need(!succeeded,"Three-second legacy read should fail on the 4.5-second response");
 }
 for(auto* reply:blocked){reply->abort();reply->deleteLater();}
 QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);
}
static void recoverIncompleteInspection(){
 QTcpServer server;
 need(server.listen(QHostAddress::LocalHost,0),"Own incomplete RPC fixture unavailable");
 CoreRpcChannels channels;
 QEventLoop loop;
 QTimer guard,heartbeat;
 guard.setSingleShot(true);
 QObject::connect(&guard,&QTimer::timeout,&loop,&QEventLoop::quit);
 int ticks=0,firstCallbacks=0,secondCallbacks=0,dripped=0;
 bool timedOut=false,readRecovered=false;
 heartbeat.setInterval(100);
 QObject::connect(&heartbeat,&QTimer::timeout,&loop,[&]{++ticks;});
 const QString url="http://127.0.0.1:"+QString::number(server.serverPort());
 QObject::connect(&server,&QTcpServer::newConnection,&server,[&]{
  auto* socket=server.nextPendingConnection();
  QObject::connect(socket,&QTcpSocket::disconnected,socket,&QObject::deleteLater);
  QObject::connect(socket,&QTcpSocket::readyRead,socket,[&,socket]{
   const auto request=socket->property("request").toByteArray()+socket->readAll();
   if(socket->property("served").toBool())return;
   if(!request.contains("\r\n")){socket->setProperty("request",request);return;}
   socket->setProperty("served",true);
   if(request.startsWith("POST /good ")){
    const QByteArray body=R"({"jsonrpc":"2.0","id":"health","result":{"blocks":4246}})";
    socket->write("HTTP/1.1 200 OK\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body);
    socket->disconnectFromHost();
    return;
   }
   socket->write("HTTP/1.1 200 OK\r\nContent-Length: 10000\r\n\r\n{");
   auto* drip=new QTimer(socket);
   QObject::connect(drip,&QTimer::timeout,socket,[&,socket]{socket->write(" ");++dripped;});
   drip->start(200);
  });
 });
 auto* slow=channels.post(QNetworkRequest{QUrl(url+"/slow")},"{}",1000,CoreRpcChannel::Inspection);
 readBoundedCoreRpcReply(slow,&loop,65536,[&,slow](QByteArray,bool oversized){
  ++firstCallbacks;
  timedOut=!oversized&&slow->property("pgn_core_rpc_total_timeout").toBool();
  slow->deleteLater();
  auto* resumed=channels.post(QNetworkRequest{QUrl(url+"/good")},"{}",1000,CoreRpcChannel::Inspection);
  readBoundedCoreRpcReply(resumed,&loop,65536,[&,resumed](QByteArray bytes,bool incomplete){
   ++secondCallbacks;
   const auto result=decodeCoreRpcResponse(bytes,"health","getblockchaininfo");
   readRecovered=!incomplete&&result.error.isEmpty()&&result.value.toObject()["blocks"].toInt()==4246;
   resumed->deleteLater();
   loop.quit();
  });
 });
 heartbeat.start();
 guard.start(4300);
 loop.exec();
 need(guard.isActive(),"Incomplete reply kept the inspection stuck despite ongoing bytes");
 need(timedOut&&firstCallbacks==1,"Absolute deadline did not settle the partial inspection once");
 need(readRecovered&&secondCallbacks==1,"A new inspection did not recover on the same read channel");
 need(ticks>=10&&dripped>=4,"The fixture did not exercise an active event loop and trickling response");
 std::cout<<"PASS incomplete RPC deadline and successful next inspection, heartbeat ticks "<<ticks<<"\n";
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);try{
 recoverIncompleteInspection();
 exchange(400000,coreRpcResponseLimit("getpgnauditactivity"),false,false);
 exchange(400000,coreRpcResponseLimit("getbalances"),true,true);
 exchange(600000,coreRpcResponseLimit("getpgnauditactivity"),true,true);
 slowInspection(false);
 slowInspection(true);
 std::cout<<"PASS real HTTP: large native response preserved, excess data stopped before server completion, callback once\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
