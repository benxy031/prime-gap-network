#pragma once
#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QObject>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <memory>

namespace pgn::wallet {
enum class CoreRpcChannel { General, Inspection };
inline constexpr int CoreInspectionTimeoutMs = 12000;

class CoreRpcChannels {
public:
    CoreRpcChannels(){
        general_.setProxy(QNetworkProxy::NoProxy);
        inspection_.setProxy(QNetworkProxy::NoProxy);
    }

    QNetworkReply* post(QNetworkRequest request, const QByteArray& body,
                        int timeout_ms, CoreRpcChannel channel = CoreRpcChannel::General){
        const int idleTimeout = std::clamp(timeout_ms, 1000, 120000);
        request.setTransferTimeout(idleTimeout);
        // Held wallet or scientific requests can occupy every ordinary HTTP connection.
        auto& network = channel == CoreRpcChannel::Inspection ? inspection_ : general_;
        auto* reply = network.post(request, body);
        if (channel == CoreRpcChannel::Inspection) {
            // Transfer activity extends the idle timeout. A partial reply
            // must still release the inspection so the next read can retry.
            QTimer::singleShot(2 * idleTimeout, Qt::PreciseTimer, reply, [reply] {
                if (reply->isFinished()) return;
                reply->setProperty("pgn_core_rpc_total_timeout", true);
                reply->abort();
            });
        }
        return reply;
    }
private:
    QNetworkAccessManager general_, inspection_;
};

// Drain incrementally: a finished-only reader can fill Qt's buffer and then
// wait forever for a server that cannot finish sending the remaining bytes.
inline void readBoundedCoreRpcReply(QNetworkReply* reply,QObject* context,qsizetype maximum,
                                   std::function<void(QByteArray,bool)> consume){
 struct State {QByteArray bytes;bool oversized{},settled{};};
 auto state=std::make_shared<State>();
 const auto drain=[reply,state,maximum]{
  if(state->oversized)return;
  const auto remaining=maximum+1-state->bytes.size();
  if(remaining>0)state->bytes.append(reply->read(remaining));
  if(state->bytes.size()>maximum){state->oversized=true;reply->abort();}
 };
 reply->setReadBufferSize(65536);
 QObject::connect(reply,&QIODevice::readyRead,context,drain);
 QObject::connect(reply,&QNetworkReply::finished,context,[state,drain,consume=std::move(consume)]()mutable{
  if(state->settled)return;
  state->settled=true; // abort() may emit finished synchronously inside drain.
  drain();consume(std::move(state->bytes),state->oversized);
 });
}
}
