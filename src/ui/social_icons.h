#pragma once
#include <QIcon>
#include <QIconEngine>
#include <QPainter>
#include <QPainterPath>

namespace pgn::ui {
// Native vector paths, painted at the requested size/DPI. No font-glyph or
// external/raster asset dependency. Button names/tooltips carry the semantics.
enum class SocialGlyph { Search, Refresh, Members, More, Preview, Send, Link, Quote, Code, Info, Bold, Italic, Underline, Strike };
class SocialIconEngine final:public QIconEngine {
    SocialGlyph glyph_;QColor ink_;
public:
    SocialIconEngine(SocialGlyph glyph,QColor ink):glyph_(glyph),ink_(ink){}
    QIconEngine* clone()const override{return new SocialIconEngine(glyph_,ink_);}
    QPixmap pixmap(const QSize& size,QIcon::Mode mode,QIcon::State state)override{QPixmap image(size);image.fill(Qt::transparent);QPainter p(&image);paint(&p,QRect(QPoint{},size),mode,state);return image;}
    void paint(QPainter* p,const QRect& rect,QIcon::Mode mode,QIcon::State)override{
        p->save();p->setRenderHint(QPainter::Antialiasing);const auto scale=qMin(rect.width(),rect.height())/20.;
        p->translate(rect.x()+(rect.width()-20*scale)/2,rect.y()+(rect.height()-20*scale)/2);p->scale(scale,scale);
        const QColor color=mode==QIcon::Disabled?QColor("#849aa4"):ink_;
        p->setPen(QPen(color,1.6,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));p->setBrush(Qt::NoBrush);
        switch(glyph_){
        case SocialGlyph::Search:p->drawEllipse(QPointF(8.5,8.5),5.2,5.2);p->drawLine(QPointF(12.4,12.4),QPointF(17,17));break;
        case SocialGlyph::Refresh:{QPainterPath path;path.moveTo(15.8,6.5);path.cubicTo(13,1.5,5,2.1,3.4,7.2);path.cubicTo(.5,14,8,19.2,13.8,15.3);p->drawPath(path);p->drawPolyline(QPolygonF{{11.7,6.5},{16,6.5},{16,2.2}});break;}
        case SocialGlyph::Members:{p->drawEllipse(QPointF(7.5,6.2),2.6,2.6);QPainterPath path;path.moveTo(2.7,16);path.cubicTo(2.7,10,12.3,10,12.3,16);p->drawPath(path);path=QPainterPath();path.moveTo(13,4);path.cubicTo(17,3.5,17,9,13,9);path.moveTo(14.2,11.2);path.cubicTo(17.5,11.2,18.3,13,18.3,16);p->drawPath(path);break;}
        case SocialGlyph::More:p->setBrush(color);p->setPen(Qt::NoPen);for(int x:{4,10,16})p->drawEllipse(QPointF(x,10),1.25,1.25);break;
        case SocialGlyph::Preview:{QPainterPath path;path.moveTo(1.5,10);path.cubicTo(6,3,14,3,18.5,10);path.cubicTo(14,17,6,17,1.5,10);p->drawPath(path);p->drawEllipse(QPointF(10,10),2.4,2.4);break;}
        case SocialGlyph::Send:p->drawPolyline(QPolygonF{{2.3,9},{17.3,2.7},{11,17.7},{8.5,11.5},{2.3,9}});p->drawLine(QPointF(8.5,11.5),QPointF(17.3,2.7));break;
        case SocialGlyph::Link:{QPainterPath path;path.moveTo(8,5.4);path.cubicTo(12,-.1,20,5.6,14.6,10.2);path.lineTo(12.7,12);path.moveTo(12,14.6);path.cubicTo(8,20.1,0,14.4,5.4,9.8);path.lineTo(7.3,8);p->drawPath(path);p->drawLine(QPointF(7.3,12.7),QPointF(12.7,7.3));break;}
        case SocialGlyph::Quote:p->drawPolyline(QPolygonF{{7.8,5},{3,5},{3,10},{7.8,10},{5.4,15}});p->drawPolyline(QPolygonF{{17,5},{12.2,5},{12.2,10},{17,10},{14.6,15}});break;
        case SocialGlyph::Code:p->drawPolyline(QPolygonF{{6,5},{1.8,10},{6,15}});p->drawPolyline(QPolygonF{{14,5},{18.2,10},{14,15}});p->drawLine(QPointF(11.5,3),QPointF(8.5,17));break;
        case SocialGlyph::Info:p->drawEllipse(QPointF(10,10),7.1,7.1);p->drawLine(QPointF(10,9),QPointF(10,14));p->drawPoint(QPointF(10,5.9));break;
        case SocialGlyph::Bold:{p->setPen(QPen(color,2.1,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));QPainterPath q;q.moveTo(6,3);q.lineTo(6,17);q.lineTo(11,17);q.cubicTo(17,17,17,10,11,10);q.lineTo(6,10);q.moveTo(6,3);q.lineTo(10.5,3);q.cubicTo(16,3,16,10,10.5,10);p->drawPath(q);break;}
        case SocialGlyph::Italic:p->drawLine(QPointF(8,3),QPointF(15,3));p->drawLine(QPointF(5,17),QPointF(12,17));p->drawLine(QPointF(12,3),QPointF(8,17));break;
        case SocialGlyph::Underline:{QPainterPath q;q.moveTo(5,3);q.lineTo(5,10);q.cubicTo(5,17,15,17,15,10);q.lineTo(15,3);p->drawPath(q);p->drawLine(QPointF(4,18),QPointF(16,18));break;}
        case SocialGlyph::Strike:{QPainterPath q;q.moveTo(15,5);q.cubicTo(11,0,3,3,5,8);q.cubicTo(7,11,15,9,15,14);q.cubicTo(15,19,7,19,4.5,15);p->drawPath(q);p->drawLine(QPointF(2,10),QPointF(18,10));break;}
        }
        p->restore();
    }
};
inline QIcon SocialIcon(SocialGlyph glyph,QColor ink=QColor("#46636f")){return QIcon(new SocialIconEngine(glyph,ink));}
}
