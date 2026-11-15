#pragma once
#include <QJsonObject>
#include <QString>

namespace pgn::ui {
inline QString auditActivityHeadline(const QJsonObject& view){
    const auto totals=view.value("totals").toObject();
    return QString("%1 assigned/committed audit slots · %2 reports · block %3%4")
        .arg(totals.value("active_jobs").toInteger()).arg(totals.value("active_reports").toInteger())
        .arg(view.value("state").toObject().value("height").toInteger())
        .arg(view.value("truncated").toBool()?" · bounded visual sample":"");
}
inline QString auditActivityExplanation(const QJsonObject& view){
    return "Network-wide confirmed audit slots: assigned and not expired, or answer committed. "
        "Pending slots additionally include slots awaiting an auditor or the next-block seed. "
        "Each report requires two slots. Assignment does not prove that computation is running. "
        "The ticket table counts tickets; the atlas groups their stages. Snapshot: "+
        view.value("state").toObject().value("bestblockhash").toString();
}
}
