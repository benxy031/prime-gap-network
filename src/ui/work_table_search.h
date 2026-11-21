#pragma once
#include <QRegularExpression>
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
