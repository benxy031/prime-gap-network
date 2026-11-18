#pragma once
#include <QObject>
#include <QCoreApplication>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QRegularExpression>
#include <functional>
#include <algorithm>

namespace pgn::wallet {
inline QString bootstrapDescriptorError(const QJsonObject& descriptor,const QString& genesis){
 if(descriptor.value("schema")!="PGN_TESTNET_BOOTSTRAP_V1"||descriptor.value("genesis").toString()!=genesis)return "Bootstrap belongs to another network";
 const auto checkpoint=descriptor.value("checkpoint").toObject();
 const auto height=checkpoint.value("height");
 if(!height.isDouble()||height.toInt()<1||height.toDouble()!=height.toInt()||!QRegularExpression("^[0-9a-f]{64}$").match(checkpoint.value("hash").toString()).hasMatch())return "Testnet checkpoint is invalid";
 const auto endpoint=descriptor.value("tls").toObject();const auto port=endpoint.value("port");
 if(!QRegularExpression("^[A-Za-z0-9][A-Za-z0-9.-]{0,252}$").match(endpoint.value("host").toString()).hasMatch()||!port.isDouble()||port.toDouble()!=port.toInt()||port.toInt()<1||port.toInt()>65535)return "TLS bootstrap endpoint is invalid";
 return {};
}

// Owns only a client-local TLS bridge. Neither Core nor mining is stopped here.
class PeerBootstrap final:public QObject {
 QProcess* process_;QTimer retry_;QElapsedTimer attachmentAttempt_;QByteArray buffer_;QString program_,host_,local_;int port_{},failures_{};bool closed_{};
 void report(const QString& message){if(onStatus)onStatus(message.left(400));}
 void start(){
  if(closed_||process_->state()!=QProcess::NotRunning)return;
  buffer_.clear();process_->setProgram(program_);process_->setArguments({"--peer-relay","--host",host_,"--port",QString::number(port_)});process_->start();
 }
 void requestAttachment(){
  constexpr qint64 retryIntervalMs=15000;
  if(local_.isEmpty()||!onAttach||(attachmentAttempt_.isValid()&&attachmentAttempt_.elapsed()<retryIntervalMs))return;
  if(onAttach(local_))attachmentAttempt_.start();
 }
 void retry(){
  attachmentAttempt_.invalidate();
  if(!local_.isEmpty()){const auto previous=local_;local_.clear();if(onRemoved)onRemoved(previous);}
  if(!closed_&&!retry_.isActive())retry_.start(std::min(30000,1000*(1<<std::min(5,failures_++))));
 }
public:
 std::function<bool(const QString&)> onAttach;
 std::function<void(const QString&)> onRemoved,onStatus;
 explicit PeerBootstrap(QString program,const QJsonObject& descriptor,QObject* parent=nullptr):QObject(parent),process_(new QProcess(this)),retry_(this),program_(std::move(program)){
  const auto endpoint=descriptor.value("tls").toObject();host_=endpoint.value("host").toString();port_=endpoint.value("port").toInt();retry_.setSingleShot(true);
  connect(&retry_,&QTimer::timeout,this,[this]{start();});
  connect(process_,&QProcess::readyReadStandardError,this,[this]{process_->readAllStandardError();});
  connect(process_,&QProcess::readyReadStandardOutput,this,[this]{
   buffer_.append(process_->readAllStandardOutput());
   if(buffer_.size()>16384){report("TLS bootstrap response exceeds its budget");process_->kill();return;}
   while(buffer_.contains('\n')){
    const auto end=buffer_.indexOf('\n');const auto line=buffer_.left(end);buffer_.remove(0,end+1);
    const auto value=QJsonDocument::fromJson(line).object();
    if(line.size()>4096||value.value("schema")!="PGN_PEER_RELAY_V1"){report("Invalid TLS bootstrap response");process_->kill();return;}
    const auto event=value.value("event").toString();
    if(event=="ready"){
     const auto localPort=value.value("local_port");
     if(!local_.isEmpty()||value.value("local_host")!="127.0.0.1"||!localPort.isDouble()||localPort.toDouble()!=localPort.toInt()||localPort.toInt()<1||localPort.toInt()>65535){report("Invalid client-local bootstrap endpoint");process_->kill();return;}
     local_="127.0.0.1:"+QString::number(localPort.toInt());requestAttachment();
    }else if(event=="connected"){
     if(value.value("tls_verified")!=true){report("Unverified TLS bootstrap refused");process_->kill();return;}
     failures_=0;report("Encrypted testnet connection established");
    }else if(event=="error")report("Testnet connection: "+value.value("message").toString());
   }
  });
  connect(process_,&QProcess::errorOccurred,this,[this](QProcess::ProcessError error){if(error==QProcess::FailedToStart){report("Integrated testnet connection could not start");retry();}});
  connect(process_,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this](int,QProcess::ExitStatus){retry();});
 }
 ~PeerBootstrap()override{
  closed_=true;retry_.stop();
  if(process_->state()==QProcess::NotRunning)return;
  auto* application=QCoreApplication::instance();
  if(!application){process_->kill();return;}
  // This process only forwards network bytes. Retire it asynchronously
  // so Core lifecycle changes cannot block the desktop event loop.
  auto* retiring=process_;
  disconnect(retiring,nullptr,this,nullptr);
  retiring->setParent(application);
  connect(retiring,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),retiring,&QObject::deleteLater);
  connect(retiring,&QProcess::readyReadStandardOutput,retiring,[retiring]{retiring->readAllStandardOutput();});
  connect(retiring,&QProcess::readyReadStandardError,retiring,[retiring]{retiring->readAllStandardError();});
  retiring->write("{\"action\":\"quit\"}\n");
  retiring->closeWriteChannel();
  QTimer::singleShot(3500,retiring,[retiring]{if(retiring->state()!=QProcess::NotRunning)retiring->kill();});
 }
 void launch(){start();}
 QString localEndpoint()const{return local_;}
 void reconcileAttachment(const QJsonArray& peers){
  // Readiness can arrive while Core RPC is unavailable. A verified peer
  // observation lets us retry without relaunching Core or the relay.
  const auto attached=std::any_of(peers.cbegin(),peers.cend(),[this](const auto& entry){
   const auto peer=entry.toObject();
   return peer.value("addr").toString()==local_&&peer.value("connection_type")=="manual";
  });
  if(!attached)requestAttachment();
 }
};
}
