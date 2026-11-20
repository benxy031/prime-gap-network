#pragma once
#include <QRegularExpression>
#include <QPainter>
#include <QTimer>
#include <QWidget>
#include <QTableWidgetItem>
namespace pgn::ui {
inline QString exactWorkTicketNumber(const QString& input)
{
    static const QRegularExpression pattern(R"(^(?:ticket\s+)?([0-9]{1,80})$)",QRegularExpression::CaseInsensitiveOption);
    const auto match=pattern.match(input.trimmed());
    if(!match.hasMatch())return {};
    const QString value=match.captured(1);
    const auto first=value.indexOf(QRegularExpression("[1-9]"));
    return first<0?QString("0"):value.mid(first);
}
inline constexpr int WorkTicketSearchDelayMs=100;

class WorkSearchIndicator final:public QWidget {
    QTimer timer_;
    int angle_{};
protected:
    void paintEvent(QPaintEvent*) override{
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(QPen(QColor("#69efc5"),1.7,Qt::SolidLine,Qt::RoundCap));
        painter.drawArc(rect().adjusted(2,2,-2,-2),angle_*16,250*16);
    }
public:
    explicit WorkSearchIndicator(QWidget* parent=nullptr):QWidget(parent){
        setFixedSize(16,16);
        auto policy=sizePolicy();
        policy.setRetainSizeWhenHidden(true);
        setSizePolicy(policy);
        setAccessibleName("Search in progress");
        setToolTip("Search in progress");
        timer_.setInterval(45);
        connect(&timer_,&QTimer::timeout,this,[this]{angle_=(angle_+25)%360;update();});
        hide();
    }
    bool active()const{return timer_.isActive();}
    void setActive(bool active){
        if(active==timer_.isActive())return;
        if(active)timer_.start();
        else timer_.stop();
        setVisible(active);
    }
};

class WorkTableItem final : public QTableWidgetItem {
public:
    using QTableWidgetItem::QTableWidgetItem;
    void setNumber(const QString& value){setData(Qt::UserRole+4,value);}
    bool operator<(const QTableWidgetItem& other)const override{
        const auto left=data(Qt::UserRole+4).toString(),right=other.data(Qt::UserRole+4).toString();
        if(left.isEmpty()||right.isEmpty())return left.isEmpty()!=right.isEmpty()?left.isEmpty():QTableWidgetItem::operator<(other);
        return left.size()!=right.size()?left.size()<right.size():left<right;
    }
};
}
