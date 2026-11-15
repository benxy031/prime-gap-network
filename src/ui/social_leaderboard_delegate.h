#pragma once
#include "ui/social_presentation.h"
#include <QStyledItemDelegate>
#include <QPainter>
namespace pgn::ui {
// Presentation only: exact integer strings and canonical identities remain in
// the table model for sorting, accessibility, copy and permission targets.
class SocialLeaderboardDelegate final:public QStyledItemDelegate {
public:using QStyledItemDelegate::QStyledItemDelegate;
 void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index)const override{
    painter->save();painter->setRenderHint(QPainter::Antialiasing);
    const bool selected=option.state&QStyle::State_Selected,hover=option.state&QStyle::State_MouseOver;
    painter->fillRect(option.rect,QColor(selected?"#c9dde2":hover?"#e0ecef":index.row()%2?"#e8f0f2":"#f0f5f6"));
    painter->setPen(QColor("#d3e0e4"));painter->drawLine(option.rect.bottomLeft(),option.rect.bottomRight());
    auto font=option.font;font.setFamily("Segoe UI");font.setPixelSize(14);painter->setFont(font);
    const int col=index.column();const auto value=index.data().toString();const auto rect=option.rect.adjusted(12,0,-12,0);
    if(col==0){
        const auto id=index.data(Qt::UserRole).toString();const QRect avatar(rect.left(),rect.center().y()-18,36,36);
        painter->setPen(Qt::NoPen);painter->setBrush(SocialAvatarColor(id));painter->drawEllipse(avatar);font.setPixelSize(12);font.setWeight(QFont::DemiBold);painter->setFont(font);painter->setPen(QColor("#214650"));painter->drawText(avatar,Qt::AlignCenter,SocialAvatarInitials(id,value));
        auto label=value;label.remove(QRegularExpression(" · [0-9a-f]{8}…[0-9a-f]{6}$"));
        const auto name=QRect(avatar.right()+12,rect.top()+13,qMax(20,rect.width()-50),22);font.setPixelSize(14);font.setWeight(QFont::DemiBold);painter->setFont(font);painter->setPen(QColor("#1a3c47"));painter->drawText(name,Qt::AlignVCenter,painter->fontMetrics().elidedText(label,Qt::ElideRight,name.width()));
        font.setPixelSize(11);font.setWeight(QFont::Normal);painter->setFont(font);painter->setPen(QColor("#637f8b"));painter->drawText(name.translated(0,21),Qt::AlignVCenter,id.size()>18?id.left(8)+"…"+id.right(6):id);
    }else if(col==6){font.setPixelSize(15);font.setWeight(QFont::DemiBold);painter->setFont(font);painter->setPen(QColor("#65828d"));painter->drawText(rect,Qt::AlignCenter,value);
    }else if(col>=7){font.setPixelSize(19);painter->setFont(font);painter->setPen(QColor("#8da4ac"));painter->drawText(rect,Qt::AlignCenter,value);
    }else{font.setWeight(col==1?QFont::DemiBold:QFont::Normal);painter->setFont(font);painter->setPen(QColor(value=="Unavailable"?"#829ba4":col==1?"#176275":"#375967"));painter->drawText(rect,Qt::AlignRight|Qt::AlignVCenter,value=="Unavailable"?QString::fromUtf8("—"):value);}
    if(option.state&QStyle::State_HasFocus){painter->setBrush(Qt::NoBrush);painter->setPen(QPen(QColor("#397c8d"),2));painter->drawRoundedRect(option.rect.adjusted(2,2,-2,-2),5,5);}
    painter->restore();
 }
};
}
