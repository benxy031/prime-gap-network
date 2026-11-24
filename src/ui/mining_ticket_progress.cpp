#include "ui/mining_ticket_progress.h"
#include "ui/mining_attention_status.h"
#include "ui/mining_cycle_progress.h"
#include "ui/audit_status_presentation.h"
#include "ui/gap_flow_motion.h"
#include "ui/position_formats.h"
#include "ui/ticket_flow_evidence.h"
#include "ui/report_visual.h"
#include <QPainter>
#include <QPainterPath>
#include <QRegion>
#include <QImage>
#include <QJsonArray>
#include <QHash>
#include <QTableWidget>
#include <QToolTip>
#include <QHelpEvent>
#include <QRegularExpression>
#include <QStringList>
#include <cmath>

namespace pgn::ui {
namespace {
constexpr int PrimaryWorkHeight=120, ConcurrentWorkRowHeight=24;
const QString chainSyncCaption=QStringLiteral("Waiting for chain synchronization");
QJsonObject legacyAuditScope(const QJsonObject& frame){
 if(frame.value("work_role")!="search"||frame.value("source_stale").toBool()||
    frame.value("phase")=="Waiting for connection"||frame.value("phase")=="Synchronizing")return {};
 const auto cycle=frame.value("audit_service").toObject().value("cycle").toObject();
 const auto request=cycle.value("request_id").toString();
 const auto ticket=frame.value("ticket_id").toString();
 if(cycle.value("schema")!="PGN_MINING_CYCLE_VIEW_V1"||cycle.contains("extra_audit_refs")||
    cycle.value("audits").toArray().size()!=2||request.isEmpty()||ticket.isEmpty()||frame.value("request_id")!=request)return {};
 return {{"request_id",request},{"ticket_id",ticket},{"extra_audit_ids",cycle.value("extra_audit_ids").toArray()}};
}
QJsonObject auditVerdictScope(const QJsonObject& frame){
 if(frame.value("source_stale").toBool()||frame.value("phase")=="Waiting for connection"||
    frame.value("phase")=="Synchronizing")return {};
 const auto legacy=legacyAuditScope(frame);
 const auto service=frame.value("audit_service").toObject();
 const auto cycle=service.value("cycle").toObject();
 if(cycle.isEmpty()&&frame.value("work_role")=="audit"){
  const auto preparation=service.value("preparation").toObject();
  return {{"stream",frame.value("stream_id")},{"device",frame.value("source_device")},
          {"ticket",frame.value("ticket_id")},{"active_audit",preparation.value("active_audit_id")},
          {"preparation_refs",preparation.value("audit_refs")}};
 }
 QJsonArray refs;
 for(const auto& row:cycle.value("audits").toArray())
  refs.append(row.toObject().value("audit_ref"));
 return {{"stream",frame.value("stream_id")},{"device",frame.value("source_device")},
         {"cycle",cycle.value("request_id")},{"ticket",frame.value("ticket_id")},
         {"regular",refs},{"extras",cycle.value("extra_audit_refs")},{"legacy",legacy}};
}
QJsonObject withLegacyAuditRefs(QJsonObject frame,const QJsonObject& snapshot){
 auto service=frame.value("audit_service").toObject();
 auto cycle=service.value("cycle").toObject();
 auto regular=cycle.value("audits").toArray();
 const auto refs=snapshot.value("regular_refs").toArray();
 for(qsizetype i=0;i<std::min(regular.size(),refs.size());++i){
  auto task=regular[i].toObject();
  if(task.value("state")=="committed")task.insert("audit_ref",refs[i]);
  regular[i]=task;
 }
 cycle.insert("audits",regular);
 cycle.insert("extra_audit_refs",snapshot.value("extra_refs"));
 service.insert("cycle",cycle);
 frame.insert("audit_service",service);
 return frame;
}
constexpr qint64 RosetteCollectMs = 2800;
constexpr qint64 RosetteMorphMs = 700;
constexpr qint64 RosetteHoldMs = 650;
constexpr qint64 RosetteExitMs = 1850;
constexpr double RosetteRestOffset = 8.;
constexpr qint64 RosetteValidationMs = RosetteMorphMs + RosetteHoldMs + RosetteExitMs;
QString resourceLabel(const QString& source){
 if(source=="GPU")return source;
 return QString("CPU %1 #%2").arg(source.section('/',1,1),source.section('/',2,2));
}
QString ticketNumber(const QString& value){
 static const QRegularExpression decimal("^[1-9][0-9]{0,79}$");
 return decimal.match(value).hasMatch()?value:QString{};
}
QString ticketNumberFromFrame(const QJsonObject& frame){
 const auto number=ticketNumber(frame.value("ticket_number").toVariant().toString());
 if(!number.isEmpty())return number;
 // Earlier engine hosts carry the lease number in the exact search title.
 const auto title=frame.value("title").toString();
 return title.startsWith("Ticket ")?ticketNumber(title.mid(7)):QString{};
}
double ease(double x){x=std::clamp(x,0.,1.);return x*x*(3-2*x);}
QImage glow(const QColor& color){
 QImage image(40,40,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);
 QPainter painter(&image);QRadialGradient gradient(QPointF(20,20),20);
 auto core=color;core.setAlpha(200);gradient.setColorAt(0,core);core.setAlpha(0);gradient.setColorAt(1,core);
 painter.fillRect(image.rect(),gradient);return image;
}
// Visual rank follows the existing merit classification; it never changes rewards.
double coinRadius(MeritTier tier){
 switch(tier){
 case MeritTier::Gold:return 4.5;
 case MeritTier::Silver:return 3.7;
 case MeritTier::Bronze:return 3.0;
 case MeritTier::NearMiss:return 2.4;
 default:return 1.6;
 }
}
void workMark(QPainter& painter,const QPointF& point,const QString& letter,const QColor& color,bool audited,double opacity=1.){
 painter.save();painter.setOpacity(opacity);painter.translate(point);
 if(audited){painter.setBrush(Qt::NoBrush);painter.setPen(QPen(QColor(149,255,202,185),1.2));painter.drawEllipse(QRectF(-12,-15,24,9));}
 painter.setBrush(QColor("#071b23"));painter.setPen(QPen(color,1.2));
 if(letter=="A")painter.drawEllipse(QPointF(),7,7);else painter.drawRoundedRect(QRectF(-7,-7,14,14),2,2);
  if(letter=="R"){painter.drawLine(QPointF(-4,-2),QPointF(4,-2));painter.drawLine(QPointF(-4,2),QPointF(3,2));painter.drawLine(QPointF(-4,5),QPointF(1,5));}
  else{QFont font("Consolas");font.setPixelSize(10);font.setBold(true);painter.setFont(font);painter.setPen(color);painter.drawText(QRectF(-7,-7,14,14),Qt::AlignCenter,letter);}painter.restore();
}
struct Port {QString id;QRectF card;QPointF point;};
QColor faded(QColor color,int alpha){color.setAlpha(std::clamp(alpha,0,255));return color;}
QColor blend(const QColor& a,const QColor& b,double amount){
 amount=std::clamp(amount,0.,1.);return QColor::fromRgbF(std::lerp(a.redF(),b.redF(),amount),std::lerp(a.greenF(),b.greenF(),amount),std::lerp(a.blueF(),b.blueF(),amount));
}
}
class GapFlowOverlay final : public QWidget {
public:
 explicit GapFlowOverlay(MiningTicketProgress* rail) : QWidget(rail->parentWidget()), rail_(rail) {
  setObjectName("pgn-card-gap-flow");setStyleSheet("background:transparent;border:0;");setAutoFillBackground(false);
  setAttribute(Qt::WA_TransparentForMouseEvents);setAttribute(Qt::WA_NoSystemBackground);setAttribute(Qt::WA_TranslucentBackground);
  for (const auto& color : {QColor("#bde9e4"),QColor("#62cfff"),QColor("#69cf8b"),QColor("#ff6474"),QColor("#cd7f32"),QColor("#c0c0c0"),QColor("#ffd700")})
   glows_.insert(color.rgb(),glow(color));
  show();raise();
 }
 void synchronize() {
  if (!rail_ || !rail_->canvas_ || !parentWidget()) return;
  ports_.clear();int top=rail_->y();
  const QRectF viewport(rail_->canvas_->mapTo(parentWidget(),QPoint()),rail_->canvas_->size());
  for (const auto& value : rail_->canvas_->property("pgn_card_layout").toJsonArray()) {
   const auto card=value.toObject();const auto rect=card.value("rect").toArray();
   if (card.value("visible")!=true || rect.size()!=4) continue;
   QRectF area(rect[0].toDouble(),rect[1].toDouble(),rect[2].toDouble(),rect[3].toDouble());
   area.moveTopLeft(rail_->canvas_->mapTo(parentWidget(),area.topLeft().toPoint()));
   const auto visible=area.intersected(viewport);if (visible.width()<8) continue;
   ports_.append({card.value("id").toString(),area,QPointF(visible.center().x(),area.bottom()+2)});
   top=std::min(top,int(std::floor(area.bottom()))+1);
  }
  const QRect area(0,top,parentWidget()->width(),std::max(1,rail_->y()+rail_->height()-top));
  if (geometry()!=area) {setGeometry(area);raise();}
 }
protected:
 void paintEvent(QPaintEvent*) override {
  if (!rail_) return;
  QElapsedTimer cost;cost.start();QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
  const QPointF offset=rail_->mapTo(parentWidget(),QPoint(0,0))-pos();
  const auto box=rail_->bar().translated(offset);const auto center=box.center();
  QRegion safe(rect());
  for (const auto& port : ports_) safe-=QRegion(port.card.translated(-QPointF(pos())).adjusted(-2,-2,2,2).toAlignedRect());
  safe-=QRegion(QRect(0,int(offset.y()+88),width(),rail_->height()-88));painter.setClipRegion(safe);
  const auto now=rail_->clock_.elapsed();const double seconds=now/1000.;
  const auto source=[&](const QString& id,double seed) {
   for (const auto& port : ports_) if (port.id.compare(id,Qt::CaseInsensitive)==0)
    return port.point-QPointF(pos())+QPointF((seed-.5)*std::min(180.,port.card.width()*.55),0);
   return QPointF(center.x(),std::min(box.top()-18.,offset.y()+3.));
  };
  const auto particle=[&](QPointF at,QColor color,double opacity,double radius) {
   painter.save();painter.setOpacity(std::clamp(opacity,0.,1.));
   painter.drawImage(QRectF(at.x()-radius*4,at.y()-radius*4,radius*8,radius*8),glows_.value(color.rgb()));
   painter.setPen(Qt::NoPen);painter.setBrush(color);painter.drawEllipse(at,radius,radius);painter.restore();
  };
  const QColor incoming=rail_->flow_.audit()?QColor("#62cfff"):QColor("#bde9e4");
  const auto incomingFlow=[&](const QString& producer,double activity,bool audit){
   const auto color=audit?QColor("#62cfff"):QColor("#bde9e4");
   const int population=3+int(activity*13);
   for (int i=0;i<population;++i) {
    const double rhythm=flowSeed(i+37+qHash(producer)%997),travel=seconds*(.18+activity*.27+rhythm*.19)+rhythm*13;
    const auto cycle=std::uint32_t(std::floor(travel));const double u=travel-std::floor(travel);
    const double seed=flowSeed(std::uint32_t(i)*65537+cycle*7919+17);
    const auto from=source(producer,seed);
    const auto at=livingFlowPoint(from,center,u,seed,seconds,30.);
    const auto tail=livingFlowPoint(from,center,std::max(0.,u-.035),seed,seconds,30.);
    painter.setPen(QPen(faded(color,40),.7));painter.drawLine(tail,at);
    particle(at,color,.16+.38*std::sin(u*3.14159265),.9+seed*.8);
    if(i==0&&audit)workMark(painter,at,"A",color,false,.65+.35*std::sin(u*3.14159265));
   }
  };
  if(rail_->flow_.moving()&&!rail_->resourceActivity_.rows().contains(rail_->flow_.producer()))incomingFlow(rail_->flow_.producer(),rail_->flow_.activity(),rail_->flow_.audit());
  QJsonArray resourceFlows;
  for(auto it=rail_->resourceActivity_.rows().cbegin();it!=rail_->resourceActivity_.rows().cend();++it){
   if(it->moving)incomingFlow(it.key(),it->activity,false);
   const auto origin=source(it.key(),.5);resourceFlows.append(QJsonObject{{"device",it.key()},{"active",it->moving},{"x",origin.x()},{"y",origin.y()}});
  }
  rail_->setProperty("pgn_ticket_resource_flows",resourceFlows);
  QVector<const GapFlowEvent*> labels;
  for (const auto& event : rail_->flow_.events()) if (event.tier!=MeritTier::Ordinary && now-event.born>3000) labels.append(&event);
  std::sort(labels.begin(),labels.end(),[this](const auto* a,const auto* b) {
   if (rail_->flow_.record(*a)!=rail_->flow_.record(*b)) return rail_->flow_.record(*a);
   if (a->tier!=b->tier) return int(a->tier)>int(b->tier);
   return a->born>b->born;
  });
  if (labels.size()>3) labels.resize(3);
  const double reach=std::max(32.,box.width()*.34);
  const QPointF receiver(center.x()+reach,center.y());
  const QPointF blackHole(center.x()-reach,center.y()),chainPort(box.right()-25,center.y());
  rail_->setProperty("pgn_ticket_audit_markers",rail_->flow_.moving()&&rail_->flow_.audit()?1:0);
  QJsonArray workTokens,reportHits;
  QHash<QString,QPointF> reportPositions;
  const QPointF queueFront = chainPort + QPointF(-12,0);
  const auto visible = [now](const auto& entry) {
   return (entry.complete || entry.confirmed) &&
          (entry.visualAt<0 || (entry.audited&&now < entry.visualAt + RosetteValidationMs));
  };
  const auto leaving = std::find_if(rail_->landings_.cbegin(),rail_->landings_.cend(),
   [&](const auto& entry) {
    return visible(entry) && entry.audited && entry.confirmed && !entry.block.isEmpty() &&
           entry.visualAt>=0;
   });
  using Landing = std::remove_reference_t<decltype(*leaving)>;
  QVector<const Landing*> queue;
  if(leaving!=rail_->landings_.cend())queue.append(&*leaving);
  for(auto at=rail_->landings_.cbegin();at!=rail_->landings_.cend()&&queue.size()<3;++at)
   if(at!=leaving&&visible(*at))queue.append(&*at);

  const double push = leaving==rail_->landings_.cend() ? 0. :
      ease((now-leaving->visualAt-RosetteMorphMs-RosetteHoldMs)/double(RosetteExitMs));
  for(int slot=0;slot<queue.size();++slot){
   const auto& entry=*queue[slot];
   const bool validating=leaving!=rail_->landings_.cend()&& &entry==&*leaving;
   const QPointF holding=queueFront-QPointF(slot*29,0);
   const double age=validating?double(now-entry.visualAt):0.;
   const double merge=ease(age/double(RosetteMorphMs));
   const double blue=validating?ease((push-.36)/.64):0.;
   const QPointF position=holding+QPointF(push*29,RosetteRestOffset*(1.-merge));
   const auto visualKey=reportVisualKey(entry.request,entry.ticket);
   const double scale=reportVisualScale(visualKey);
   const double opacity=validating?1.-ease((push-.76)/.24):1.;
   const QColor state=validating?blend(QColor("#95ffca"),QColor("#58c8ff"),blue):
                      entry.confirmed?QColor("#69c9e4"):QColor("#b4cec8");
   painter.save();
   painter.setOpacity(opacity);
   if(blue>0.){
    QRadialGradient glow(position,22);
    glow.setColorAt(0,faded(QColor("#3caeff"),int(215*blue)));
    glow.setColorAt(.48,faded(QColor("#3caeff"),int(95*blue)));
    glow.setColorAt(1,QColor(60,174,255,0));
    painter.setPen(Qt::NoPen);
    painter.setBrush(glow);
    painter.drawEllipse(position,22,22);
   }
   drawReportVisual(painter,position,visualKey,state,seconds,scale,merge);
   painter.restore();
   if(opacity>.25){
    const auto hit=position-offset;
    reportHits.append(QJsonObject{{"request",entry.request},{"ticket",entry.ticket},{"confirmed",entry.confirmed},
                                  {"audited",entry.audited},{"x",hit.x()},{"y",hit.y()}});
   }
   reportPositions.insert(entry.ticket,holding+QPointF(0,RosetteRestOffset));
   workTokens.append(QJsonObject{{"ticket_id",entry.ticket},{"request_id",entry.request},
                                 {"kind","range"},{"report_included",entry.confirmed},
                                 {"audited",entry.audited},{"x",position.x()},{"y",position.y()},
                                 {"morph",merge},{"exit_blue",blue},{"opacity",opacity},{"scale",scale}});
  }
  rail_->setProperty("pgn_ticket_work_tokens",workTokens);
  rail_->setProperty("pgn_ticket_report_hits",reportHits);
  const bool alive=rail_->flow_.moving()||rail_->resourceActivity_.moving()||!rail_->flow_.events().isEmpty()||!rail_->flow_.collected().isEmpty()||!rail_->flow_.departing().isEmpty();const double beat=alive?seconds:0;
  // The right-hand collector is a destination, not a declaration that an audit
  // or a monetary reward has passed. The underlying Best table retains results.
  painter.setBrush(Qt::NoBrush);
  for (int i=0;i<3;++i) {
   const double angle=beat*(i%2?-.21:.16)+i*1.7;
   painter.save();painter.translate(receiver);painter.rotate(angle*180/3.14159265);
   painter.setPen(QPen(QColor(77,175,170,42),.8));painter.drawEllipse(QPointF(),29+i*7,7+i*2);painter.restore();
  }
  const auto collectionPointAt=[&](const GapFlowEvent& gap,double time) {
   const double angle=gap.seed*6.2831853+time*(.055+gap.seed*.06);
   const double radius=10+flowSeed(std::uint32_t(gap.seed*65535)+13)*std::min(100.,box.width()*.12);
   return receiver+QPointF(std::cos(angle)*radius,std::sin(angle)*(6+gap.seed*10));
  };
  const auto collectionPoint=[&](const GapFlowEvent& gap){return collectionPointAt(gap,seconds);};
  rail_->hits_.clear();
  for(const auto& node:rail_->flow_.collected()){
   if(now<node.arrives)continue;
   const auto& gap=node.gap;auto at=collectionPoint(gap);
   const bool hover=rail_->hovered_==gap.pair;
   if(hover){const auto mouse=rail_->pointer_+offset;const auto delta=mouse-at;const double length=std::hypot(delta.x(),delta.y());if(length>.1)at+=delta*(std::min(4.,length*.15)/length);}
   const QColor color=meritColor(gap.tier,QColor("#69cf8b"));
   const bool notable=gap.tier!=MeritTier::Ordinary;
   particle(at,color,notable?.9:.45,coinRadius(gap.tier));
   rail_->hits_.append({gap.pair,at-offset});
   if(rail_->flow_.record(gap)){painter.setPen(color);painter.drawText(QRectF(at.x()-5,at.y()-16,12,12),Qt::AlignCenter,"★");}
   if(hover||rail_->pulsed_==gap.pair&&now-rail_->pulsedAt_<850){
    const double pulse=hover?.35:std::clamp((now-rail_->pulsedAt_)/850.,0.,1.);
    painter.setBrush(Qt::NoBrush);painter.setPen(QPen(faded(QColor("#effffb"),int(220*(1-pulse))),1));painter.drawEllipse(at,6+pulse*10,6+pulse*10);
    QFont font("Consolas");font.setPixelSize(11);painter.setFont(font);painter.setPen(color);painter.drawText(QRectF(at.x()-38,at.y()-25,76,14),Qt::AlignCenter,QString::number(gap.gap));
   }
  }
  QJsonArray hitRows;for(const auto& hit:rail_->hits_)hitRows.append(QJsonObject{{"pair",hit.pair},{"x",hit.point.x()},{"y",hit.point.y()}});rail_->setProperty("pgn_ticket_collection_hits",hitRows);
  for(const auto& node:rail_->flow_.departing()){
   const double t=std::clamp((now-node.leaves)/double(node.toSink?1700:node.ticketTransition?RosetteCollectMs:1700),0.,1.);
   const auto destination=reportPositions.constFind(node.gap.ticket());
   if(node.ticketTransition&&!node.toSink&&(destination==reportPositions.cend()||t>=1.))continue;
   const QPointF holding=destination==reportPositions.cend()?queueFront:destination.value();
   const QPointF exit=node.toSink?blackHole+QPointF(std::cos(node.gap.seed*6.28)*24,std::sin(node.gap.seed*6.28)*10):node.ticketTransition?holding:QPointF(receiver.x()+100,center.y()-20);
   const double leaveSeconds=node.leaves/1000.;QPointF origin=collectionPointAt(node.gap,leaveSeconds);
   if(node.flight){const auto& event=*node.flight;const double age=node.flightAge;
    if(age<1.2)origin=livingFlowPoint(source(event.producer,event.seed),center,age/1.2,event.seed,leaveSeconds,28.);
    else if(age<2.2){const double phase=age-1.2,angle=phase*(6+event.seed*5)+event.seed*13;origin=center+QPointF(std::cos(angle)*(10-7*phase),std::sin(angle)*(6-4*phase));}
    else{const bool notable=event.tier!=MeritTier::Ordinary||node.flightFromBest;const QPointF destination=node.flightFromBest?collectionPointAt(node.gap,leaveSeconds):notable?receiver:blackHole+QPointF(std::cos(event.seed*6.28)*24,std::sin(event.seed*6.28)*10);origin=livingFlowPoint(center,destination,(age-2.2)/1.5,event.seed,leaveSeconds,17.);}
   }
   auto at=livingFlowPoint(origin,exit,t,node.gap.seed,seconds,12.);double opacity=node.ticketTransition?1.:std::min(1.,(1-t)*2);
   if(node.toSink&&t>=1.){const double swallowed=std::clamp((now-node.leaves-1700)/500.,0.,1.);const double angle=node.gap.seed*6.28+swallowed*9;at=blackHole+QPointF(std::cos(angle)*24,std::sin(angle)*10)*std::sqrt(1-swallowed);}
   particle(at,meritColor(node.gap.tier,QColor("#69cf8b")),opacity,coinRadius(node.gap.tier)*(1-t*.1)*(node.ticketTransition&&!node.toSink?opacity:1.));
  }
  double reaction=0;QColor resonance=alive?(rail_->flow_.audit()?QColor("#62cfff"):QColor("#69cf8b")):QColor("#426b70");
  const GapFlowEvent* measured=nullptr;
  for (const auto& event : rail_->flow_.events()) {
   const double age=(now-event.born)/1000.;if (age<0) continue;
   const auto* retained=rail_->flow_.collected(event);const auto color=meritColor(event.tier,retained?QColor("#69cf8b"):incoming);const bool notable=event.tier!=MeritTier::Ordinary||retained;
   const double radius=1.9+.4*std::sin(seconds*2.7+event.seed*17);
   if (age<1.2) {
    particle(livingFlowPoint(source(event.producer,event.seed),center,age/1.2,event.seed,seconds,28.),incoming,.9,radius);
   } else if (age<2.2) {
    const double t=age-1.2,pulse=std::sin(t*3.14159265);
    if (pulse>reaction) {reaction=pulse;resonance=blend(incoming,color,ease(t));measured=&event;}
    const double angle=t*(6+event.seed*5)+event.seed*13;
    particle(center+QPointF(std::cos(angle)*(10-7*t),std::sin(angle)*(6-4*t)),blend(incoming,color,ease(t)),1.,std::lerp(2.5,coinRadius(event.tier),ease(t)));
   } else {
    const double transit=(age-2.2)/1.5;
    const int slot=labels.indexOf(&event);
    const QPointF destination=retained?collectionPoint(retained->gap):notable?receiver:blackHole+QPointF(std::cos(event.seed*6.28)*24,std::sin(event.seed*6.28)*10);
    QPointF at=livingFlowPoint(center,destination,transit,event.seed,seconds,17.);
    double fade=1.;
    if (transit>=1. && retained) continue;
    if (transit>=1.) {
     const double settling=age-3.7;
     if (notable) {
      const double lane=slot>=0?slot:std::fmod(event.seed*19,3.);
      const double angle=event.seed*6.28+settling*(.7+event.seed*.8);
      const double orbit=(13+lane*17)*std::clamp((event.lifetime()/1000.-age)/1.7,0.,1.);
      at=receiver+QPointF(std::cos(angle)*orbit,std::sin(angle)*(4+lane*3));
      fade=std::clamp((event.lifetime()/1000.-age)/.8,0.,1.);
     } else {
      const double absorption=std::clamp(settling/.5,0.,1.);const double angle=event.seed*6.28+absorption*9.;
      at=blackHole+QPointF(std::cos(angle)*24,std::sin(angle)*10)*std::sqrt(1-absorption);fade=1.;
     }
    }
    if (transit<.65) {
     const double impulse=std::clamp(transit/.65,0.,1.);painter.setBrush(Qt::NoBrush);
     painter.setPen(QPen(faded(color,int((1-impulse)*65)),.8));painter.drawEllipse(center,38+impulse*42,8+impulse*8);
    }
    particle(at,color,fade,coinRadius(event.tier));
    if (notable && transit>=.7 && slot>=0) {
     painter.setOpacity(fade);painter.setPen(color);QFont font("Consolas");font.setPixelSize(10);painter.setFont(font);
     // Stable label lanes remain legible while the entities themselves orbit.
     const double labelX=receiver.x()+(slot-1)*76;
     painter.drawText(QRectF(labelX-36,box.bottom()-6,72,13),Qt::AlignCenter,(rail_->flow_.record(event)?QString("★ "):QString{})+QString::number(event.gap));
     painter.setOpacity(1.);
    }
   }
  }
  // The ordinary stream winds into a quiet visual sink. No data operation is
  // attached to this black hole: reports and counts remain source-owned.
  painter.save();painter.translate(blackHole);painter.rotate(-14);painter.setBrush(Qt::NoBrush);
  QConicalGradient accretion(QPointF(),beat*12);accretion.setColorAt(0,QColor(78,163,183,25));accretion.setColorAt(.35,QColor(153,219,223,190));accretion.setColorAt(.65,QColor(59,116,153,80));accretion.setColorAt(1,QColor(78,163,183,25));
  painter.setPen(QPen(QBrush(accretion),1.7));painter.drawEllipse(QPointF(),27,9);painter.setPen(QPen(QColor(99,168,181,80),.8));painter.drawEllipse(QPointF(),13,13);
  painter.setBrush(QColor("#000407"));painter.setPen(QPen(QColor(98,182,199,145),1.));painter.drawEllipse(QPointF(),8,8);painter.restore();
  // Prime anchors deform a lens as the candidate is measured. Interference is
  // decorative; exact progress, merit, record status and audits stay separate.
  const double half=52+reaction*11;
  QPainterPath membrane;membrane.moveTo(center-QPointF(half,0));
  for (int side : {-1,1}) for (int i=0;i<=40;++i) {
   const double u=side<0?double(i)/40:1.-double(i)/40;
   const double envelope=std::sin(u*3.141592653589793);
   const double ripple=(std::sin(u*15+beat*1.8)+flowNoise(.27+u,beat*.6))*(.8+reaction*2)*envelope;
   membrane.lineTo(center+QPointF(-half+2*half*u,side*(17+reaction*3)*std::pow(envelope,.65)+ripple));
  }
  membrane.closeSubpath();painter.setPen(QPen(faded(resonance,30),6));painter.setBrush(Qt::NoBrush);painter.drawPath(membrane);
  QLinearGradient skin(center-QPointF(half,0),center+QPointF(half,0));
  skin.setColorAt(0,faded(resonance,25));skin.setColorAt(.5,QColor(3,22,29,245));skin.setColorAt(1,faded(resonance,25));
  painter.setBrush(skin);painter.setPen(QPen(faded(resonance,170),1));painter.drawPath(membrane);
  painter.save();painter.setClipPath(membrane,Qt::IntersectClip);painter.setBrush(Qt::NoBrush);
  for (int i=0;i<3;++i) {const double radius=9+std::fmod(beat*8+i*15,46.);painter.setPen(QPen(faded(resonance,int(48*(1-radius/60))),.7));painter.drawEllipse(center,radius,radius*.5);}
  painter.restore();
  const double separation=24+reaction*9;painter.setPen(QPen(faded(resonance,135),1));painter.drawLine(center-QPointF(separation,0),center+QPointF(separation,0));
  for (int side : {-1,1}) {
   const auto anchor=center+QPointF(side*separation,0);painter.setBrush(QColor("#071c24"));painter.setPen(QPen(resonance,1.2));painter.drawEllipse(anchor,4,4);
   painter.setPen(QPen(faded(resonance,60),.7));painter.setBrush(Qt::NoBrush);painter.drawEllipse(anchor,7+reaction*2,7+reaction*2);
  }
  QFont font("Consolas");font.setPixelSize(10);painter.setFont(font);painter.setPen(resonance);
  painter.drawText(QRectF(center.x()-separation-8,center.y()-20,16,12),Qt::AlignCenter,"p");
  painter.drawText(QRectF(center.x()+separation-8,center.y()-20,16,12),Qt::AlignCenter,"q");
  painter.drawText(QRectF(center.x()-35,center.y()+4,70,12),Qt::AlignCenter,measured?QString("Δ %1").arg(measured->gap):rail_->flow_.audit()?QString("AUDIT"):QString("Δ / ln p"));
  const auto elapsed=cost.nsecsElapsed();rail_->setProperty("pgn_ticket_animation_paint_ns",elapsed);
  rail_->setProperty("pgn_ticket_animation_paint_max_ns",std::max(rail_->property("pgn_ticket_animation_paint_max_ns").toLongLong(),elapsed));
 }
private:
 QPointer<MiningTicketProgress> rail_;QVector<Port> ports_;QHash<QRgb,QImage> glows_;
};
MiningTicketProgress::MiningTicketProgress(QWidget* parent):QWidget(parent){
 setObjectName("pgn-ticket-progress-rail");setFixedHeight(PrimaryWorkHeight);setMouseTracking(true);clock_.start();timer_.setInterval(33);
 connect(&timer_,&QTimer::timeout,this,[this]{if(!isVisible())return;const bool wasMoving=flow_.moving();flow_.expire(clock_.elapsed());pollAuditVerdicts();pollLandings();if(results_)flow_.setVisibleBest(results_->property("pgn_visible_best").toJsonObject(),clock_.elapsed());setProperty("pgn_ticket_collection_size",flow_.collected().size());setProperty("pgn_ticket_collection_departing",flow_.departing().size());
  const bool resourceChanged=resourceActivity_.expire(clock_.elapsed());
  if(resourceChanged||resourceActivity_.moving()||wasMoving!=flow_.moving()||flow_.moving()||workMarksActive(clock_.elapsed())||!flow_.events().isEmpty()||!flow_.collected().isEmpty()||!flow_.departing().isEmpty()||frame_.value("phase")=="Starting"||frame_.value("phase")=="Stopping"){
   update();ensureOverlay();if(overlay_){overlay_->synchronize();overlay_->update();}
  }});timer_.start();
}
MiningTicketProgress::~MiningTicketProgress(){delete overlay_.data();}
QRectF MiningTicketProgress::bar()const{return {16,28,double(std::max(1,width()-32)),50};}
void MiningTicketProgress::ensureOverlay(){if(!overlay_&&canvas_&&parentWidget())overlay_=new GapFlowOverlay(this);}
void MiningTicketProgress::bindSourceCanvas(QWidget* canvas){canvas_=canvas;if(canvas_)canvas_->setProperty("pgn_card_geometry_enabled",true);ensureOverlay();}
void MiningTicketProgress::bindResultsView(QWidget* view){results_=view;}
void MiningTicketProgress::setRecordEligibility(const QJsonArray& pairs){flow_.setEligibility(pairs);if(overlay_)overlay_->update();}
void MiningTicketProgress::presentResourceActivity(const QString& device,const QJsonObject& status,bool connected){
 const auto previous=resourceActivity_.rows().value(device);
 resourceActivity_.observe(device,status,connected,clock_.elapsed());
 const auto current=resourceActivity_.rows().value(device);
 if(workViews_.contains(device)&&(current.connected!=previous.connected||current.enabled!=previous.enabled))update();
 ensureOverlay();if(overlay_){overlay_->synchronize();overlay_->update();}
}
bool MiningTicketProgress::selectWorkSource(const QString& source){
 if(source!=workSource_&&!workViews_.contains(source))return false;
 if(source!=workSource_){auto& old=workViews_[workSource_];old.flow=flow_;old.cycle=cycleHistory_;workSource_=source;++legacyAuditGeneration_;legacyAuditWatch_={};legacyAuditScope_={};legacyAuditSnapshot_={};auditVerdicts_={};const auto view=workViews_.value(source);flow_=view.flow;cycleHistory_=view.cycle;frame_={};}
 auto value=workViews_.value(source).frame;
 if(value.isEmpty())value={{"schema","PGN_HORIZON_MINING_VIEW_V1"},{"phase","Waiting for a ticket"},{"source_device",source},{"source_stale",true},{"scopes",QJsonObject{}}};
 updateWorkHeight();applyFrame(value);if(onWorkSelected)onWorkSelected(value);
 setProperty("pgn_selected_work_source",source);return true;
}
void MiningTicketProgress::present(const QJsonObject& frame){
 if(!coherentMiningCycleFrame(frame))return;
 const auto device=frame.value("source_device").toString();const auto source=device.startsWith("CPU/")?device:QString("GPU");
 if(source!="GPU"&&!QRegularExpression("^CPU/[A-Za-z0-9_.-]{1,100}/[0-9]+$").match(source).hasMatch())return;
 if(source!="GPU"&&!workViews_.contains(source)&&workViews_.size()-workViews_.contains("GPU")>=8)return;
 workViews_[source].frame=frame;
 updateWorkHeight();
 if(source!=workSource_){workViews_[source].cycle.observe(frame);update();return;}
 applyFrame(frame);if(onWorkSelected)onWorkSelected(frame);
 setProperty("pgn_selected_work_source",source);
}
void MiningTicketProgress::applyFrame(const QJsonObject& frame){
 if(auditVerdictScope(frame_)!=auditVerdictScope(frame)){
  ++legacyAuditGeneration_;
  legacyAuditSnapshot_={};auditVerdicts_={};
  nextAuditLookupAt_=0;
 }
 const auto remember=[this](const QJsonObject& packet){
  if(packet.value("work_role")=="audit")return;
  const auto ticket=packet.value("ticket_id").toString(),request=packet.value("request_id").toString();
  if(ticket.isEmpty()||!QRegularExpression("^[0-9a-f]{64}$").match(request).hasMatch())return;
  QVector<MeritTier> signature;
  for(const auto& node:flow_.collected())
   if(node.gap.ticket()==ticket)signature.append(node.gap.tier);
  const auto number=ticketNumberFromFrame(packet);
  rememberReportVisual(reportVisualKey(request,ticket),std::move(signature),number);
  for(auto& entry:landings_)if(entry.ticket==ticket&&entry.request==request){if(!entry.complete&&packet.value("phase")=="Report ready"){entry.complete=true;entry.born=clock_.elapsed();}return;}
  Landing entry;entry.ticket=ticket;entry.request=request;entry.complete=packet.value("phase")=="Report ready";entry.born=clock_.elapsed();landings_.append(entry);
  while(landings_.size()>32)landings_.removeFirst();
 };
 if(frame_.value("ticket_id")!=frame.value("ticket_id"))remember(frame_);
 if(frame.value("phase")=="Report ready")remember(frame);
 cycleHistory_.observe(frame);
 const auto producer=frame.value("source_device").toString();
 frame_=frame;flow_.observe(frame,!producer.isEmpty()?producer:canvas_?canvas_->property("pgn_managed_source_device").toString():QString{},clock_.elapsed());
 // A phase is evidence of a failure, not evidence that a retry or durable save
 // succeeded. Keep the complete current reason reachable when the row elides.
 const bool attention=frame.value("phase")=="Needs attention";
 const auto reason=frame.value("message").toString().trimmed().left(4096);
 const auto auditCompletion=auditCompletionStatus(frame);
 QStringList auditDetails;
 if(!auditCompletion.isEmpty())auditDetails.append(auditCompletion);
 const auto cycle=frame.value("audit_service").toObject().value("cycle").toObject();
 const auto regular=cycle.value("audits").toArray();
 if(!frame.value("source_stale").toBool()&&cycle.value("schema")=="PGN_MINING_CYCLE_VIEW_V1"&&regular.size()==2)
  for(int slot=0;slot<2;++slot)if(regular[slot].toObject().value("state")=="unavailable")
   auditDetails.append(QString("Audit %1/2 skipped because no eligible independent work was available.").arg(slot+1));
 setToolTip(attention?QString("<qt><b>Mining stopped</b><br>%1</qt>").arg(
     (reason.isEmpty()?QString("Open Diagnostics for the failure details"):reason).toHtmlEscaped().replace("\n","<br>")):
     auditDetails.isEmpty()?QString{}:QString("<qt>%1</qt>").arg(auditDetails.join('\n').toHtmlEscaped().replace("\n","<br>")));
 setAccessibleDescription(attention?miningAttentionStatus(reason):
     frame.value("phase")=="Synchronizing"?chainSyncCaption:auditDetails.join(' '));
 setProperty("pgn_ticket_flow_received",QVariant::fromValue(flow_.received()));setProperty("pgn_ticket_flow_coalesced",QVariant::fromValue(flow_.coalesced()));
 ensureOverlay();update();if(overlay_){overlay_->synchronize();overlay_->update();}
}
void MiningTicketProgress::updateWorkHeight(){
 const auto rows=workViews_.size()-workViews_.contains(workSource_);
 const int wanted=PrimaryWorkHeight+ConcurrentWorkRowHeight*rows;
 if(height()!=wanted)setFixedHeight(wanted);
}

void MiningTicketProgress::paintConcurrentWork(QPainter& painter){
 QFont font("Segoe UI");font.setPixelSize(12);painter.setFont(font);
 const auto metrics=painter.fontMetrics();
 QJsonArray rows;
 int top=PrimaryWorkHeight;
 const int available=std::max(1,width()-32);
 for(auto it=workViews_.constBegin();it!=workViews_.constEnd();++it){
  const auto& source=it.key();
  if(source==workSource_)continue;
  const auto& frame=it->frame;
  const bool audit=frame.value("work_role")=="audit";
  const auto resource=resourceActivity_.rows().constFind(source);
  const bool disconnected=resource!=resourceActivity_.rows().constEnd()&&!resource->connected;
  auto phase=frame.value("phase").toString("Waiting for a ticket");
  if(disconnected)phase="Disconnected";
  else if(phase=="Searching"){
   if(resource!=resourceActivity_.rows().constEnd()&&resource->enabled==false)phase="Paused";
   else if(frame.value("source_stale").toBool())phase="Last reported";
   else if(audit)phase="Auditing";
  }
  const auto title=frame.value("title").toString(audit?"Network audit":"Network ticket");
  bool doneOk=false,totalOk=false;
  const auto done=frame.value("positions").toString().toDouble(&doneOk);
  const auto total=frame.value("positions_total").toString().toDouble(&totalOk);
  const bool measured=doneOk&&totalOk&&std::isfinite(done)&&std::isfinite(total)&&total>0&&done>=0&&done<=total;
  const double fraction=measured?done/total:0.;
  const auto auditCompletion=disconnected?QString{}:auditCompletionStatus(frame);
  const QString status=phase=="Synchronizing"?chainSyncCaption:
      !auditCompletion.isEmpty()?auditCompletion:
      measured?phase+QString(" · %1%").arg(100*fraction,0,'f',2):phase;
  const auto color=disconnected?QColor("#a7b8bf"):QColor("#8cb9ec");
  painter.fillRect(QRect(16,top,available,ConcurrentWorkRowHeight-2),QColor("#0b222b"));
  const int textWidth=std::max(1,available-16);
  const int statusWidth=std::min(textWidth/2,metrics.horizontalAdvance(status)+2);
  const int labelWidth=std::max(1,textWidth-statusWidth-9);
  painter.setPen(QColor("#def2f3"));
  painter.drawText(QRect(24,top,labelWidth,19),Qt::AlignVCenter,metrics.elidedText(resourceLabel(source)+" · "+title,Qt::ElideRight,labelWidth));
  painter.setPen(color);
  painter.drawText(QRect(24+textWidth-statusWidth,top,statusWidth,19),Qt::AlignRight|Qt::AlignVCenter,metrics.elidedText(status,Qt::ElideRight,statusWidth));
  if(measured){
   painter.fillRect(QRectF(24,top+19,textWidth,2),QColor("#284655"));
   painter.fillRect(QRectF(24,top+19,textWidth*fraction,2),color);
  }
  rows.append(QJsonObject{{"source",source},{"title",title},{"phase",phase},
                         {"fraction",measured?QJsonValue(fraction):QJsonValue()}});
  top+=ConcurrentWorkRowHeight;
 }
 setProperty("pgn_ticket_concurrent_work",rows);
}

void MiningTicketProgress::paintEvent(QPaintEvent*){
 QPainter painter(this);painter.setRenderHint(QPainter::Antialiasing);
 const auto phase=frame_.value("phase").toString("Waiting for a ticket");const bool active=phase=="Searching",transition=phase=="Starting"||phase=="Stopping",storageWait=phase=="Waiting for storage";
 const auto role=frame_.value("work_role").toString();
 const bool audit=role=="audit"||(role.isEmpty()&&frame_.value("audit_service").toObject().value("active_role")=="audit");
 QString displayPhase=phase;
 if(audit){
  if(active)displayPhase="Auditing";
  else if(phase=="Starting")displayPhase="Preparing audit";
  else if(phase=="Report ready")displayPhase="Audit computed";
  else if(phase=="Paused")displayPhase="Audit paused";
 }
 const auto accent=transition?QColor("#ffbd69"):QColor("#69efc5");
 const double total=frame_.value("positions_total").toString().toDouble(),done=frame_.value("positions").toString().toDouble();
 const auto legacyBound=legacyAuditScope_==legacyAuditScope(frame_)&&!legacyAuditScope_.isEmpty()&&
      !legacyAuditSnapshot_.isEmpty();
 const auto frameForCycle=legacyBound?withLegacyAuditRefs(frame_,legacyAuditSnapshot_):frame_;
 const auto cycle=miningCycleProgress(frameForCycle,cycleHistory_.audits(),cycleHistory_.search());
 const auto fraction=total>0?std::optional<double>(std::clamp(done/total,0.,1.)):std::nullopt;
 const auto trackFraction=cycle.known?cycle.markerFraction:fraction;
 if(cycle.known&&(active||phase=="Auditing"))displayPhase=cycle.phase;
 const auto field=bar();const double seconds=flow_.moving()?clock_.elapsed()/1000.:0.;
 double pressure=0.;
 for(const auto& event:flow_.events()){
  const double age=(clock_.elapsed()-event.born)/1000.;
  if(age>=1.2&&age<=2.2)pressure=std::max(pressure,std::sin((age-1.2)*3.14159265));
 }
 const auto contour=[&](int side,double u){
  const double x=std::lerp(field.left(),field.right(),u);
  const double center=std::exp(-std::pow((u-.5)/.09,2.));
  const double collector=std::exp(-std::pow((u-.85)/.12,2.));
  const double envelope=std::pow(std::sin(u*3.14159265),.55);
  const double breath=(std::sin(u*16+seconds*.9)+std::sin(u*29-seconds*.7))*(1.+pressure*1.5);
  return QPointF(x,field.center().y()+side*(5+center*(11+pressure*3)+collector*8)*envelope+breath*envelope);
 };
 QPainterPath silhouette;
 for(int side:{-1,1})for(int i=0;i<=128;++i){const double u=side<0?i/128.:1.-i/128.;const auto point=contour(side,u);if(side==-1&&i==0)silhouette.moveTo(point);else silhouette.lineTo(point);}
 silhouette.closeSubpath();
 QLinearGradient mist(field.left(),field.center().y(),field.right(),field.center().y());
 mist.setColorAt(0,QColor(5,25,29,25));mist.setColorAt(.48,QColor(7,53,58,140));mist.setColorAt(.85,QColor(9,47,51,115));mist.setColorAt(1,QColor(5,25,29,25));
 painter.setBrush(Qt::NoBrush);painter.setPen(QPen(faded(accent,12),8));painter.drawPath(silhouette);
 painter.setBrush(mist);painter.setPen(QPen(faded(accent,90),.9));painter.drawPath(silhouette);
 // The ticket's exact fraction has its own quiet track. The deforming field
 // above is event presentation and can never advance this measurement.
 const double trackY=84.;const QPointF trackStart(field.left(),trackY),trackEnd(field.right(),trackY);
 painter.setPen(QPen(QColor("#17383e"),1));painter.drawLine(trackStart,trackEnd);
 if(cycle.known){double offset=0;QJsonArray auditColors;
 const auto verdicts=frame_.value("source_stale").toBool()?QJsonArray{}:auditVerdicts_;
 const auto workPhase=frame_.value("phase");
 const bool working=(workPhase=="Searching"||workPhase=="Auditing")&&!frame_.value("source_stale").toBool();
 for(const auto& slice:cycle.slices){
  const auto state=confirmedProgressAuditState(verdicts,slice.auditRef);
  QColor color("#658a96");
  if(slice.role=="search")color=QColor("#69efc5");
  else if(!state.isEmpty()&&state!="assigned")color=auditStatusPresentation(state).color;
  else if(slice.workerReportedCommit)color=auditStatusPresentation("committed").color;
  else if(slice.active&&working)color=ticketStagePresentation("assigned").color;
  else if(state=="assigned")color=auditStatusPresentation(state).color;
  if(slice.role!="search")auditColors.append(color.name());const double left=std::lerp(field.left(),field.right(),offset),right=std::lerp(field.left(),field.right(),offset+slice.weight);const QRectF background(left,trackY-3,std::max(0.,right-left-2),6);if(slice.pendingAssignment){painter.fillRect(background,faded(color,20));painter.setPen(QPen(faded(color,100),.8,Qt::DashLine));painter.drawRect(background);}else if(slice.skipped){painter.fillRect(background,QBrush(faded(color,160),Qt::BDiagPattern));painter.setPen(QPen(faded(color,180),.8));painter.drawRect(background);}else{painter.fillRect(background,faded(color,45));if(slice.fraction>0)painter.fillRect(QRectF(left,trackY-3,background.width()*slice.fraction,6),faded(color,250));}offset+=slice.weight;}setProperty("pgn_ticket_audit_colors",auditColors);setProperty("pgn_ticket_cycle_fraction",cycle.preparing?QVariant{}:QVariant(cycle.fraction));setProperty("pgn_ticket_cycle_eta_seconds",(active||phase=="Auditing")&&cycle.cycleEta?QVariant(*cycle.cycleEta):QVariant{});}else {setProperty("pgn_ticket_cycle_fraction",QVariant{});setProperty("pgn_ticket_audit_colors",QVariant{});}
 if(trackFraction&&!cycle.preparing){const QPointF measured(std::lerp(trackStart.x(),trackEnd.x(),*trackFraction),trackY);
  if(!cycle.known){painter.setPen(QPen(faded(accent,70),4));painter.drawLine(trackStart,measured);painter.setPen(QPen(accent,1.2));painter.drawLine(trackStart,measured);}
  painter.setPen(Qt::NoPen);painter.setBrush(accent);painter.drawEllipse(measured,2.,2.);
 }
 QFont titleFont("Segoe UI");titleFont.setPixelSize(13);titleFont.setWeight(QFont::DemiBold);painter.setFont(titleFont);
 QString title=frame_.contains("bits")?(cycle.known?cycle.title:frame_.value("title").toString(audit?"Network audit":"Network ticket"))+QString(" · %1 bits").arg(frame_.value("bits").toInt()):audit?QString("Network audit"):QString("Network work · awaiting assignment");
 if(height()>PrimaryWorkHeight)title=resourceLabel(workSource_)+" · "+title;
 const double rate=frame_.value("eta_rate").toDouble();const auto eta=active&&rate>0&&std::isfinite(rate)&&total>0?std::optional<double>(std::max(0.,total-done)/rate):std::nullopt;
 const bool attention=frame_.value("phase")=="Needs attention";
 const auto auditCompletion=auditCompletionStatus(frame_);
 const QString status=attention?miningAttentionStatus(frame_.value("message").toString()):
      phase=="Synchronizing"?chainSyncCaption:
      !auditCompletion.isEmpty()?auditCompletion:displayPhase+(fraction?QString(" · ≈ %1%").arg(100* *fraction,0,'f',2):QString{});
 const QString positions=frame_.contains("positions_total")?(audit?QString("Complete audit · "):QString{})+positionFormats(frame_.value("positions_total").toString()).inlineText()+" positions":QString{};
 QString remaining;
 if(eta&&std::isfinite(*eta)&&*eta<365.*86400){const auto value=qint64(std::ceil(*eta));remaining=QString("ETA ≈ %1:%2:%3").arg(value/3600,2,10,QChar('0')).arg(value/60%60,2,10,QChar('0')).arg(value%60,2,10,QChar('0'));}
 if(cycle.known&&!cycle.preparing&&(active||phase=="Auditing")){const auto format=[](std::optional<double> seconds){if(!seconds||!std::isfinite(*seconds)||*seconds>=365.*86400)return QString("—");const auto n=qint64(std::ceil(*seconds));return QString("≈ %1:%2:%3").arg(n/3600,2,10,QChar('0')).arg(n/60%60,2,10,QChar('0')).arg(n%60,2,10,QChar('0'));};remaining="Cycle ETA "+format(cycle.cycleEta)+" · "+(audit?QString("audit "):QString("search "))+format(cycle.taskEta);}
 if(attention)remaining.clear();
 // One measured row keeps the tables close to the live track. Reserve the
 // changing state/ETA first, then elide metadata instead of resizing fonts.
 constexpr double rowY=94.,rowHeight=22.,gap=16.;
 const int contentLeft=16,available=std::max(1,width()-contentLeft-18);
 QFont metricFont("Consolas");metricFont.setPixelSize(12);
 const QFontMetrics metricMetrics(metricFont);
 const QString activity=status+(remaining.isEmpty()?QString{}:QString("  ·  ")+remaining);
 const int activityWidth=std::min(available*3/5,metricMetrics.horizontalAdvance(activity)+2);
 const int titleWidth=std::min(std::max(1,available-activityWidth-int(gap)),painter.fontMetrics().horizontalAdvance(title)+2);
 painter.setPen(QColor("#e1f6f0"));painter.drawText(QRectF(contentLeft,rowY,titleWidth,rowHeight),Qt::AlignLeft|Qt::AlignVCenter,painter.fontMetrics().elidedText(title,Qt::ElideRight,titleWidth));
 QFont detailFont("Segoe UI");detailFont.setPixelSize(12);painter.setFont(detailFont);painter.setPen(QColor("#9cbdbb"));
 const int detailLeft=contentLeft+titleWidth+int(gap),detailWidth=std::max(0,width()-18-activityWidth-int(gap)-detailLeft);
 if(detailWidth>0)painter.drawText(QRectF(detailLeft,rowY,detailWidth,rowHeight),Qt::AlignLeft|Qt::AlignVCenter,painter.fontMetrics().elidedText(positions,Qt::ElideRight,detailWidth));
 painter.setFont(metricFont);painter.setPen(accent);painter.drawText(QRectF(width()-18-activityWidth,rowY,activityWidth,rowHeight),Qt::AlignRight|Qt::AlignVCenter,metricMetrics.elidedText(activity,Qt::ElideRight,activityWidth));
 setProperty("pgn_ticket_display_phase",attention?status:displayPhase);setProperty("pgn_ticket_progress_scope",cycle.preparing?"pre_ticket_audit":cycle.known?"ticket_cycle":"task");setProperty("pgn_ticket_header_rows",1);setProperty("pgn_ticket_header_height",height());
 paintConcurrentWork(painter);
 setProperty("pgn_ticket_eta_seconds",eta?QVariant(*eta):QVariant{});setProperty("pgn_ticket_progress_fraction",fraction?QVariant(*fraction):QVariant{});if(storageWait)setProperty("pgn_ticket_cycle_eta_seconds",QVariant{});
 setProperty("pgn_ticket_work_presentation",QJsonObject{{"role",audit?"audit":role},{"phase",displayPhase},{"title",title},{"status",status},{"quantity",positions},{"remaining",remaining}});
 setProperty("pgn_ticket_animation_active",flow_.moving());setProperty("pgn_ticket_animation_phase",flow_.moving()?QVariant(std::fmod(clock_.elapsed()/1000.,1.)):QVariant{});
 if(overlay_)overlay_->synchronize();
}
void MiningTicketProgress::bindAuditLookup(AuditLookup lookup) {
 auditLookup_=std::move(lookup);
 ++auditBinding_;
 auditLookupBusy_=false;
 auditRequests_={};auditVerdicts_={};legacyAuditScope_={};legacyAuditSnapshot_={};legacyAuditWatch_={};
 nextAuditLookupAt_=nextAuditRequestScanAt_=0;
 update();
}

void MiningTicketProgress::bindLegacyAuditLookup(LegacyAuditLookup lookup){
 legacyAuditLookup_=std::move(lookup);
 bindAuditLookup(auditLookup_);
}

void MiningTicketProgress::pollAuditVerdicts() {
 const auto now=clock_.elapsed();
 if((!auditLookup_&&!legacyAuditLookup_)||now<nextAuditRequestScanAt_)return;
 nextAuditRequestScanAt_=now+250;
 const auto cycle=miningCycleProgress(frame_,cycleHistory_.audits(),cycleHistory_.search());
 QJsonArray requests;
 for(const auto& slice:cycle.slices){
  if(!validProgressAuditRef(slice.auditRef))continue;
  const auto request=slice.auditRef.value("request");
  if(!requests.contains(request))requests.append(request);
 }
 const auto fallback=requests.isEmpty()&&legacyAuditLookup_?legacyAuditScope(frame_):QJsonObject{};
 if(requests!=auditRequests_||legacyAuditScope_!=fallback){
  if(legacyAuditScope_!=fallback){
   nextAuditLookupAt_=now;
   QJsonArray retained;
   if(fallback.value("request_id")==legacyAuditScope_.value("request_id")&&
      fallback.value("ticket_id")==legacyAuditScope_.value("ticket_id")){
    const auto wanted=fallback.value("extra_audit_ids").toArray();
    for(const auto& ref:legacyAuditWatch_)if(wanted.contains(ref.toObject().value("audit_id")))retained.append(ref);
   }
   legacyAuditWatch_=retained;
  }
  auditRequests_=requests;legacyAuditScope_=fallback;
  auditVerdicts_={};legacyAuditSnapshot_={};update();
 }
 if((requests.isEmpty()&&fallback.isEmpty())||auditLookupBusy_||now<nextAuditLookupAt_)return;
 auditLookupBusy_=true;
 const auto binding=auditBinding_;
 QPointer<MiningTicketProgress> weak(this);
 const auto generation=legacyAuditGeneration_;
 const auto done=[weak,binding,requests,fallback,generation](QJsonObject value,QString error){
  if(!weak||weak->auditBinding_!=binding)return;
  weak->auditLookupBusy_=false;
  const auto observed=weak->clock_.elapsed();
  if(requests!=weak->auditRequests_||fallback!=weak->legacyAuditScope_||
     generation!=weak->legacyAuditGeneration_)return;
  if(!error.isEmpty()){
   // Keep the last matching verdict during an explicit transport timeout.
   // Branch, epoch and identity conflicts invalidate it immediately.
   if(value.value("audit_read_timeout")!=true){
    weak->auditVerdicts_={};weak->legacyAuditSnapshot_={};weak->update();
   }
   weak->nextAuditLookupAt_=observed+1500;
   return;
  }
  weak->nextAuditLookupAt_=observed+(fallback.isEmpty()?5500:9500);
  // Replace the whole snapshot. Rows from different active-branch anchors
  // must never be combined into one set of completed audit indicators.
  weak->auditVerdicts_=value.value("audits").toArray();
  weak->legacyAuditSnapshot_=fallback.isEmpty()?QJsonObject{}:value;
  if(!weak->legacyAuditSnapshot_.isEmpty()){
   const auto ids=fallback.value("extra_audit_ids").toArray();
   QMap<QString,QJsonObject> locators;
   auto refs=weak->legacyAuditWatch_;
   for(const auto& ref:value.value("extra_refs").toArray())refs.append(ref);
   for(const auto& ref:refs){
    const auto entry=ref.toObject();
    const auto id=entry.value("audit_id").toString();
    if(!id.isEmpty()&&ids.contains(id))locators.insert(id,{{"audit_id",id},{"request",entry.value("request")},
        {"slot",entry.value("slot")},{"attempt",entry.value("attempt")}});
   }
   weak->legacyAuditWatch_={};
   for(const auto& locator:locators)weak->legacyAuditWatch_.append(locator);
  }
  weak->update();
 };
 if(fallback.isEmpty()&&auditLookup_)auditLookup_(requests,done);
 else if(!fallback.isEmpty()){
  auto watch=fallback;
  if(!legacyAuditWatch_.isEmpty())watch.insert("remembered",legacyAuditWatch_);
  legacyAuditLookup_(watch,done);
 }
 else auditLookupBusy_=false;
}

void MiningTicketProgress::bindTicketLookup(TicketLookup lookup){
 lookup_=std::move(lookup);++lookupGeneration_;++activeLookup_;lookupBusy_=false;nextLookupAt_=0;
 for(auto& entry:landings_){entry.checking=false;entry.nextCheck=0;entry.confirmed=entry.audited=false;entry.approvedAt=entry.visualAt=-1;entry.animatedBlock.clear();flow_.revokeReport(entry.ticket);}
 setProperty("pgn_ticket_landing_confirmed",false);setProperty("pgn_ticket_landing_audited",false);
}
bool MiningTicketProgress::workMarksActive(qint64 now)const{
 for(const auto& entry:landings_)if((entry.complete&&now-entry.born<RosetteCollectMs)||(entry.audited&&entry.visualAt>=0&&now<entry.visualAt+RosetteValidationMs))return true;
 return false;
}
void MiningTicketProgress::pollLandings(){
 if(!lookup_||lookupBusy_)return;
 const auto now=clock_.elapsed();
 if(now<nextLookupAt_)return;
 auto next=std::min_element(landings_.begin(),landings_.end(),[](const auto& a,const auto& b){return a.nextCheck<b.nextCheck;});
 if(next==landings_.end()||next->nextCheck>now)return;
 {
  auto& entry=*next;
  entry.checking=true;entry.nextCheck=now+(entry.audited?30000:5000);nextLookupAt_=now+1000;
  lookupBusy_=true;
  // A queued animation uses a new active-branch proof at departure.
  const bool verifyDeparture=entry.audited&&entry.visualAt<0&&entry.approvedAt>=0&&now>=entry.approvedAt;
  const auto ticket=entry.ticket,request=entry.request;const auto generation=lookupGeneration_,query=++entry.query,lookupId=++activeLookup_;QPointer<MiningTicketProgress> weak(this);
  lookup_(request,[weak,ticket,request,generation,query,lookupId,verifyDeparture](QJsonObject state,QString error){
   if(!weak||weak->lookupGeneration_!=generation||!weak->lookupBusy_||weak->activeLookup_!=lookupId)return;
   weak->lookupBusy_=false;
   auto at=std::find_if(weak->landings_.begin(),weak->landings_.end(),[&](const auto& item){return item.ticket==ticket&&item.request==request&&item.checking&&item.query==query;});
   if(at==weak->landings_.end())return;
   const auto leases=state.value("leases").toArray();
   if(error.isEmpty()&&leases.size()==1){
    const auto lease=leases.first().toObject();
    if(lease.value("request")==request)
     rememberReportVisual(reportVisualKey(request,ticket),{},ticketNumber(lease.value("ticket_number").toString()));
   }
   const auto evidence=ticketFlowEvidence(request,ticket,state,error);
   const auto observedAt=weak->clock_.elapsed();
   const bool previouslyConfirmed=at->confirmed;
   at->checking=false;at->confirmed=evidence.included;at->block=evidence.block;
   if(evidence.included&&!at->complete){at->complete=true;at->born=observedAt;}
   if(evidence.audited&&!at->animatedBlock.isEmpty()&&at->animatedBlock!=evidence.block){
    at->visualAt=-1;
    at->animatedBlock.clear();
    at->audited=false;
   }
   if(evidence.audited&&!at->audited&&at->visualAt<0){
    at->approvedAt=std::max(observedAt,at->born+RosetteCollectMs);
    if(!previouslyConfirmed)at->approvedAt=std::max(at->approvedAt,observedAt+RosetteCollectMs);
    for(const auto& other:weak->landings_)
     if(&other!=&*at&&other.audited)
       at->approvedAt=std::max(at->approvedAt,(other.visualAt>=0?other.visualAt:other.approvedAt)+RosetteValidationMs);
      at->nextCheck=std::min(at->nextCheck,at->approvedAt);
     }
     if(!evidence.audited){
      at->approvedAt=-1;
      if(at->visualAt>=0)at->visualAt=std::min(at->visualAt,observedAt-RosetteValidationMs);
      at->nextCheck=std::min(at->nextCheck,observedAt+5000);
     }else if(verifyDeparture){
      qint64 waitUntil=observedAt;
      for(const auto& other:weak->landings_)
       if(&other!=&*at&&other.audited&&other.visualAt>=0)
        waitUntil=std::max(waitUntil,other.visualAt+RosetteValidationMs);
      if(waitUntil==observedAt){at->visualAt=observedAt;at->animatedBlock=evidence.block;}
      else at->nextCheck=waitUntil;
     }
     at->audited=evidence.audited;
   if(evidence.included)weak->flow_.confirmReport(ticket,weak->clock_.elapsed());else weak->flow_.revokeReport(ticket);
   QJsonArray deliveries;for(const auto& item:weak->landings_)deliveries.append(QJsonObject{{"ticket_id",item.ticket},{"request_id",item.request},{"confirmed",item.confirmed},{"audited",item.audited},{"block",item.block},{"visual_started",item.visualAt>=0}});weak->setProperty("pgn_ticket_deliveries",deliveries);
   weak->setProperty("pgn_ticket_landing_confirmed",evidence.included);weak->setProperty("pgn_ticket_landing_audited",evidence.audited);weak->setProperty("pgn_ticket_landing_error",error.left(160));
   weak->update();if(weak->overlay_)weak->overlay_->update();
  });
 }
}
QString MiningTicketProgress::reportTooltipAt(const QPointF& point)const{
 double distance=12.;
 QJsonObject closest;
 for(const auto& value:property("pgn_ticket_report_hits").toJsonArray()){
  const auto hit=value.toObject();
  const auto range=std::hypot(point.x()-hit.value("x").toDouble(),point.y()-hit.value("y").toDouble());
  if(range>=distance)continue;
  distance=range;
  closest=hit;
 }
 if(closest.isEmpty())return {};
 return reportVisualTooltip(closest.value("request").toString(),closest.value("ticket").toString(),
                            closest.value("confirmed").toBool(),closest.value("audited").toBool());
}
bool MiningTicketProgress::event(QEvent* event){
 if(event->type()==QEvent::ToolTip){
  const auto* help=static_cast<QHelpEvent*>(event);
  const auto reportTip=reportTooltipAt(help->pos());
  if(!reportTip.isEmpty()){
   QToolTip::showText(help->globalPos(),reportTip,this);
   setProperty("pgn_ticket_report_tooltip",true);
   return true;
  }
 }
 return QWidget::event(event);
}
void MiningTicketProgress::mouseMoveEvent(QMouseEvent* event){
 pointer_=event->position();QString closest;double distance=12.;
 for(const auto& hit:hits_){const double d=std::hypot(pointer_.x()-hit.point.x(),pointer_.y()-hit.point.y());if(d<distance){distance=d;closest=hit.pair;}}
 const bool showingReport=property("pgn_ticket_report_tooltip").toBool();
 const auto reportTip=reportTooltipAt(pointer_);
 const bool showReport=!reportTip.isEmpty();
 if(showReport){
  QToolTip::showText(event->globalPosition().toPoint(),reportTip,this);
  closest.clear();
 }else if(showingReport){
  QToolTip::hideText();
 }
 if(showReport!=showingReport)setProperty("pgn_ticket_report_tooltip",showReport);
 hovered_=closest;setCursor(hovered_.isEmpty()?Qt::ArrowCursor:Qt::PointingHandCursor);if(overlay_)overlay_->update();
}
void MiningTicketProgress::leaveEvent(QEvent* event){
 if(property("pgn_ticket_report_tooltip").toBool()){QToolTip::hideText();setProperty("pgn_ticket_report_tooltip",false);}
 hovered_.clear();unsetCursor();if(overlay_)overlay_->update();QWidget::leaveEvent(event);}
void MiningTicketProgress::mousePressEvent(QMouseEvent* event){
 if(event->button()!=Qt::LeftButton){QWidget::mousePressEvent(event);return;}
 mouseMoveEvent(event);if(hovered_.isEmpty()||!results_)return;
 if(auto* table=results_->findChild<QTableWidget*>("pgn-results-best"))for(int row=0;row<table->rowCount();++row){
  const auto* item=table->item(row,0);if(!item||item->data(Qt::UserRole).toString()!=hovered_)continue;
  results_->setProperty("pgn_best_selected_pair",hovered_);table->setCurrentCell(row,0);table->selectRow(row);pulsed_=hovered_;pulsedAt_=clock_.elapsed();if(overlay_)overlay_->update();break;
 }
}

}
