#pragma once
#include "ui/social_presentation.h"
#include <QHeaderView>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTableWidget>
#include <QTimer>
#include <functional>

namespace pgn::ui {
// Presentation roles only. The existing model's exact values, provenance and
// canonical ID remain authoritative; no rank, presence or award is fabricated.
enum LeaderboardRole { LeaderboardOwnRole=Qt::UserRole+180, LeaderboardRankRole };
class LeaderboardDelegate final:public QStyledItemDelegate {
public:
    explicit LeaderboardDelegate(QObject* parent):QStyledItemDelegate(parent){}
    QSize sizeHint(const QStyleOptionViewItem&,const QModelIndex&)const override{return {80,36};}
    void paint(QPainter* p,const QStyleOptionViewItem& option,const QModelIndex& index)const override{
        p->save();p->setClipRect(option.rect);p->setRenderHint(QPainter::Antialiasing);
        const bool selected=option.state&QStyle::State_Selected,hover=(option.state&QStyle::State_MouseOver)||(parent()&&parent()->property("leaderboardHoverRow").toInt()==index.row()),own=index.data(LeaderboardOwnRole).toBool();
        p->fillRect(option.rect,selected?QColor("#bdd8d4"):hover?QColor("#d4e4e4"):own?QColor("#d9e8e3"):QColor("#e9f0f0"));
        p->setPen(QColor("#d3dfe0"));p->drawLine(option.rect.bottomLeft(),option.rect.bottomRight());
        QFont body("Segoe UI");body.setPixelSize(13);p->setFont(body);p->setPen(QColor("#203c43"));
        if(index.column()==0){
            const auto id=index.data(Qt::UserRole).toString(),name=index.data().toString();
            auto rect=option.rect;const auto rank=index.data(LeaderboardRankRole).toInt();
            QFont small("Segoe UI");small.setPixelSize(11);p->setFont(small);p->setPen(QColor("#36575f"));p->drawText(QRect(rect.left()+8,rect.top(),26,36),Qt::AlignCenter,QString::number(rank));
            const QRect avatar(rect.left()+42,rect.top()+6,24,24);p->setPen(Qt::NoPen);p->setBrush(SocialAvatarColor(id));p->drawEllipse(avatar);
            small.setWeight(QFont::DemiBold);p->setFont(small);p->setPen(QColor("#214b50"));p->drawText(avatar,Qt::AlignCenter,SocialAvatarInitials(id,name));
            const int x=avatar.right()+10,w=rect.right()-x-10;body.setWeight(QFont::DemiBold);p->setFont(body);p->setPen(QColor("#18383f"));
            p->drawText(QRect(x,rect.top()+2,w,17),Qt::AlignVCenter,p->fontMetrics().elidedText(name,Qt::ElideRight,w));
            small.setWeight(QFont::Normal);small.setPixelSize(11);p->setFont(small);p->setPen(QColor("#36575f"));
            p->drawText(QRect(x,rect.top()+19,w,14),Qt::AlignVCenter,p->fontMetrics().elidedText(id,Qt::ElideMiddle,w));
            if(own){p->setPen(Qt::NoPen);p->setBrush(QColor("#397d70"));p->drawRoundedRect(QRect(rect.left()+1,rect.top()+7,3,22),1,1);}
        }else{
            // Segoe UI's numeric text is right aligned; the exact integer stays
            // in the native model tooltip and accessible description.
            const bool award=index.column()>=6;p->setPen(award?QColor("#36575f"):QColor("#294950"));
            p->drawText(option.rect.adjusted(10,0,-14,0),award?Qt::AlignCenter:Qt::AlignRight|Qt::AlignVCenter,index.data().toString());
        }
        if(option.state&QStyle::State_HasFocus){p->setBrush(Qt::NoBrush);p->setPen(QPen(QColor("#397e79"),1));p->drawRoundedRect(option.rect.adjusted(2,2,-2,-3),4,4);}
        p->restore();
    }
};
class LeaderboardHeader final:public QHeaderView {
    int focused_{1},hovered_{-1};
public:
    std::function<void(int)> onSort;
    explicit LeaderboardHeader(QWidget* parent):QHeaderView(Qt::Horizontal,parent){
        setFocusPolicy(Qt::StrongFocus);setSectionsClickable(true);setSortIndicatorShown(true);setHighlightSections(false);setMouseTracking(true);setFixedHeight(58);setDefaultAlignment(Qt::AlignRight|Qt::AlignVCenter);
        setObjectName("pgn-social-miner-sort-header");setAccessibleName("Sort leaderboard. Left and Right choose a metric. Enter or Space changes order.");
        connect(this,&QHeaderView::sectionClicked,this,[this](int col){if(col<6){focused_=col;if(onSort)onSort(col);}});
    }
protected:
    void keyPressEvent(QKeyEvent* event)override{
        const int key=event->key();
        if(key==Qt::Key_Left||key==Qt::Key_Right){int visual=visualIndex(focused_);do{visual+=key==Qt::Key_Right?1:-1;}while(visual>=0&&visual<count()&&logicalIndex(visual)>=6);if(visual>=0&&visual<count())focused_=logicalIndex(visual);viewport()->update();event->accept();return;}
        if(key==Qt::Key_Home||key==Qt::Key_End){focused_=key==Qt::Key_Home?0:5;viewport()->update();event->accept();return;}
        if(key==Qt::Key_Return||key==Qt::Key_Enter||key==Qt::Key_Space){if(onSort)onSort(focused_);viewport()->update();event->accept();return;}
        QHeaderView::keyPressEvent(event);
    }
    void mouseMoveEvent(QMouseEvent* event)override{hovered_=logicalIndexAt(event->position().toPoint());viewport()->update();QHeaderView::mouseMoveEvent(event);}
    void leaveEvent(QEvent* event)override{hovered_=-1;viewport()->update();QHeaderView::leaveEvent(event);}
    void paintSection(QPainter* p,const QRect& r,int col)const override{
        if(!r.isValid())return;p->save();p->fillRect(r,col==hovered_&&col<6?QColor("#d3e3e4"):QColor("#dce8e9"));
        p->setPen(QColor("#bfced1"));p->drawLine(r.bottomLeft(),r.bottomRight());
        QFont font("Segoe UI");font.setPixelSize(11);font.setWeight(QFont::DemiBold);p->setFont(font);p->setPen(QColor("#46666e"));
        if(col>=6){const auto icon=qvariant_cast<QIcon>(model()->headerData(col,orientation(),Qt::DecorationRole));icon.paint(p,QRect(r.center().x()-12,r.top()+25,24,28));}
        else{
            const QStringList captions{"Miner","Positions","Reported","Validated","Audits","Last activity"};
            const QStringList groups{"","VALIDATED","","","VALIDATED","BLOCK HEIGHT"};
            const QRect textRect(r.left()+(col?8:16),r.top()+27,r.width()-(col?26:30),23);
            p->setPen(QColor("#244951"));font.setPixelSize(12);p->setFont(font);p->drawText(textRect,col?Qt::AlignRight|Qt::AlignVCenter:Qt::AlignLeft|Qt::AlignVCenter,captions[col]);
            font.setPixelSize(10);p->setFont(font);p->setPen(QColor("#46666e"));p->drawText(r.adjusted(12,4,-14,-30),col?Qt::AlignRight|Qt::AlignVCenter:Qt::AlignLeft|Qt::AlignVCenter,groups[col]);
            if(sortIndicatorSection()==col){p->setPen(Qt::NoPen);p->setBrush(QColor("#337c73"));const int x=r.right()-8,y=r.top()+39;QPolygon triangle;const int d=sortIndicatorOrder()==Qt::AscendingOrder?-1:1;triangle<<QPoint(x-3,y-2*d)<<QPoint(x+3,y-2*d)<<QPoint(x,y+2*d);p->drawPolygon(triangle);}
        }
        if(hasFocus()&&focused_==col){p->setBrush(Qt::NoBrush);p->setPen(QPen(QColor("#39827b"),2));p->drawRoundedRect(r.adjusted(3,3,-3,-3),5,5);}
        p->restore();
    }
    void paintEvent(QPaintEvent* event)override{
        QHeaderView::paintEvent(event);QPainter p(viewport());QFont f("Segoe UI");f.setPixelSize(10);f.setWeight(QFont::DemiBold);p.setFont(f);p.setPen(QColor("#46666e"));
        for(const auto& group:QList<QPair<QPair<int,int>,QString>>{{{6,10},"AWARDS"},{{2,3},"TICKETS"}}){const int a=group.first.first,b=group.first.second;const QRect r(sectionViewportPosition(a),4,sectionViewportPosition(b)+sectionSize(b)-sectionViewportPosition(a),20);p.drawText(r,Qt::AlignCenter,group.second);}
    }
};
class LeaderboardTable final:public QTableWidget {
    void layoutColumns(){
        if(columnCount()!=11)return;const int available=viewport()->width(),minimum[]{264,148,110,110,100,146},weights[]{4,2,1,1,1,1};
        const int extra=qMax(0,available-1098);int used=220;auto* header=horizontalHeader();header->setStretchLastSection(false);
        for(int col=0;col<6;++col){header->setSectionResizeMode(col,QHeaderView::Fixed);const int size=minimum[col]+extra*weights[col]/10;header->resizeSection(col,size);used+=size;}
        // Integer remainder belongs to the identity, never a surprise scroll.
        if(available>=1098)header->resizeSection(0,header->sectionSize(0)+available-used);
    }
public:
    explicit LeaderboardTable(QWidget* parent=nullptr):QTableWidget(0,11,parent){setProperty("leaderboardHoverRow",-1);setItemDelegate(new LeaderboardDelegate(this));setShowGrid(false);setMouseTracking(true);setFrameShape(QFrame::NoFrame);setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);setMinimumHeight(100);}
    QSize sizeHint()const override{int rows=0;for(int i=0;i<rowCount();++i)if(!isRowHidden(i))++rows;return {1100,58+qMax(1,rows)*36+2};}
    QSize minimumSizeHint()const override{return {0,100};}
protected:
    void mouseMoveEvent(QMouseEvent* event)override{const int row=indexAt(event->position().toPoint()).row();if(property("leaderboardHoverRow").toInt()!=row){setProperty("leaderboardHoverRow",row);viewport()->update();}QTableWidget::mouseMoveEvent(event);}
    bool viewportEvent(QEvent* event)override{if(event->type()==QEvent::Leave){setProperty("leaderboardHoverRow",-1);viewport()->update();}if(event->type()==QEvent::Resize)QTimer::singleShot(0,this,[this]{layoutColumns();});return QTableWidget::viewportEvent(event);}
    void resizeEvent(QResizeEvent* event)override{QTableWidget::resizeEvent(event);layoutColumns();QPainterPath p;p.addRoundedRect(QRectF(rect()),12,12);setMask(QRegion(p.toFillPolygon().toPolygon()));}
};
class LeaderboardStack final:public QStackedWidget {
public:
    QSize sizeHint()const override{return currentWidget()?currentWidget()->sizeHint():QSize(1100,160);}
    QSize minimumSizeHint()const override{return {0,100};}
};
}
