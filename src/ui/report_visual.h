#pragma once
#include "ui/gap_flow_motion.h"
#include "ui/merit_palette.h"
#include <array>
#include <QPainter>
#include <QPainterPath>
#include <QHash>
#include <QStringList>
#include <QVector>
namespace pgn::ui {
struct ReportVisual {QVector<MeritTier> tiers; QString number;};
inline QHash<QString,ReportVisual>& reportVisuals(){static QHash<QString,ReportVisual> values;return values;}
inline void rememberReportVisual(const QString& request,QVector<MeritTier> tiers,const QString& number){
 if(request.isEmpty()||(tiers.isEmpty()&&number.isEmpty()))return;
 tiers.erase(std::remove(tiers.begin(),tiers.end(),MeritTier::Ordinary),tiers.end());
 if(tiers.size()>10)tiers.resize(10);
 auto& cache=reportVisuals();
 if(cache.size()>=32&&!cache.contains(request))cache.erase(cache.begin());
 auto& report=cache[request];
 if(!tiers.isEmpty())report.tiers=std::move(tiers);
 if(!number.isEmpty())report.number=number;
}
inline QString reportVisualTooltip(const QString& request,bool published,bool audited){
 const auto report=reportVisuals().value(request);
 const auto label=report.number.isEmpty()?request.left(12):report.number;
 const auto state=audited?"Assigned audits passed":published?"Report recorded · audits pending":"Range computed locally";
 QStringList discoveries;
 for(const auto& entry:{std::pair{MeritTier::Gold,"Gold"},std::pair{MeritTier::Silver,"Silver"},
                         std::pair{MeritTier::Bronze,"Bronze"},std::pair{MeritTier::NearMiss,"Rust"}}){
  const auto count=std::count(report.tiers.cbegin(),report.tiers.cend(),entry.first);
  if(count)discoveries.append(QString("%1 %2").arg(count).arg(entry.second));
 }
 const auto awards=discoveries.isEmpty()?QString("No rosettes observed"):QString("Shown: ")+discoveries.join(", ");
 return QString("Ticket %1\n%2\n%3").arg(label,state,awards);
}
inline double reportVisualScale(const QString& request) {
    const auto count = reportVisuals().value(request).tiers.size();
    return 1.2 + .20 * std::log2(1. + double(count));
}
// The same contour forms the rosette and then the block. The internal edges
// grow only once the boundary is recognisably angular.
inline void drawReportVisual(QPainter& painter, QPointF at, const QString& request,
                             QColor state, double seconds, double scale, double merge) {
    constexpr double turn = 6.283185307179586;
    constexpr int subdivisions = 4;
    const std::array<QPointF, 6> corners{{
        {0,-7}, {6,-3}, {6,3}, {0,7}, {-6,3}, {-6,-3},
    }};
    const double seed = flowSeed(uint(qHash(request)));
    painter.save();
    painter.translate(at);
    painter.scale(scale, scale);

    QRadialGradient haze(QPointF(), 15);
    auto tint = state;
    tint.setAlpha(int(40 * (1. - merge)));
    haze.setColorAt(0, tint);
    tint.setAlpha(0);
    haze.setColorAt(1, tint);
    painter.setPen(Qt::NoPen);
    painter.setBrush(haze);
    painter.drawEllipse(QPointF(), 15, 15);

    const int samples = int(corners.size()) * subdivisions;
    QPainterPath silhouette;
    for (int sample = 0; sample < samples; ++sample) {
        const int side = sample / subdivisions;
        const double along = double(sample % subdivisions) / subdivisions;
        const double angle = turn * (double(sample) / samples - .25);
        const double radius = 7.4 + std::sin(angle * 3 + seconds * .7 + seed * 9) * .65
                                  + flowNoise(seed + angle * .02, seconds * .45) * .3;
        const QPointF round(std::cos(angle) * radius, std::sin(angle) * radius * .8);
        const QPointF square = corners[side] * (1. - along) +
                               corners[(side + 1) % corners.size()] * along;
        const QPointF point = round * (1. - merge) + square * merge;
        if (sample == 0) silhouette.moveTo(point);
        else silhouette.lineTo(point);
    }
    silhouette.closeSubpath();
    painter.setPen(QPen(state, 1));
    painter.setBrush(QColor(5, 23, 29, 190));
    painter.drawPath(silhouette);

    const auto visual = reportVisuals().value(request);
    const int count = visual.tiers.size();
    if (count == 0 && merge < 1.) {
        const double radius = (1. - merge) * 2.1;
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor("#b4d8dc"));
        painter.drawEllipse(QPointF(), radius, radius);
    }
    painter.setPen(Qt::NoPen);
    for (int index = 0; index < count; ++index) {
        const double angle = seed * turn + index * turn / count + seconds * (index % 2 ? .24 : -.18);
        const QPointF rosette = count == 1 ? QPointF() :
            QPointF(std::cos(angle) * 3.8, std::sin(angle) * 3.2);
        const int columns = count > 6 ? 4 : count > 2 ? 3 : count;
        const int rows = (count + columns - 1) / columns;
        const int row = index / columns;
        const int rowSize = std::min(columns, count - row * columns);
        const QPointF inset((index % columns - (rowSize - 1) * .5) * 2.15,
                             (row - (rows - 1) * .5) * 2.1);
        const QPointF point = rosette * (1. - merge) + inset * merge;
        painter.setBrush(meritColor(visual.tiers[index]));
        const double radius = std::lerp(count > 6 ? 1.3 : 1.7, .72, merge);
        painter.drawEllipse(point,radius,radius);
    }

    const double edges = std::clamp((merge - .3) / .7, 0., 1.);
    if (edges > 0.) {
        painter.setOpacity(painter.opacity() * edges);
        painter.setPen(QPen(state, 1));
        const QPointF hub(0, 1);
        for (const int corner : {1, 3, 5}) {
            painter.drawLine(hub, hub + (corners[corner] - hub) * edges);
        }
    }
    painter.restore();
}
}
