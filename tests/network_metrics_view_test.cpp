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
#include <QWheelEvent>
#include <cmath>
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
    auto* walletChart=home->findChild<QWidget*>("pgn-wallet-network-chart");
    require(walletChart,"Wallet balances does not expose the unified chart");
    auto draw=[&](QWidget* view){
        app.processEvents();
        QImage image(view->size(),QImage::Format_ARGB32);
        image.fill(Qt::transparent);
        view->render(&image);
        return image;
    };
    auto plotFor=[&]{
        const auto frame=walletChart->property("pgn_network_chart_geometry").toJsonArray();
        require(frame.size()==1,"Wallet balances must expose exactly one plot");
        const auto geometry=frame[0].toObject();
        require(geometry["left_axis"].toString().contains("hashrate")&&geometry["right_axis"].toString().contains("difficulty"),
                "Wallet chart unit axes are swapped or absent");
        require(geometry["horizontal_ticks"].toInt()>=2,"Wallet chart lost regular time graduations");
        return QRectF(geometry["x"].toDouble(),geometry["y"].toDouble(),geometry["width"].toDouble(),geometry["height"].toDouble());
    };
    auto bounds=[&]{return walletChart->property("pgn_rate_plot_bounds").toList();};
    auto mouse=[&](QEvent::Type type,QPointF at,Qt::MouseButton button,Qt::MouseButtons buttons){
        QMouseEvent event(type,at,walletChart->mapToGlobal(at.toPoint()),button,buttons,Qt::NoModifier);
        QCoreApplication::sendEvent(walletChart,&event);
    };
    for(const int width:{520,800,1400}){
        home->resize(width,330);
        const auto image=draw(home);
        const auto plot=plotFor();
        const auto rate=bounds(), merit=walletChart->property("pgn_merit_plot_bounds").toList();
        require(rate[0]==merit[0]&&rate[1]==merit[1]&&rate[2]!=merit[2],"Wallet axes are not independent on one time viewport");
        require(merit[3].toDouble()-merit[2].toDouble()<1.,"Wallet merit axis lost its own scale");
        int ratePixels=0,meritPixels=0;
        const auto origin=walletChart->mapTo(home,QPoint());
        for(int y=int(plot.top());y<=plot.bottom();++y)for(int x=int(plot.left()+4);x<plot.right()-4;++x){
            const auto color=image.pixelColor(origin+QPoint(x,y));
            if(color.green()>170&&color.green()-color.red()>65&&color.green()>color.blue())++ratePixels;
            if(color.red()>200&&color.green()>150&&color.green()-color.blue()>70)++meritPixels;
        }
        require(ratePixels>30&&meritPixels>30,"Both exact-source curves must paint in the shared plot");
        const auto middle=plot.center();
        mouse(QEvent::MouseMove,middle,Qt::NoButton,Qt::NoButton);
        const auto hover=walletChart->property("pgn_network_chart_hover").toJsonObject();
        require(hover["sample_index"].toInt()==1&&hover["reported_positions"]==points[1].toObject()["reported_positions"],
                "Wallet hover did not retain an actual observation");
        require(walletChart->toolTip().contains("Smoothed estimate")&&walletChart->toolTip().contains("Exact target"),
                "Wallet tooltip conflates exact and smoothed values");
        if(argc>1)require(image.save(QString::fromLocal8Bit(argv[1])+QString("/wallet-%1.png").arg(width)),"Wallet render receipt failed");
    }
    const auto plot=plotFor();
    const auto center=plot.center();
    const auto screen=walletChart->mapToGlobal(center.toPoint());
    const auto unzoomed=bounds();
    QWheelEvent wheel(center,screen,{},QPoint(0,480),Qt::NoButton,Qt::NoModifier,Qt::ScrollUpdate,false);
    QCoreApplication::sendEvent(walletChart,&wheel);
    draw(home);
    const auto zoomed=bounds();
    require(zoomed[1].toDouble()-zoomed[0].toDouble()<unzoomed[1].toDouble()-unzoomed[0].toDouble(),"Wallet scroll did not zoom");
    require(std::abs((zoomed[0].toDouble()+zoomed[1].toDouble())-(unzoomed[0].toDouble()+unzoomed[1].toDouble()))<.1,
            "Wallet zoom changed the timestamp below the middle of the pointer");
    mouse(QEvent::MouseButtonPress,center,Qt::RightButton,Qt::RightButton);
    const auto left=center-QPointF(plot.width()/10.,0.);
    mouse(QEvent::MouseMove,left,Qt::NoButton,Qt::RightButton);
    draw(home);
    const auto during=bounds();
    require(during[0].toDouble()>zoomed[0].toDouble(),"Wallet pan redraws only after button release");
    mouse(QEvent::MouseButtonRelease,left,Qt::RightButton,Qt::NoButton);
    draw(home);
    require(during==bounds()&&pending.empty(),"Wallet pan snapped or requested data on release");
    home->findChild<QPushButton*>("pgn-wallet-network-chart-reset")->click();
    draw(home);
    require(bounds()==unzoomed,"Wallet reset did not recover the full selected period");
    QJsonArray noisy;
    for(int i=0;i<21;++i){
        auto point=points[0].toObject();
        point.insert("time",1793550000+i*30);
        point.insert("height",i+1);
        point.insert("reported_positions", i==10?"1200000000000000":"600000000000000");
        noisy.append(point);
    }
    auto noisySnapshot=snapshot;noisySnapshot.insert("points",noisy);
    noisySnapshot.insert("span_seconds",3600);noisySnapshot.insert("window_seconds",600);
    hub.refresh();answer(noisySnapshot);draw(home);
    mouse(QEvent::MouseMove,plotFor().center(),Qt::NoButton,Qt::NoButton);
    const auto filtered=walletChart->property("pgn_network_chart_hover").toJsonObject();
    require(filtered["reported_positions"]=="1200000000000000"&&filtered["visible_rate"].toDouble()>1.0e12&&filtered["visible_rate"].toDouble()<1.1e12,
            "Elapsed-time filter does not correctly attenuate the one-sample reporting spike");
    home->findChild<QPushButton*>("pgn-wallet-network-chart-smooth")->click();
    draw(home);
    mouse(QEvent::MouseMove,plotFor().center(),Qt::NoButton,Qt::NoButton);
    require(walletChart->property("pgn_network_chart_hover").toJsonObject()["visible_rate"].toDouble()==2e12,
            "Unchecking Smooth did not restore the original reported rate");
    auto* week = home->findChild<QPushButton*>("pgn-network-metrics-span-604800");
    auto* homeDay = home->findChild<QPushButton*>("pgn-network-metrics-span-86400");
    homeDay->click();
    week->click();
    require(pending.size()==1&&pending.front().first==604800,"Wallet period did not dispatch the shared request");
    homeDay->click();
    answer(noisySnapshot);
    require(pending.size()==1&&pending.front().first==86400,"Superseded Wallet period response was not discarded");
    require(home->property("pgn_network_metrics_snapshot").toJsonObject()==noisySnapshot,"Stale period replaced Wallet frame");

    QJsonArray daily;
    for(int i=0;i<133;++i){
        auto point=points[0].toObject();
        const int work=310+int(120.*std::sin(i*0.07))+((i%4==0)?130:-45)+(i>53&&i<82?165:0);
        point.insert("time",1793516400+i*600);
        point.insert("height",i+1);
        point.insert("gold_count",i/3);
        point.insert("merit_target_q16",27*65536+(i/12)*1638+(i>94?6554:0));
        point.insert("reported_positions",QString::number(qint64(work)*600000000000LL));
        if(i==28)point.insert("window_seconds",0);
        daily.append(point);
    }
    auto dailySnapshot=snapshot;
    dailySnapshot.insert("height",133);
    dailySnapshot.insert("bestblockhash", QString(64,'a'));
    dailySnapshot.insert("span_seconds",86400);
    dailySnapshot.insert("window_seconds",600);
    dailySnapshot.insert("points",daily);
    answer(dailySnapshot);
    require(homeDay->isChecked()&&!week->isChecked(),"Wallet period selector and shared snapshot diverged");
    home->findChild<QPushButton*>("pgn-wallet-network-chart-smooth")->click();
    home->resize(1140,345);
    const auto dailyImage=draw(home);
    const auto dailyPlot=plotFor();
    require(walletChart->property("pgn_network_chart_time_ticks").toList().size()>=5,"Wide daily view lacks enough time ticks");
    const double missingFraction=28./132.;
    mouse(QEvent::MouseMove,QPointF(dailyPlot.left()+missingFraction*dailyPlot.width(),dailyPlot.center().y()),Qt::NoButton,Qt::NoButton);
    require(!walletChart->property("pgn_network_chart_hover").toJsonObject().contains("visible_rate"),
            "Wallet rate filter filled an unavailable sample");
    if(argc>1)require(dailyImage.save(QString::fromLocal8Bit(argv[1])+"/wallet-illustrative-day.png"),"Daily render receipt failed");

    hub.refresh();answer(snapshot);draw(home);

    for(auto* view:{history})for(const auto width:{520,800,1400}){
        view->resize(width,view==home?260:600);app.processEvents();QImage image(view->size(),QImage::Format_ARGB32);image.fill(Qt::transparent);view->render(&image);
        const auto merit=view->property("pgn_merit_plot_bounds").toList(),rate=view->property("pgn_rate_plot_bounds").toList();
        require(merit.size()==4&&rate.size()==4&&merit[3].toDouble()>merit[2].toDouble()&&rate[3].toDouble()>0,"Valid data produces a blank or degenerate graph");
        require(merit[3].toDouble()-merit[2].toDouble()<1,"Merit scale flattened against an unrelated unit range");
        const auto charts=view->property("pgn_network_chart_geometry").toJsonArray();require(charts.size()==2,"Both chart axes must expose actual geometry");
        const auto rateGeometry=charts[1].toObject();
        const auto pixelsAt=[&](double value,bool primary){
            const auto height=rateGeometry["height"].toDouble();
            const int row=int(rateGeometry["y"].toDouble()+height*(1-value/rate[3].toDouble()));
            const auto x=rateGeometry["x"].toDouble(), width=rateGeometry["width"].toDouble();
            int hits=0;
            for(int y=row-2;y<=row+2;++y){
                for(int px=int(x+width*.20);px<=int(x+width*.80);++px){
                    const auto c=image.pixelColor(px,y);
                    if(primary?c.green()>160&&c.green()-c.red()>50&&c.green()>c.blue():
                        c.blue()>160&&c.blue()-c.green()>25&&c.blue()-c.red()>70)++hits;
                }
            }
            return hits;
        };
        require(pixelsAt(1e12,true)>12,"Measured curve disagrees with reported positions over chain time");
        require(pixelsAt(2e14/600.,false)==0,"A second audited hashrate curve is still drawn");
        for(int chart=0;chart<2;++chart){const auto g=charts[chart].toObject();require(g.value("axis_font_px").toInt()>=11&&g.value("vertical_ticks").toInt()>=3&&g.value("horizontal_ticks").toInt()>=2,"Axes lack readable value/time graduations");require(g.value("samples").toInt()==points.size(),"Sample annotation fabricates precision");
            const QPointF at(g.value("x").toDouble()+g.value("width").toDouble()/2,g.value("y").toDouble()+g.value("height").toDouble()/2);
            QMouseEvent move(QEvent::MouseMove,at,view->mapToGlobal(at.toPoint()),Qt::NoButton,Qt::NoButton,Qt::NoModifier);QCoreApplication::sendEvent(view,&move);app.processEvents();
            const auto hover=view->property("pgn_network_chart_hover").toJsonObject();require(hover.value("chart").toInt()==chart&&hover.value("sample_index").toInt()==1&&hover.value("time")==points[1].toObject().value("time"),"Crosshair must select a real nearest sample, never interpolated data");require(hover.value("reported_positions")==points[1].toObject().value("reported_positions")&&hover.value("merit_target_q16")==points[1].toObject().value("merit_target_q16"),"Hover exact source values changed");
            if(chart==1)require(view->toolTip().contains("Audited:"),"Audited value is missing from sample details");
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
    hub.refresh();answer(QJsonObject{{"height",1},{"points",QJsonArray{points.first()}}});render(home);require(walletChart->property("pgn_rate_plot_bounds").toList()[3].toDouble()>0,"Single-sample rate is degenerate");
    hub.refresh();answer(QJsonObject{{"height",0},{"points",QJsonArray{}}});render(home);require(walletChart->property("pgn_merit_plot_bounds").toList().isEmpty()&&walletChart->property("pgn_rate_plot_bounds").toList().isEmpty(),"Empty source kept old data bounds");
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
    std::deque<std::function<void(QJsonObject,QString)>> branchReads;
    NetworkMetricsHub branchHub([&](int,auto callback){branchReads.push_back(std::move(callback));});
    QWidget branchSubscriber;
    QJsonObject delivered;
    int deliveries=0;
    branchHub.subscribe(&branchSubscriber,[&](const auto& value,const auto&){delivered=value;++deliveries;});
    const auto genesis=QString(64,'a'), hashA=QString(64,'b'), hashB=QString(64,'c'), hashC=QString(64,'d');
    const auto runBranchRead=[&](const QJsonObject& data){
        require(branchReads.size()==1,"Each ledger branch has one in-flight metrics read");
        auto finish=std::move(branchReads.front());branchReads.pop_front();finish(data,{});
    };
    branchHub.observeTip(genesis,hashA,10);
    const QJsonObject oldSnapshot{{"genesis",genesis},{"height",10},{"bestblockhash",hashA}};
    runBranchRead(oldSnapshot);
    branchHub.refresh();
    branchHub.observeTip(genesis,hashB,10);
    require(deliveries==2&&delivered.isEmpty(),"Subscribers must immediately retire metrics from a known old branch");
    branchHub.observeTip(genesis,hashC,11);
    runBranchRead(oldSnapshot);
    require(deliveries==2,"An old in-flight metrics result crossed a known branch change");
    const QJsonObject verifiedNewSnapshot{{"genesis",genesis},{"height",12},{"bestblockhash",QString(64,'e')}};
    runBranchRead(verifiedNewSnapshot);
    require(deliveries==3&&delivered==verifiedNewSnapshot,"Verified metrics at a different height must flow during catch-up");
    std::cout<<"PASS wallet network chart and graph hub: shared frame, three dual-axis wallet renders, three detailed renders, exact tooltip, pointer-anchored zoom, live pan, smoothing and raw mode, readable value/time axes, exact-sample crosshair/leave/change,256point history,single/empty source, retained error frame, stale response, exclusive selection, destroyed subscriber\n";
}
