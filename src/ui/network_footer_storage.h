
#pragma once
#include <QJsonObject>
#include <QString>
#include "wallet/chain_observation.h"

namespace pgn::ui {
inline QString nativeBlockStorageSuffix(const QJsonObject& totals,const pgn::wallet::ChainObservation& anchor,
                                        const QString& field){
    if(totals.value("genesis")!=anchor.genesis||totals.value("anchor_hash")!=anchor.tipHash||
       totals.value("height").toInteger(-1)!=anchor.height)return {};
    const auto bytes=totals.value(field).toInteger(-1);
    if(bytes<0)return {};
    constexpr qint64 bytesPerMegabyte=1'000'000;
    constexpr qint64 bytesPerGigabyte=1'000'000'000;
    const bool gigabytes=bytes>=bytesPerGigabyte;
    const auto unit=gigabytes?bytesPerGigabyte:bytesPerMegabyte;
    return QString(" (%1 %2)").arg(double(bytes)/unit,0,'f',1).arg(gigabytes?"GB":"MB");
}
}
