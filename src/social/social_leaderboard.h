#pragma once
#include "social/social_data.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QRegularExpression>
namespace pgn::social {
// The scoped wallet bridge calls this same bounded grammar. No arbitrary RPC
// method, hidden historical-day query or signing operation is introduced.
inline bool validLeaderboardRpcArguments(const QJsonArray& args){
 if(args.size()<2||args.size()>5)return false;
 const auto offset=args[0].toInteger(-1),limit=args[1].toInteger(-1);
 if(!args[0].isDouble()||offset<0||args[0].toDouble()!=double(offset)||!args[1].isDouble()||limit<1||limit>100||args[1].toDouble()!=double(limit))return false;
 const auto anchor=[&]{return args[2].isString()&&QRegularExpression("^[0-9a-f]{64}$").match(args[2].toString()).hasMatch();};
 if(args.size()==2)return true;
 if(args.size()==3)return anchor();
 Window window=Window::AllTime;
 for(const auto candidate:{Window::Day,Window::Week,Window::Month,Window::Hour})if(args[3]==QJsonValue(leaderboardPeriodName(candidate)))window=candidate;
 if(window==Window::AllTime)return false;
 if(args.size()==4)return offset==0&&args[2].isNull();
 const auto start=args[4].toInteger(-1);
 return anchor()&&args[4].isDouble()&&args[4].toDouble()==double(start)&&validLeaderboardPeriod({args[2].toString(),0,start},window);
}
inline bool leaderboardArguments(const ReadRequest& request,QJsonArray& args){
 if(request.section!=Section::Miners||leaderboardPeriodName(request.window).isEmpty()||request.limit<1||request.limit>100)return false;
 qint64 offset=0,utcStart=0;QString anchor;
 if(!request.cursor.isEmpty()){
  const auto parts=request.cursor.split(':');bool ok=false;
  if(parts.size()!=(request.window!=Window::AllTime?3:2)||!QRegularExpression("^[0-9a-f]{64}$").match(parts[0]).hasMatch())return false;
  offset=parts[1].toLongLong(&ok);if(!ok||offset<0||QString::number(offset)!=parts[1])return false;anchor=parts[0];
  if(request.window!=Window::AllTime){utcStart=parts[2].toLongLong(&ok);if(!ok||QString::number(utcStart)!=parts[2]||!validLeaderboardPeriod({anchor,0,utcStart},request.window))return false;}
 }
 args={offset,request.limit};
 if(request.window!=Window::AllTime){args.append(anchor.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(anchor));args.append(leaderboardPeriodName(request.window));if(!anchor.isEmpty())args.append(utcStart);}
 else if(!anchor.isEmpty())args.append(anchor);
 return validLeaderboardRpcArguments(args);
}
inline QString decodeLeaderboard(const ReadRequest& request,const QJsonObject& value,Snapshot& out){
 const QRegularExpression hex("^[0-9a-f]{64}$"),decimal("^(0|[1-9][0-9]{0,255})$");QJsonArray args;
 if(!leaderboardArguments(request,args)||value.value("schema")!="PGN_LEADERBOARD_V1"||value.value("sort")!="validated_search_positions_desc_then_operator_id_asc"||value.value("genesis")!=request.identity.genesis||!hex.match(value.value("bestblockhash").toString()).hasMatch()||!value.value("rows").isArray()||value.value("rows").toArray().size()>request.limit)return "Invalid authenticated leaderboard page.";
 const auto tip=value.value("bestblockhash").toString();const auto height=value.value("height").toInteger(-1),offset=args[0].toInteger(),next=value.value("next_offset").toInteger(-1),total=value.value("total").toInteger(-1);
 const auto rows=value.value("rows").toArray();
 if(height<0||value.value("offset").toInteger(-1)!=offset||offset>std::numeric_limits<qint64>::max()-rows.size()||next!=offset+rows.size()||total<next||!value.value("more").isBool()||value.value("more").toBool()!=(next<total)||(value.value("more").toBool()&&rows.isEmpty())||(!request.cursor.isEmpty()&&args[2].toString()!=tip))return "Leaderboard snapshot or pagination changed; refresh the ranking.";
 LeaderboardPeriod period{tip,height,{}};period.total=total;
 if(request.window!=Window::AllTime){
  const auto start=value.value("utc_start").toInteger(-1),end=value.value("utc_end").toInteger(-1);period.utcStart=start;
  const auto expectedEnd=leaderboardWindowEnd(request.window,start);
  if(value.value("period")!=leaderboardPeriodName(request.window)||!validLeaderboardPeriod(period,request.window)||!expectedEnd||end!=*expectedEnd||value.value("history_complete")!=QJsonValue(true)||value.value("event_time_source")!="canonical_block_header_time"||value.value("validation_date_scope")!="latest_current_qualification_episode"||(!request.cursor.isEmpty()&&args[4].toInteger()!=start))return "Leaderboard UTC interval or event provenance changed; refresh the ranking.";
 }else if((value.contains("period")&&value.value("period")!="total")||value.contains("utc_start")||value.contains("utc_end"))return "Unexpected UTC interval in Total leaderboard.";
 Snapshot decoded;decoded.identity=request.identity;decoded.section=Section::Miners;decoded.window=request.window;decoded.authenticated=true;decoded.observedUtc=QDateTime::currentDateTimeUtc();decoded.source="PGN verified chain · block "+QString::number(height)+" · "+tip;decoded.leaderboardPeriod=period;
 decoded.leaderboardWindows={Window::AllTime,Window::Day};
 const auto supported=value.value("supported_periods");
 if(supported.isArray()&&supported.toArray().size()<=5){
  for(const auto candidate:{Window::Week,Window::Month,Window::Hour})if(supported.toArray().contains(leaderboardPeriodName(candidate)))decoded.leaderboardWindows.append(candidate);
 }
 if(!decoded.leaderboardWindows.contains(request.window))return "This Core has not advertised the requested UTC calendar period.";
 decoded.largestGapSupported=value.value("largest_gap_scope")=="unique_disclosed_gaps_per_operator_from_current_qualified_reports"&&value.value("largest_gap_time_source")=="first_qualified_disclosure_canonical_header_time";
 const Provenance provenance{request.identity.genesis,decoded.source,decoded.observedUtc,true};QSet<QString> seen;
 for(const auto& entry:rows){
  const auto row=entry.toObject();const auto id=row.value("operator_id").toString();if(!hex.match(id).hasMatch()||seen.contains(id))return "Invalid or repeated operator identity.";seen.insert(id);
  Miner miner;miner.id=id;miner.name=publicIdentityDisplayName(id);
  // The signed report label is public, self-declared and non-unique. The
  // canonical operator ID remains the sole row/permission identity.
  const auto label=row.value("display_name");QString publicName;
  if(label.isString()){
   const auto name=label.toString().trimmed();bool safe=!name.isEmpty()&&name.toUtf8().size()<=192;
   for(const auto ch:name)if(ch.isNull()||ch.category()==QChar::Other_Control||ch.category()==QChar::Other_Format||ch.category()==QChar::Separator_Line||ch.category()==QChar::Separator_Paragraph)safe=false;
   if(safe){publicName=name;miner.name=publicIdentityDisplayName(id,name);}
  }
  if(row.contains("signing_scripts")||row.contains("signing_scripts_total")||row.contains("signing_scripts_more")){
   const auto aliases=row.value("signing_scripts");const auto count=row.value("signing_scripts_total").toInteger(-1);const auto more=row.value("signing_scripts_more");
   if(!aliases.isArray()||aliases.toArray().size()>8||count<aliases.toArray().size()||!more.isBool()||more.toBool()!=(count>aliases.toArray().size()))return "Invalid authenticated signing-alias registry.";
   PublicIdentity identity{id,publicName,{},!more.toBool(),provenance};QSet<QString> scripts;
   for(const auto& entry:aliases.toArray()){const auto script=entry.toString();if(!entry.isString()||!publicScript(script)||scripts.contains(script))return "Invalid authenticated signing alias.";scripts.insert(script);identity.signingScripts.append(script);}
   miner.publicIdentity=std::move(identity);
  }

  for(const auto& item:QList<QPair<QString,Counter*>>{{"tickets_reported",&miner.ticketsReported},{"tickets_validated",&miner.ticketsValidated},{"audits_finalized_valid",&miner.validatedAudits},{"validated_search_positions",&miner.validatedSearchPositions}}){const auto raw=row.value(item.first);if(!raw.isString()||!decimal.match(raw.toString()).hasMatch())return "An exact miner counter is unavailable.";*item.second={raw.toString(),provenance};}
  const bool rosetteScope=value.value("rosette_counting_unit")=="unique_disclosed_gap_records_per_operator"&&value.value("rosette_history_scope")=="qualified_report_disclosures";
  const auto counts=row.value("rosettes").toObject();const QStringList tiers{"q","g","s","b","r"};
  for(int tier=0;tier<5;++tier){
   auto& reason=miner.rosetteReasons[tier];reason=rosetteScope?QString("No complete recorded-discovery count supplied. No zero inferred."):QString("This Core does not supply the supported recorded-discovery scope.");
   const auto item=counts.value(tiers[tier]);if(!item.isObject())continue;const auto counter=item.toObject();
   if(counter.value("complete")!=QJsonValue(true)){if(counter.value("reason").isString())reason=counter.value("reason").toString().left(256);continue;}
   const auto raw=counter.value("count");if(!rosetteScope||!raw.isString()||!decimal.match(raw.toString()).hasMatch()){reason="Malformed or unsupported recorded-discovery count; no zero inferred.";continue;}
   auto evidence=provenance;evidence.source="Recorded discoveries; not coin payments · block "+QString::number(height)+" · "+tip;
   miner.rosettes[tier]={raw.toString(),evidence};reason.clear();
  }
  miner.largestGapReason="This Core does not provide the supported recorded-gap scope.";
  if(decoded.largestGapSupported){
   const auto gap=row.value("largest_recorded_gap").toObject();miner.largestGapReason=gap.value("reason").toString("No complete disclosed gap supplied.").left(256);
   if(gap.value("complete")==QJsonValue(true)&&gap.contains("gap")){
    const auto text=gap.value("gap");bool ok=false;const auto size=text.toString().toULongLong(&ok);
    if(!text.isString()||!decimal.match(text.toString()).hasMatch()||!ok||!size||size>4294967296ULL||size%2)return "Invalid exact recorded-gap length.";
    auto evidence=provenance;evidence.source="Qualified first disclosure · UTC publication · block "+QString::number(height)+" · "+tip;
    miner.largestRecordedGap={text.toString(),evidence};miner.largestGapReason.clear();
   }
  }
  const auto last=row.value("last_activity_height").toInteger(-1);if(last<0||last>height)return "Invalid last-activity height.";miner.lastActivityHeight={QString::number(last),provenance};decoded.miners.append(miner);
 }
 if(value.value("more").toBool())decoded.nextCursor=tip+":"+QString::number(next)+(request.window!=Window::AllTime?":"+QString::number(*period.utcStart):QString{});
 out=std::move(decoded);
 return {};
}
}
