#pragma once
#include <QColor>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <QIcon>
#include <QPainter>
#include <QTableWidgetItem>
#include <QTreeWidgetItem>
#include <QLabel>

namespace pgn::ui {
struct AuditStatusPresentation {QString text,tooltip;QColor color;};

inline AuditStatusPresentation auditStatusPresentation(const QString& state){
    if(state.isEmpty()||state=="awaiting")return {"Awaiting auditor","This audit remains pending until an eligible auditor is assigned.",QColor("#e6bd64")};
    if(state=="assigned")return {"Assigned","An auditor holds this assignment. Assignment alone does not prove that computation is running.",QColor("#ba9aee")};
    if(state=="committed")return {"Answer committed","The auditor committed its answer; reveal and comparison are still required.",QColor("#ba9aee")};
    if(state=="passed")return {"Counts match","The revealed sampling counts match. Exact gap proof and monetary acceptance remain separate checks.",QColor("#63dfb3")};
    if(state=="failed")return {"Mismatch","The revealed sampling counts disagree. This report must not be presented as validated.",QColor("#f37b7e")};
    return {"Unknown state","Unrecognized audit state: "+state,QColor("#99aeb8")};
}

// Ticket stages aggregate audit jobs. A search assignment is distinct from
// an assigned audit; assigned and committed audit slots share the atlas lane.
inline AuditStatusPresentation ticketStagePresentation(const QString& state){
    if(state=="assigned")return {"Assigned","Search ticket assigned; no report has been submitted.",QColor("#70badb")};
    auto result=auditStatusPresentation(state=="pending_audits"?"awaiting":state=="audits_assigned"?"assigned":state=="audits_passed"?"passed":state=="audit_failed"?"failed":"unknown");
    if(state=="pending_audits")result.text="Awaiting audits";
    if(state=="audits_assigned")result.text="Audits assigned / committed";
    if(state=="audits_passed")result.text="Assigned audits passed";
    if(state=="audit_failed")result.text="Audit failed";
    return result;
}
inline QString ticketStageLegendText(){
    QStringList entries;
    for(const auto* state:{"assigned","pending_audits","audits_assigned","audits_passed","audit_failed"}){
        const auto status=ticketStagePresentation(state);
        entries.append(QString("<span style='color:%1'>● %2</span>").arg(status.color.name(),status.text));
    }
    return entries.join(" &nbsp; ");
}

inline AuditStatusPresentation auditReportPresentation(const QJsonObject& report){
    bool assigned=false,committed=false,unknown=false;
    for(const auto& entry:report.value("jobs").toArray()){
        const auto state=entry.toObject().value("state").toString();
        if(state=="failed")return auditStatusPresentation("failed");
        // Match the native atlas: a passed slot keeps an unqualified report in Checking, while expired assignments return to Pending.
        assigned|=state=="passed"||(state=="assigned"&&!entry.toObject().value("expired").toBool());committed|=state=="committed";
        unknown|=state!="assigned"&&state!="committed"&&state!="passed";
    }
    if(unknown)return auditStatusPresentation("unknown");
    if(report.value("audited")==true){auto result=auditStatusPresentation("passed");result.text="Assigned audits passed";return result;}
    if(committed)return auditStatusPresentation("committed");
    if(assigned){auto result=auditStatusPresentation("assigned");result.text="Audits assigned";return result;}
    auto result=auditStatusPresentation("awaiting");result.text="Awaiting audits";return result;
}

inline QIcon auditStatusIcon(const QColor& color){
    QPixmap image(12,12);image.fill(Qt::transparent);QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);painter.setPen(Qt::NoPen);painter.setBrush(color);painter.drawEllipse(QRectF(2,2,8,8));return QIcon(image);
}
inline void presentAuditStatus(QTableWidgetItem* item,const AuditStatusPresentation& status){
    item->setText(status.text);item->setForeground(status.color);item->setIcon(auditStatusIcon(status.color));item->setToolTip(status.tooltip);
}
inline void presentAuditStatus(QTreeWidgetItem* item,const AuditStatusPresentation& status){
    item->setText(1,status.text);item->setForeground(1,status.color);item->setIcon(1,auditStatusIcon(status.color));item->setToolTip(1,status.tooltip);
}
inline QLabel* auditStatusLegend(QWidget* parent){
    auto* label=new QLabel(parent);label->setObjectName("pgn-audit-status-legend");label->setWordWrap(true);
    QStringList entries;for(const auto& state:{"awaiting","assigned","committed","passed","failed"}){const auto status=auditStatusPresentation(state);entries.append(QString("<span style='color:%1'>● %2</span>").arg(status.color.name(),status.text));}label->setText(entries.join(" &nbsp; "));
    label->setToolTip("Colors describe confirmed audit states, not inferred live worker activity. Pending audit reports do not expire.");return label;
}
}
