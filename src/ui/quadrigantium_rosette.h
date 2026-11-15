// R227 approved dark emerald/silver four-facet rosette. Presentation only.
// No merit rule, counter, Native identity or Mining renderer is modified.
#pragma once
#include "ui/gap_rosette.h"
#include <cmath>
namespace pgn::ui {
inline QIcon quadrigantiumRosetteIcon(){
 static const QIcon cached=[] {
  QIcon icon;
  for(const int scale:{1,2,3}){
   QPixmap sample(24*scale,28*scale);sample.setDevicePixelRatio(scale);sample.fill(Qt::transparent);
   QPainter p(&sample);const auto tier=MeritTier::Silver;const bool greenSilver=true;const int variant=3;
const bool referenceImprovement=false;const QColor base=meritColor(tier);
p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);
        p.setBrush(referenceImprovement?QColor("#43ac87"):base.darker(125));
        p.drawPolygon(QPolygonF{{7,15},{4,26},{9,23},{12,26},{13,16}});
        p.setBrush(base.darker(110));p.drawPolygon(QPolygonF{{12,16},{13,26},{17,23},{21,26},{17,15}});
        if(greenSilver){p.setPen(Qt::NoPen);p.setBrush(QColor("#216B50"));p.drawPolygon(QPolygonF{{8.2,17},{6.3,24.3},{7.7,23.3},{9.4,17}});p.drawPolygon(QPolygonF{{14.3,17},{16.6,23.4},{18.2,24.4},{15.7,17}});}
        const QPointF center(12,10.5);const double step=std::acos(-1.)/16;
        for(int fold=0;fold<32;++fold){
            const double a=fold*step,b=(fold+1)*step;
            const QPointF outer=center+QPointF(std::cos(a),std::sin(a))*(fold%2?8.2:10.0);
            const QPointF next=center+QPointF(std::cos(b),std::sin(b))*((fold+1)%2?8.2:10.0);
            p.setBrush(referenceImprovement&&fold%4<2?QColor("#43ac87"):fold%2?base.darker(112):base.lighter(113));
            if(greenSilver&&fold%4<2)p.setBrush(fold%2?QColor("#174B3A"):QColor("#3D8C6C"));
            p.drawPolygon(QPolygonF{center,outer,next});
        }
        p.setPen(QPen(base.lighter(135),.8));p.setBrush(QColor("#10272c"));p.drawEllipse(center,5.7,5.7);
        p.setPen(Qt::NoPen);p.setBrush(base);
        if(referenceImprovement){
            QPolygonF star;
            for(int i=0;i<10;++i){const double a=i*std::acos(-1.)/5-std::acos(-1.)/2;star<<center+QPointF(std::cos(a),std::sin(a))*(i%2?1.8:4.0);}
            p.drawPolygon(star);
        }else p.drawEllipse(center,2.3,2.3);


 const QPointF c(12,10.5);const QColor pale("#b4d6d2");p.setPen(Qt::NoPen);p.setBrush(QColor("#10272c"));p.drawEllipse(c,5.25,5.25);
 if(variant==0){p.setBrush(pale);for(const QPointF offset:{QPointF(-2,-2),QPointF(2,-2),QPointF(2,2),QPointF(-2,2)})p.drawEllipse(c+offset,1.18,1.18);}
 else if(variant==1){p.setBrush(QColor(Qt::transparent));QPen pen(pale,1.35);pen.setCapStyle(Qt::RoundCap);pen.setJoinStyle(Qt::RoundJoin);p.setPen(pen);for(int part=0;part<4;++part){QPolygonF arc;for(int n=0;n<=12;++n){const qreal angle=(part*90+12+n*5.5)*std::acos(-1.)/180;arc<<c+QPointF(std::cos(angle),std::sin(angle))*3.65;}p.drawPolyline(arc);}}
 else{const QPointF points[]{QPointF(0,-4.1),QPointF(4.1,0),QPointF(0,4.1),QPointF(-4.1,0)};const QColor colors[]{QColor("#d5eeeb"),QColor("#a1cdc8"),QColor("#79aaa5"),QColor("#b7dbd6")};for(int i=0;i<4;++i){p.setBrush(colors[i]);p.drawPolygon(QPolygonF{c,c+points[i],c+points[(i+1)%4]});}
  if(variant==3){p.setPen(QPen(QColor("#10272c"),.32));p.setBrush(QColor("#a8d6c9"));p.drawPolygon(QPolygonF{{15.5,13.7},{16.0,13.5},{17.2,15.6},{16.3,15.4}});}
 }

   p.end();icon.addPixmap(sample);icon.addPixmap(sample,QIcon::Selected);icon.addPixmap(sample,QIcon::Active);
  }
  return icon;
 }();
 return cached;
}
}
