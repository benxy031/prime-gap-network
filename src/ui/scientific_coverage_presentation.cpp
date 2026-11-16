#include "scientific_coverage_presentation.h"
#include <boost/multiprecision/cpp_int.hpp>
#include <QJsonValue>
#include <QLocale>
#include <QRegularExpression>
#include <optional>

namespace pgn::ui {
namespace {
using Integer=boost::multiprecision::cpp_int;
std::optional<Integer> integer(const QJsonObject& value,const char* name){
 const auto raw=value.value(QLatin1String(name));
 if(!raw.isString())return std::nullopt;
 const auto text=raw.toString();
 static const QRegularExpression canonical(QStringLiteral("^(0|[1-9][0-9]*)$"));
 if(text.size()>512||!canonical.match(text).hasMatch())return std::nullopt;
 return Integer{text.toStdString()};
}
QString decimal(const Integer& value){return QString::fromStdString(value.str());}
}
ScientificCoveragePresentation scientificCoveragePresentation(const QJsonObject& snapshot){
 const auto unavailable=ScientificCoveragePresentation{QStringLiteral("Scientific coverage unavailable"),
  QStringLiteral("No complete, verified coverage summary for this snapshot. No zero is inferred."),false};
 const auto value=snapshot.value(QStringLiteral("scientific_coverage")).toObject();
 static const QRegularExpression hash(QStringLiteral("^[0-9a-f]{64}$"));
 if(!hash.match(snapshot.value("genesis").toString()).hasMatch()||
    !hash.match(snapshot.value("bestblockhash").toString()).hasMatch()||
    value.value("genesis")!=snapshot.value("genesis")||
    value.value("bestblockhash")!=snapshot.value("bestblockhash")||
    !value.value("height").isDouble()||value.value("height")!=snapshot.value("height"))return unavailable;
 const auto origin=integer(value,"origin"),width=integer(value,"range_width"),
  end=integer(value,"contiguous_end"),index=integer(value,"first_missing_index"),
  missingEnd=integer(value,"first_missing_end");
 const auto count=value.value("validated_ranges").toInteger(-1);
 if(!origin||!width||!end||!index||!missingEnd||*width<=0||count<0||
    *end!=*origin+(*width)*(*index)||*missingEnd!=*end+*width||*index>count)return unavailable;
 const auto last=integer(value,"last_validated_end");
 if(count==0){if(*index!=0||value.contains("last_validated_end"))return unavailable;}
 else if(!last||*last<*end||*last<=*origin||(*last-*origin)%*width!=0||
         (*last-*origin)/ *width<count)return unavailable;
 const Integer firstHuman=*index+1;
 ScientificCoveragePresentation result;
 result.available=true;
 result.text=QStringLiteral("Validated ranges: %1 · Consecutive from origin: %2 · First unvalidated range: %3")
  .arg(QLocale(QLocale::English).toString(count),decimal(*index),decimal(firstHuman));
 result.richText=QStringLiteral(
  "<table width='100%' cellspacing='0' cellpadding='3'><tr>"
  "<td width='33%'><span style='font-size:12px;color:#a8c8cb'>Validated ranges</span><br>"
  "<span style='font-size:22px;font-weight:600;color:#d8f3ee'>%1</span></td>"
  "<td width='33%'><span style='font-size:12px;color:#a8c8cb'>Consecutive from origin</span><br>"
  "<span style='font-size:22px;font-weight:600;color:#d8f3ee'>%2</span></td>"
  "<td width='34%'><span style='font-size:12px;color:#f3d49a'>First unvalidated range</span><br>"
  "<span style='font-size:22px;font-weight:600;color:#f3d49a'>%3</span></td></tr></table>")
  .arg(QLocale(QLocale::English).toString(count),decimal(*index),decimal(firstHuman));
 result.tooltip=QStringLiteral("Verified snapshot %1 at height %2.\nConsecutive coverage: [%3, %4).\nFirst unvalidated interval: [%4, %5).\nRange numbering starts at 1 in scientific order, independently of random ticket IDs.")
  .arg(value.value("bestblockhash").toString(),QString::number(value.value("height").toInteger()),
       decimal(*origin),decimal(*end),decimal(*missingEnd));
 return result;
}
}
