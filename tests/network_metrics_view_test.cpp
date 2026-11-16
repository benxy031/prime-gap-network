#include "ui/network_metrics_view.h"
#include "ui/network_metrics_caption.h"
#include "ui/panel_style.h"
#include "ui/render/horizon_workspace_style.h"
#include <QLabel>
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QJsonArray>
#include <QPushButton>
#include <QMouseEvent>
#include <QEventLoop>
#include <deque>
#include <iostream>
#include <stdexcept>
using namespace pgn::ui;
static void require(bool okay,const char* message){if(!okay)throw std::runtime_error(message);}
int main(int argc,char** argv){
    QApplication app(argc,argv);app.setStyleSheet(dataPanelStyle()+HorizonWorkspaceControlStyle());std::deque<std::pair<int,std::function<void(QJsonObject,QString)>>> pending;
    NetworkMetricsHub hub([&](int span,auto callback){pending.push_back({span,std::move(callback)});});
    auto* home=createNetworkMetricsView(&hub,true);auto* history=createNetworkMetricsView(&hub,false);
    home->resize(700,260);history->resize(1100,600);home->show();history->show();app.processEvents();
    require(pending.size()==1,"Two views opened duplicate parallel requests");
    QJsonArray points;for(int i=0;i<3;++i)points.append(QJsonObject{{"time",1793550000+i*300},{"height",i+1},
        {"gold_count",i},{"hash",QString(64,QChar(u'a'+i))},{"merit_target_q16",28*65536+i*137},{"window_seconds",600},{"reported_positions","600000000000000"},{"audited_positions","200000000000000"}});
    QJsonObject snapshot{{"height",3},{"bestblockhash",QString(64,'c')},{"points",points}};
    auto answer=[&](QJsonObject value,QString error={}){auto callback=std::move(pending.front().second);pending.pop_front();callback(value,error);app.processEvents();};
    answer(snapshot);if(!pending.empty())answer(snapshot);
    require(networkGoldCount(snapshot)==2,"Primary Gold count must not be the coordination ledger height");
    require(networkMetricsCaption(snapshot,{}).contains("Gold blocks 2"),"Primary caption does not show Gold blocks");
    auto malformed=snapshot;auto badPoints=points;auto bad=badPoints.last().toObject();bad.remove("gold_count");badPoints.replace(2,bad);malformed.insert("points",badPoints);
    require(!networkGoldCount(malformed)&&networkMetricsCaption(malformed,{}).contains("unavailable"),"Absent Gold data invented a count");
    bad.insert("gold_count",0);badPoints.replace(2,bad);malformed.insert("points",badPoints);require(networkGoldCount(malformed)==0,"Verified zero Gold count was hidden");
    bad.insert("gold_count",4);badPoints.replace(2,bad);malformed.insert("points",badPoints);require(!networkGoldCount(malformed),"Gold count exceeded ledger height");
    bad.insert("gold_count",1.5);badPoints.replace(2,bad);malformed.insert("points",badPoints);require(!networkGoldCount(malformed),"Fractional Gold count accepted");
    malformed=snapshot;malformed.insert("bestblockhash",QString(64,'d'));require(!networkGoldCount(malformed),"Mixed branch Gold count accepted");
    malformed=snapshot;malformed.insert("height",4);require(!networkGoldCount(malformed),"Mixed height Gold count accepted");
    require(networkMetricsCaption(snapshot,"Connection lost").contains("unavailable")&&!networkMetricsCaption(snapshot,"Connection lost").contains("Gold blocks 2"),"Stale count presented as current after transport failure");
    require(networkMetricsTechnicalTip(snapshot).contains("3"),"Technical ledger height was lost");
    require(home->property("pgn_network_metrics_snapshot").toJsonObject()==snapshot&&history->property("pgn_network_metrics_snapshot").toJsonObject()==snapshot,"Views do not share one exact snapshot");
    for(auto* view:{home,history}){bool caption=false;for(auto* label:view->findChildren<QLabel*>())if(label->text().contains("Gold blocks 2")){caption=true;require(label->toolTip().contains("3"),"Actual caption lost technical ledger tooltip");}require(caption,"Actual graph widget does not show the verified Gold count");}
    for(auto* view:{home,history})for(const auto width:{520,800,1400}){
        view->resize(width,view==home?260:600);app.processEvents();QImage image(view->size(),QImage::Format_ARGB32);image.fill(Qt::transparent);view->render(&image);
        const auto merit=view->property("pgn_merit_plot_bounds").toList(),rate=view->property("pgn_rate_plot_bounds").toList();
        require(merit.size()==4&&rate.size()==4&&merit[3].toDouble()>merit[2].toDouble()&&rate[3].toDouble()>0,"Valid data produces a blank or degenerate graph");
        require(merit[3].toDouble()-merit[2].toDouble()<1,"Merit scale flattened against an unrelated unit range");
        const auto charts=view->property("pgn_network_chart_geometry").toJsonArray();require(charts.size()==2,"Both chart axes must expose actual geometry");
        for(int chart=0;chart<2;++chart){const auto g=charts[chart].toObject();require(g.value("axis_font_px").toInt()>=11&&g.value("vertical_ticks").toInt()>=3&&g.value("horizontal_ticks").toInt()>=2,"Axes lack readable value/time graduations");require(g.value("samples").toInt()==points.size(),"Sample annotation fabricates precision");
            const QPointF at(g.value("x").toDouble()+g.value("width").toDouble()/2,g.value("y").toDouble()+g.value("height").toDouble()/2);
            QMouseEvent move(QEvent::MouseMove,at,view->mapToGlobal(at.toPoint()),Qt::NoButton,Qt::NoButton,Qt::NoModifier);QCoreApplication::sendEvent(view,&move);app.processEvents();
            const auto hover=view->property("pgn_network_chart_hover").toJsonObject();require(hover.value("chart").toInt()==chart&&hover.value("sample_index").toInt()==1&&hover.value("time")==points[1].toObject().value("time"),"Crosshair must select a real nearest sample, never interpolated data");require(hover.value("reported_positions")==points[1].toObject().value("reported_positions")&&hover.value("merit_target_q16")==points[1].toObject().value("merit_target_q16"),"Hover exact source values changed");
            require(view->toolTip().contains("Exact target: "+QString::number(points[1].toObject().value("merit_target_q16").toInteger())+"/65536")&&view->toolTip().contains("Target merit: ≈ "),"Rounded merit lacks approximation or exact source ratio");
        }
        QEvent leave(QEvent::Leave);QCoreApplication::sendEvent(view,&leave);require(view->toolTip().isEmpty()&&view->property("pgn_network_chart_hover").toJsonObject().isEmpty(),"Stale hover remains outside chart");
        if(argc>1){QDir().mkpath(QString::fromLocal8Bit(argv[1]));require(image.save(QString::fromLocal8Bit(argv[1])+QString("/%1-%2.png").arg(view==home?"overview":"history").arg(width)),"Render receipt failed");}
    }
    hub.refresh();answer({},"Injected connection loss");require(home->property("pgn_network_metrics_snapshot").toJsonObject()==snapshot,"Read failure erased the last verified frame");
    auto* hour=history->findChild<QPushButton*>("pgn-network-metrics-span-3600");hour->click();
    require(pending.size()==1&&pending.front().first==3600,"History button did not select its bounded request");
    auto* day=history->findChild<QPushButton*>("pgn-network-metrics-span-86400");day->click();answer(snapshot);
    require(pending.size()==1&&pending.front().first==86400,"Stale history response was not replaced");answer(snapshot);
    require(day->isChecked()&&!hour->isChecked(),"Span selection is ambiguous");
    // A full bounded source history stays exact; no resampling/interpolation is
    // promoted to a data observation. The first real sample also renders.
    QJsonArray dense;for(int i=0;i<256;++i){auto point=points[0].toObject();point.insert("time",1793550000+i*60);point.insert("height",i+1);point.insert("merit_target_q16",28*65536+i);dense.append(point);}
    auto render=[&](QWidget* view){app.processEvents();QImage image(view->size(),QImage::Format_ARGB32);image.fill(Qt::transparent);view->render(&image);return image;};
    hub.refresh();answer(QJsonObject{{"height",256},{"points",dense}});render(history);require(history->property("pgn_network_chart_geometry").toJsonArray()[0].toObject().value("samples")==256,"Dense bounded history lost source points");require(history->property("pgn_network_chart_hover").toJsonObject().isEmpty(),"Changed snapshot retained old sample hover");
    hub.refresh();answer(QJsonObject{{"height",1},{"points",QJsonArray{points.first()}}});render(home);require(home->property("pgn_rate_plot_bounds").toList()[3].toDouble()>0,"Single-sample rate is degenerate");
    hub.refresh();answer(QJsonObject{{"height",0},{"points",QJsonArray{}}});render(home);require(home->property("pgn_merit_plot_bounds").toList().isEmpty()&&home->property("pgn_rate_plot_bounds").toList().isEmpty(),"Empty source kept old data bounds");
    delete home;delete history;hub.refresh();answer(snapshot); // Closing subscribers cannot leave dangling callbacks.
    // Mining consumes the shared snapshot through an ordinary QWidget,
    // unlike the graph subclass. Test that exact subscription boundary.
    std::deque<std::function<void(QJsonObject,QString)>> visibilityReads;
    NetworkMetricsHub visibilityHub([&](int,auto callback){visibilityReads.push_back(std::move(callback));});
    QWidget miningPage,secondPage;int observations=0;
    visibilityHub.subscribe(&miningPage,[&](const auto&,const auto&){++observations;});
    visibilityHub.subscribe(&secondPage,[&](const auto&,const auto&){++observations;});
    visibilityHub.refresh();auto seed=std::move(visibilityReads.front());visibilityReads.pop_front();seed(snapshot,{});
    QEventLoop hiddenWait;QTimer::singleShot(5600,Qt::PreciseTimer,&hiddenWait,&QEventLoop::quit);hiddenWait.exec();
    require(visibilityReads.empty(),"Hidden pages must not poll a retained snapshot");
    miningPage.show();secondPage.show();app.processEvents();app.processEvents();
    if(visibilityReads.size()!=1){std::cerr<<"Hidden-page return did not request one fresh snapshot immediately; pending="<<visibilityReads.size()<<"\n";return 7;}
    miningPage.hide();miningPage.show();secondPage.hide();secondPage.show();app.processEvents();app.processEvents();
    require(visibilityReads.size()==1,"Showing two subscribers queued duplicate concurrent refreshes");
    auto fresh=std::move(visibilityReads.front());visibilityReads.pop_front();fresh(snapshot,{});app.processEvents();
    require(visibilityReads.empty()&&observations==4,"A visibility refresh queued an unnecessary follow-up read");
    miningPage.hide();miningPage.show();app.processEvents();app.processEvents();
    require(visibilityReads.empty(),"Rapid page toggles ignored the fresh five-second budget");
    std::cout<<"PASS network graph hub: shared frame, six responsive renders, readable value/time axes, exact-sample crosshair/leave/change,256point history,single/empty source, retained error frame, stale response, exclusive selection, destroyed subscriber\n";
}
