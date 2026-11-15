#pragma once
#include "ui/audit_status_presentation.h"
#include <QApplication>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QStyle>
namespace pgn::ui {
struct AuditSlotIndicator {QString state,label,tooltip;QColor color;};
inline AuditSlotIndicator auditSlotIndicator(const QJsonObject& report,int slot,bool submitted){
 auto unavailable=[&](const QString& why){return AuditSlotIndicator{"unavailable","Unavailable",why,QColor("#91a8b2")};};
 if(report.isEmpty())return unavailable(submitted?"Audit details have not been loaded.":"No report has been submitted for this ticket.");
 if(!report.value("jobs").isArray())return unavailable("Audit slot data is unavailable.");
 QJsonObject found;int matches=0;
 for(const auto& value:report.value("jobs").toArray()){
  const auto job=value.toObject();const auto index=job.value("slot");
  if(index.isDouble()&&index.toDouble()==slot){found=job;++matches;}
 }
 if(matches>1)return unavailable("Conflicting entries for this audit slot; no verdict is inferred.");
 if(!matches){
  if(report.value("audited").toBool())return unavailable("The report is validated, but this slot's details are unavailable.");
  const auto p=auditStatusPresentation("awaiting");return {"awaiting",p.text,p.tooltip,p.color};
 }
 const auto state=found.value("state").toString();
 if(state=="assigned"&&found.value("expired").toBool())return {"expired","Assignment expired","The reservation expired; no successful audit is inferred.",QColor("#e6bd64")};
 if(state!="assigned"&&state!="committed"&&state!="passed"&&state!="failed")return unavailable("Unrecognized audit slot state.");
 const auto p=auditStatusPresentation(state);return {state,state=="passed"?"Passed":p.text,p.tooltip,p.color};
}
inline QIcon auditSlotIcon(const AuditSlotIndicator& value){
 QPixmap image(32,32);image.setDevicePixelRatio(2);image.fill(Qt::transparent);QPainter painter(&image);
 painter.setRenderHint(QPainter::Antialiasing);painter.setPen(QPen(value.color,2,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));painter.setBrush(Qt::NoBrush);
 if(value.state=="passed"){QPainterPath path;path.moveTo(3,8);path.lineTo(6.5,11.5);path.lineTo(13,4.5);painter.drawPath(path);}
 else if(value.state=="failed"){painter.drawLine(QPointF(4,4),QPointF(12,12));painter.drawLine(QPointF(12,4),QPointF(4,12));}
 else if(value.state=="assigned"||value.state=="awaiting")painter.drawEllipse(QRectF(3,3,10,10));
 else if(value.state=="committed"){painter.setBrush(value.color);painter.drawEllipse(QRectF(3,3,10,10));}
 else painter.drawLine(QPointF(4,8),QPointF(12,8));
 return QIcon(image);
}
inline void presentAuditSlot(QTableWidgetItem* item,const QJsonObject& report,int slot,bool submitted){
 const auto value=auditSlotIndicator(report,slot,submitted);item->setText({});item->setIcon(auditSlotIcon(value));item->setTextAlignment(Qt::AlignCenter);
 const auto label=QString("Audit %1: %2").arg(slot+1).arg(value.label);
 item->setData(Qt::AccessibleTextRole,label);item->setData(Qt::UserRole+2,value.state);item->setToolTip(label+"\n"+value.tooltip);
}
class AuditSlotDelegate final:public QStyledItemDelegate {
public:using QStyledItemDelegate::QStyledItemDelegate;
 void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index)const override{
  QStyleOptionViewItem background(option);initStyleOption(&background,index);const auto icon=background.icon;background.icon={};background.text.clear();
  background.features.setFlag(QStyleOptionViewItem::HasDecoration,false);background.features.setFlag(QStyleOptionViewItem::HasDisplay,false);
  const auto* style=option.widget?option.widget->style():QApplication::style();style->drawControl(QStyle::CE_ItemViewItem,&background,painter,option.widget);
  icon.paint(painter,QRect(option.rect.center()-QPoint(8,8),QSize(16,16)),Qt::AlignCenter);
 }
 QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex&)const override{return {48,option.fontMetrics.height()+6};}
};
}
