#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <optional>
namespace pgn::ui {
// A Gold count is useful only when the latest verified metric is bound to the
// same ledger record. The consensus ledger height remains technical metadata.
inline std::optional<qint64> networkGoldCount(const QJsonObject& snapshot) {
 const auto points=snapshot.value("points").toArray();
 if(points.isEmpty())return {};
 const auto last=points.last().toObject();
 const auto ledger=snapshot.value("height").toInteger(-1);
 const auto value=last.value("gold_count");
 const auto gold=value.toInteger(-1);
 if(ledger<0||!value.isDouble()||gold<0||gold>ledger||value.toDouble()!=double(gold)
    ||last.value("height").toInteger(-1)!=ledger
    ||snapshot.value("bestblockhash").toString().isEmpty()
    ||last.value("hash")!=snapshot.value("bestblockhash"))return {};
 return gold;
}
inline QString networkMetricsCaption(const QJsonObject& snapshot,const QString& error) {
 if(!error.isEmpty())return QString("Network metrics unavailable · %1%2").arg(error,
   snapshot.isEmpty()?QString{}:QString(" · last verified snapshot retained"));
 const auto gold=networkGoldCount(snapshot);
 return gold?QString("Network · Gold blocks %1 · 10-minute chain-time estimate").arg(*gold)
            :QString("Network · Gold block count unavailable");
}
inline QString networkMetricsTechnicalTip(const QJsonObject& snapshot) {
 const auto height=snapshot.value("height").toInteger(-1);
 return height<0?QString{}:QString("Technical coordination ledger record: %1").arg(height);
}
}
