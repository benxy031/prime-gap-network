#undef NDEBUG
#include "wallet/peer_bootstrap.h"
#include "engine/local_runtime_client.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QPointer>
#include <QThread>
#include <cassert>
#include <cstdio>

namespace {
bool observeUntil(const std::function<bool()>& condition, int timeoutMs) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!condition() && elapsed.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QThread::msleep(5);
    }
    return condition();
}

void verifyRelayRecovery(const QJsonObject& descriptor) {
    using pgn::wallet::PeerBootstrap;
    bool coreReady = false;
    int attempts = 0;
    PeerBootstrap relay(QCoreApplication::applicationFilePath(), descriptor);
    relay.onAttach = [&](const QString& endpoint) {
        assert(endpoint == "127.0.0.1:38491");
        if (!coreReady) return false;
        ++attempts;
        return true;
    };
    relay.launch();
    assert(observeUntil([&] { return !relay.localEndpoint().isEmpty(); }, 4000));
    assert(attempts == 0);
    coreReady = true;
    const QJsonObject existing{{"addr", relay.localEndpoint()}, {"connection_type", "manual"}};
    relay.reconcileAttachment(QJsonArray{existing});
    assert(attempts == 0);
    relay.reconcileAttachment({});
    assert(attempts == 1);
    relay.reconcileAttachment({});
    assert(attempts == 1);

    qputenv("PGN_QA_RELAY_IGNORE_QUIT", "1");
    auto slowRelay = std::make_unique<PeerBootstrap>(QCoreApplication::applicationFilePath(), descriptor);
    slowRelay->launch();
    assert(observeUntil([&] { return !slowRelay->localEndpoint().isEmpty(); }, 4000));
    qunsetenv("PGN_QA_RELAY_IGNORE_QUIT");
    const QPointer<QProcess> retiring(slowRelay->findChild<QProcess*>());
    assert(retiring && retiring->state() == QProcess::Running);
    QElapsedTimer delay;
    delay.start();
    slowRelay.reset();
    assert(delay.elapsed() < 1000);
    assert(observeUntil([&] { return retiring.isNull(); }, 5000));
}
}
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);
 if(app.arguments().contains("--peer-relay")){
    std::puts(R"({"schema":"PGN_PEER_RELAY_V1","event":"ready","local_host":"127.0.0.1","local_port":38491})");
    std::fflush(stdout);
    if(qEnvironmentVariableIsSet("PGN_QA_RELAY_IGNORE_QUIT"))QThread::sleep(20);
    char input[256];
    return std::fgets(input,sizeof input,stdin)?0:1;
 }
 const QString genesis(64,'a');
 const QJsonObject valid{{"schema","PGN_TESTNET_BOOTSTRAP_V1"},{"genesis",genesis},{"checkpoint",QJsonObject{{"height",1242},{"hash",QString(64,'b')}}},{"tls",QJsonObject{{"host","bootstrap.example.org"},{"port",8443}}}};
 assert(pgn::wallet::bootstrapDescriptorError(valid,genesis).isEmpty());
 assert(!pgn::wallet::bootstrapDescriptorError(valid,QString(64,'c')).isEmpty());
 for(const auto& port:QJsonArray{0,-1,65536,443.5,"8443",QJsonValue{}}){auto value=valid;auto tls=value["tls"].toObject();tls["port"]=port;value["tls"]=tls;assert(!pgn::wallet::bootstrapDescriptorError(value,genesis).isEmpty());}
 for(const auto& host:QStringList{"","https://example.org","127.0.0.1:443","host\nheader","../bootstrap"}){auto value=valid;auto tls=value["tls"].toObject();tls["host"]=host;value["tls"]=tls;assert(!pgn::wallet::bootstrapDescriptorError(value,genesis).isEmpty());}
 for(const auto& height:QJsonArray{0,-1,1.5,"1242",QJsonValue{}}){auto value=valid;auto checkpoint=value["checkpoint"].toObject();checkpoint["height"]=height;value["checkpoint"]=checkpoint;assert(!pgn::wallet::bootstrapDescriptorError(value,genesis).isEmpty());}
 const QJsonObject device{{"id","GPU-11111111-2222-3333-4444-555555555555"},{"name","GPU"},{"utilization_percent",40},{"power_watts",80},{"memory_used_bytes","1048576"},{"memory_total_bytes","8388608"}};
 QJsonObject hardware{{"schema","PGN_LOCAL_HARDWARE_V1"},{"available",true},{"devices",QJsonArray{device}}};
 const auto sample=pgn::engine::localHardwareSnapshot(hardware);assert(sample&&sample->devices.size()==1&&sample->devices[0].utilizationPercent==40&&!sample->devices[0].throttleTelemetryAuthoritative);
 hardware["available"]=false;assert(!pgn::engine::localHardwareSnapshot(hardware));hardware["available"]=true;hardware["devices"]=QJsonArray{device,device};assert(!pgn::engine::localHardwareSnapshot(hardware));
 verifyRelayRecovery(valid);
 return 0;
}
