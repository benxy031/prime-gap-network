#pragma once
#include "wallet/core_profile.h"
#include "wallet/core_rpc.h"
#include <QDateTime>
#include <QRegularExpression>
#include <QJsonArray>
#include <cmath>
#include <optional>

namespace pgn::wallet {
// Read-only wallet->presentation boundary. No GPU/miner types or calls.
// Numbers are observations of the verified owned Core, never engine fields,
// fallback defaults, reward rules or inferred network throughput.
struct ChainObservation {
    CoreEpoch owner;
    QString genesis;
    qint64 height{};
    QDateTime observedUtc;
    std::optional<bool> synchronizing;
    QString tipHash;
    std::optional<qint64> peers;
    std::optional<qint64> headerHeight;
    std::optional<bool> coreInitialBlockDownload;
    std::optional<qint64> tipTime;
    bool nativePeerCaughtUp{};
    QJsonObject nativePeerDiagnostic;
    bool oldTipAt(const QDateTime& now)const {
        return tipTime&&now.isValid()&&now.toSecsSinceEpoch()-*tipTime>24*60*60;
    }
    bool valid()const {
        static const QRegularExpression hash("^[0-9a-fA-F]{64}$");
        return owner.matches(owner)&&height>=0&&height<=9007199254740991LL&&
            observedUtc.isValid()&&hash.match(genesis).hasMatch()&&
            (tipHash.isEmpty()||hash.match(tipHash).hasMatch());
    }
    bool freshAt(const QDateTime& now,qint64 maximumAgeMs=30000)const {
        if(!valid()||!now.isValid()||maximumAgeMs<0)return false;
        const auto age=observedUtc.msecsTo(now);return age>=0&&age<=maximumAgeMs;
    }
};
struct GoldBlockReading {
    qint64 count{};
    qint64 height{};
    bool atTip{};
};

class GoldBlockStatus {
public:
    quint64 branch()const {return branch_;}
    bool observe(const ChainObservation& tip) {
        if(!tip.valid()||tip.tipHash.isEmpty())return false;
        const bool changed=lastTip_&&(!lastTip_->owner.matches(tip.owner)||lastTip_->genesis!=tip.genesis||
            tip.height<lastTip_->height||(tip.height==lastTip_->height&&tip.tipHash!=lastTip_->tipHash));
        if(changed){++branch_;sample_.reset();}
        lastTip_=tip;
        if(pending_){
            if(!sameCore(*pending_,tip)||pending_->height<tip.height)pending_.reset();
            else if(atTip(*pending_,tip)){sample_=pending_;pending_.reset();}
        }
        return changed;
    }
    void observeState(const QJsonObject& state,const CoreEpoch& owner,const QDateTime& now) {
        const auto candidate=project(state,owner,now);
        if(!candidate)return;
        if(atTip(*candidate,*lastTip_)){sample_=candidate;pending_.reset();}
        else if(candidate->height>=lastTip_->height)pending_=candidate;
    }
    void observeConfirmedState(const QJsonObject& state,const CoreEpoch& owner,quint64 requestedBranch,const QDateTime& now) {
        const auto candidate=project(state,owner,now);
        if(!candidate)return;
        if(atTip(*candidate,*lastTip_)||(requestedBranch==branch_&&candidate->height<lastTip_->height&&
            (!sample_||candidate->height>=sample_->height)))sample_=candidate;
    }
    std::optional<GoldBlockReading> reading(const ChainObservation& tip,const QDateTime& now)const {
        if(!sample_||!tip.freshAt(now)||tip.tipHash.isEmpty()||!sameCore(*sample_,tip))return {};
        const bool exact=atTip(*sample_,tip);
        const auto age=sample_->at.msecsTo(now);
        if(!exact&&(sample_->height>=tip.height||age<0||age>90000))return {};
        return GoldBlockReading{sample_->count,sample_->height,exact};
    }
private:
    struct Sample {CoreEpoch owner;QString genesis,hash;qint64 height,count;QDateTime at;};
    static bool sameCore(const Sample& sample,const ChainObservation& tip){
        return sample.owner.matches(tip.owner)&&sample.genesis==tip.genesis;
    }
    static bool atTip(const Sample& sample,const ChainObservation& tip){
        return sample.height==tip.height&&sample.hash==tip.tipHash;
    }
    std::optional<Sample> project(const QJsonObject& state,const CoreEpoch& owner,const QDateTime& now)const{
        static const QRegularExpression hashPattern("^[0-9a-f]{64}$");
        if(!lastTip_||!lastTip_->freshAt(now)||!owner.matches(lastTip_->owner)||state.value("genesis")!=lastTip_->genesis)return {};
        const auto height=state.value("height").toInteger(-1),count=state.value("gold_blocks").toInteger(-1);
        const auto hash=state.value("bestblockhash").toString();
        if(height<0||height>9007199254740991LL||count<0||count>height||!hashPattern.match(hash).hasMatch())return {};
        return Sample{owner,lastTip_->genesis,hash,height,count,now};
    }
    std::optional<Sample> sample_,pending_;
    std::optional<ChainObservation> lastTip_;
    quint64 branch_{};
};
struct ChainObservationResult {std::optional<ChainObservation> observation;QString error;};
inline ChainObservationResult projectChainObservation(const CoreEpoch& owner,const CoreProfile& profile,
    const QJsonObject& chain,const QString& genesis,const QDateTime& observedUtc){
    static const QRegularExpression hash("^[0-9a-fA-F]{64}$");
    if(!owner.matches(owner)||!observedUtc.isValid()||!hash.match(profile.genesisHash).hasMatch())
        return {{},"Core observation lacks a pinned chain, owned epoch or original observation time"};
    const auto mismatch=verifyCoreIdentity(profile,chain,genesis);
    if(!mismatch.isEmpty())return {{},mismatch};
    const auto height=chain.value("blocks");const double number=height.toDouble(-1);
    if(!height.isDouble()||!std::isfinite(number)||number<0||number>9007199254740991.0||std::trunc(number)!=number)
        return {{},"Core block height missing, invalid or outside exact JSON integer domain"};
    const auto sync=chain.value("initialblockdownload");
    const auto tip=chain.value("bestblockhash");
    if(!tip.isUndefined()&&(!tip.isString()||!hash.match(tip.toString()).hasMatch()))return {{},"Core tip hash is invalid"};
    auto observation=ChainObservation{owner,genesis.toLower(),height.toInteger(),observedUtc.toUTC(),
        sync.isBool()?std::optional<bool>(sync.toBool()):std::nullopt,tip.toString().toLower()};
    observation.coreInitialBlockDownload=observation.synchronizing;
    const auto exact=[](const QJsonValue& value)->std::optional<qint64>{
        const auto number=value.toDouble(-1);
        if(!value.isDouble()||!std::isfinite(number)||number<0||number>9007199254740991.0||std::trunc(number)!=number)return {};
        return value.toInteger();
    };
    observation.headerHeight=exact(chain.value("headers"));observation.tipTime=exact(chain.value("time"));
    return {observation,{}};
}
// The owned Core retains its IBD heuristic, including tip age. Native work
// readiness describes validated header/body catch-up with our explicit local
// peer bridge; it never changes Core validation or claims monetary acceptance.
inline ChainObservation projectNativeSynchronization(ChainObservation value,const QJsonArray& peers,const QString& endpoint){
    value.nativePeerCaughtUp=false;
    value.nativePeerDiagnostic={};
    if(endpoint.isEmpty()||peers.size()>512||!value.coreInitialBlockDownload||!value.headerHeight||*value.headerHeight<value.height){value.synchronizing.reset();return value;}
    bool found=false,ambiguous=false,pending=*value.headerHeight>value.height;
    QJsonObject candidate;
    const auto knownHeight=[](const QJsonValue& entry,qint64 floor){
        const auto n=entry.toInteger(floor-1);
        return n>=floor&&n<=9007199254740991LL;
    };
    for(const auto& entry:peers){
        if(!entry.isObject()){value.synchronizing.reset();return value;}
        const auto peer=entry.toObject();if(peer.value("addr").toString()!=endpoint)continue;
        const auto height=peer.value("pgn_announced_height"),sync=peer.value("pgn_syncing");const auto number=height.toDouble(-1);
        if(peer.value("connection_type")!="manual"||!height.isDouble()||!std::isfinite(number)||number<0||number>9007199254740991.0||std::trunc(number)!=number||!sync.isBool()){value.synchronizing.reset();return value;}
        ambiguous=ambiguous||found;
        if(!found){
            candidate={{"announced_height",height},{"syncing",sync}};
            for(const auto* key:{"synced_headers","synced_blocks"}){
                const auto observed=peer.value(key);
                if(knownHeight(observed,-1))candidate.insert(key,observed);
            }
            const auto inFlight=peer.value("inflight");
            if(inFlight.isArray()){
                const auto heights=inFlight.toArray();
                bool valid=heights.size()<=512;
                for(const auto& requested:heights)valid=valid&&knownHeight(requested,0);
                if(valid){
                    candidate.insert("inflight_count",heights.size());
                    if(!heights.isEmpty())candidate.insert("first_inflight_height",heights.first());
                }
            }
        }
        found=true;pending=pending||sync.toBool()||value.height<height.toInteger();
    }
    if(found&&!ambiguous)value.nativePeerDiagnostic=std::move(candidate);
    value.synchronizing=!found||pending||value.height<1;
    value.nativePeerCaughtUp=found&&!pending&&value.height>0;
    return value;
}
}
