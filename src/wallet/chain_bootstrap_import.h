#pragma once

#include "wallet/core_rpc_stream.h"
#include "wallet/chain_observation.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPointer>
#include <QTimer>
#include <utility>
#include <QUrl>
#include <QtEndian>

namespace pgn::wallet {

inline constexpr auto ChainBootstrapGenesis = "710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314";
inline constexpr auto ChainBootstrapTip = "2e3c62ee8ecc9016d7c8da3d8d275d34f63d0f2f8d13bcfb7f9bdf983b26e8be";
inline constexpr auto ChainBootstrapAsset = "https://github.com/wizz13150/prime-gap-network/releases/download/horizon-testnet-epoch3/PGN-Testnet-Bootstrap-22100.pgn-bootstrap";
inline constexpr auto ChainBootstrapAssetDigest = "f628a2a82f7465bcc5d3782a5c9984f52c8ebaeb5b7b3f45fbc81590355f4bec";
inline constexpr auto ChainBootstrapDataDigest = "fc567765de235fe085a63d5328a06c2e58b132d7f6bac5677f36371fecc46f3a";
inline constexpr qint64 ChainBootstrapHeight = 22100;
inline constexpr qsizetype ChainBootstrapAssetBytes = 9086947;
inline constexpr qsizetype ChainBootstrapDataBytes = 33804250;

inline bool needsAutomaticChainBootstrap(const ChainObservation& observation) {
    return observation.valid() && observation.genesis == ChainBootstrapGenesis
        && observation.coreInitialBlockDownload.value_or(false)
        && observation.height < ChainBootstrapHeight;
}

class ChainBootstrapImport final : public QObject {
public:
    using Progress = std::function<void(qint64, const QString&, bool)>;
    using Reply = std::function<void(QJsonValue, QString)>;
    using Read = std::function<void(const QString&, const QJsonArray&, Reply)>;
    using Submit = std::function<void(const QJsonArray&, std::function<void(QJsonArray, QString)>)>;

    ChainBootstrapImport(QObject* parent, Read read, Submit submit, Progress progress)
        : QObject(parent), read_(std::move(read)), submit_(std::move(submit)), progress_(std::move(progress)) {}

    void start(){
        progress_(0, "Downloading the testnet bootstrap…", false);
        QNetworkRequest request{QUrl(QString::fromLatin1(ChainBootstrapAsset))};
        request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,QNetworkRequest::NoLessSafeRedirectPolicy);
        request.setTransferTimeout(30000);
        auto* reply=download_.get(request);
        downloadReply_=reply;
        QTimer::singleShot(60000,this,[this]{
            if(downloadReply_)cancel("Bootstrap download timed out. Peer synchronization continues.");
        });
        readBoundedCoreRpcReply(reply,this,ChainBootstrapAssetBytes,[this,reply](QByteArray payload,bool oversized){
            downloadReply_.clear();
            const auto failed=reply->error()!=QNetworkReply::NoError;
            reply->deleteLater();
            if(finished_)return;
            if(oversized||failed){finish("Bootstrap download failed. Peer synchronization continues.");return;}
            if(!decode(payload)){finish("Bootstrap integrity check failed. Peer synchronization continues.");return;}
            read_("getblockcount",{},[this,weak=QPointer<ChainBootstrapImport>(this)](QJsonValue value,QString error){
                if(!weak||finished_)return;
                const auto height=value.toInteger(-1);
                if(!error.isEmpty()||height<0){finish("Core is unavailable. Retry when it reconnects.");return;}
                accepted_=std::min(height,ChainBootstrapHeight);
                read_("getblockhash",{accepted_},[this,weak](QJsonValue hash,QString error){
                    if(!weak||finished_)return;
                    if(!error.isEmpty()||hash.toString()!=expectedHash(accepted_)){
                        finish("The current chain differs from this bootstrap. Peer synchronization continues.");
                        return;
                    }
                    advance();
                });
            });
        });
    }
    void cancel(const QString& message){
        const auto reply=downloadReply_;
        finish(message);
        if(reply)reply->abort();
    }

private:
    struct Block {qsizetype position, size;};
    static bool digestMatches(const QByteArray& bytes, const char* digest){
        return QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex()==digest;
    }

    bool decode(const QByteArray& payload){
        if(payload.size()!=ChainBootstrapAssetBytes||!digestMatches(payload,ChainBootstrapAssetDigest)
           ||qFromBigEndian<quint32>(payload.constData())!=ChainBootstrapDataBytes) return false;
        bytes_=qUncompress(payload);
        if(bytes_.size()!=ChainBootstrapDataBytes||!digestMatches(bytes_,ChainBootstrapDataDigest))return false;
        const auto magic=bytes_.first(4);
        for(qsizetype position=0;position<bytes_.size();){
            if(bytes_.size()-position<8||QByteArrayView(bytes_).sliced(position,4)!=QByteArrayView(magic))return false;
            const auto size=qFromLittleEndian<quint32>(bytes_.constData()+position+4);
            position+=8;
            if(size<112||size>bytes_.size()-position)return false;
            blocks_.append({position,qsizetype(size)});
            position+=size;
        }
        return blocks_.size()==ChainBootstrapHeight+1;
    }

    QString expectedHash(qint64 height)const{
        if(height==ChainBootstrapHeight)return QString::fromLatin1(ChainBootstrapTip);
        QByteArray parent=bytes_.sliced(blocks_.at(height+1).position+4,32);
        std::reverse(parent.begin(),parent.end());
        return QString::fromLatin1(parent.toHex());
    }

    void advance(){
        if(accepted_==ChainBootstrapHeight){
            read_("getblockhash",{accepted_},[this,weak=QPointer<ChainBootstrapImport>(this)](QJsonValue hash,QString error){
                if(!weak||finished_)return;
                finish(error.isEmpty()&&hash.toString()==expectedHash(accepted_)
                    ?QString("Bootstrap validated through coordination block %1. Continuing with peers.").arg(accepted_)
                    :QString("Could not confirm the bootstrap tip. Peer synchronization continues."));
            });
            return;
        }
        progress_(accepted_,QString("Validating bootstrap blocks · %1 / %2").arg(accepted_).arg(ChainBootstrapHeight),false);
        const qint64 end=std::min(accepted_+64,ChainBootstrapHeight);
        QJsonArray batch;
        for(qint64 height=accepted_+1;height<=end;++height){
            const auto block=blocks_.at(height);
            const auto hex=QByteArrayView(bytes_).sliced(block.position,block.size).toByteArray().toHex();
            batch.append(QJsonObject{{"jsonrpc","2.0"},{"id",height},{"method","submitblock"},{"params",QJsonArray{QString::fromLatin1(hex)}}});
        }
        const auto first=accepted_+1;
        submit_(batch,[this,first,end,weak=QPointer<ChainBootstrapImport>(this)](QJsonArray replies,QString error){
            if(!weak||finished_)return;
            if(!error.isEmpty()||replies.size()!=end-first+1){finish("Core interrupted the import. Retry to resume from its verified height.");return;}
            qint64 height=first;
            for(const auto& entry:replies){
                const auto row=entry.toObject();
                const auto result=row.value("result");
                const auto failure=row.value("error");
                if(row.value("id").toInteger(-1)!=height++||(!failure.isUndefined()&&!failure.isNull())
                   ||(!result.isNull()&&result.toString()!=QStringLiteral("duplicate"))){
                    finish("Core rejected a bootstrap block. Peer synchronization continues.");
                    return;
                }
            }
            accepted_=end;
            advance();
        });
    }

    void finish(const QString& message){
        if(std::exchange(finished_,true))return;
        progress_(accepted_,message,true);
        deleteLater();
    }

    QNetworkAccessManager download_;
    QPointer<QNetworkReply> downloadReply_;
    bool finished_{};
    Read read_;
    Submit submit_;
    Progress progress_;
    QByteArray bytes_;
    QList<Block> blocks_;
    qint64 accepted_{};
};

}
