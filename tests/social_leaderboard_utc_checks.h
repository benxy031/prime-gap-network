#pragma once
#include "social/social_leaderboard.h"
#include "ui/social_view.h"
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QPushButton>
#include <QTableWidget>
#include <stdexcept>

namespace pgn::test::utc {
inline int checks=0;
inline void expect(bool good,const char* why){++checks;if(!good)throw std::runtime_error(why);}
template<class T>inline T* control(QWidget& root,const QString& name){auto* p=root.findChild<T*>(name);expect(p,"Missing UTC leaderboard control");return p;}
inline QJsonObject page(qint64 offset=0,bool more=true){
    const QJsonObject row{{"operator_id",QString(64,offset?'3':'2')},{"tickets_reported","1"},{"tickets_validated","1"},{"audits_finalized_valid","2"},{"validated_search_positions","9007199254740993"},{"last_activity_height",99}};
    return {{"schema","PGN_LEADERBOARD_V1"},{"genesis",QString(64,'a')},{"bestblockhash",QString(64,'c')},{"height",100},{"offset",offset},{"next_offset",offset+1},{"total",more?offset+2:offset+1},{"more",more},{"sort","validated_search_positions_desc_then_operator_id_asc"},{"rows",QJsonArray{row}}};
}
inline QJsonObject today(QJsonObject v=page(),qint64 start=0){
    v.insert("period","today_utc");v.insert("utc_start",start);v.insert("utc_end",start+86400);v.insert("history_complete",true);v.insert("event_time_source","canonical_block_header_time");v.insert("validation_date_scope","latest_current_qualification_episode");return v;
}
inline void run(QApplication& app,const QString& path){
    using namespace social;
    const Identity identity{QString(64,'a'),"SYNTHETIC-UTC-ACCOUNT"};
    ReadRequest total{Section::Miners,identity};QJsonArray args;
    expect(leaderboardArguments(total,args)&&args==QJsonArray{0,100},"Total RPC initial arguments changed");
    total.cursor=QString(64,'c')+":1";expect(leaderboardArguments(total,args)&&args==QJsonArray{1,100,QString(64,'c')},"Total RPC continuation arguments changed");
    ReadRequest day{Section::Miners,identity,Window::Day};
    expect(leaderboardArguments(day,args)&&args==QJsonArray{0,100,QJsonValue(QJsonValue::Null),"today_utc"},"Today initial query invents a day or anchor");
    day.cursor=QString(64,'c')+":1:0";expect(leaderboardArguments(day,args)&&args==QJsonArray{1,100,QString(64,'c'),"today_utc",0},"Today continuation lost UTC interval or anchor");
    const QJsonValue anchor(QString(64,'c'));
    const QList<QJsonArray> invalid{
        {},{0},{0,0},{0,101},{-1,1},{.5,1},{true,1},{0,true},{0,1,"INVALID"},
        {0,1,anchor,"total"},{0,1,anchor,"today_utc"},{1,1,QJsonValue(QJsonValue::Null),"today_utc"},
        {0,1,QJsonValue(QJsonValue::Null),"today_utc",0},{0,1,anchor,"today_utc",1},
        {0,1,anchor,"today_utc",-86400},{0,1,anchor,"today_utc","0"},
        {0,1,anchor,"today_utc",0.5},{0,1,anchor,"today_utc",0,"extra"}};
    for(const auto& bad:invalid)expect(!validLeaderboardRpcArguments(bad),"Scoped leaderboard bridge accepted invalid RPC arguments");
    for(const auto cursor:QStringList{QString(64,'c')+":01:0",QString(64,'c')+":1",QString(64,'c')+":1:1",QString(64,'c')+":1:00",QString(64,'c')+":1:-86400",QString(64,'C')+":1:0"}){
        auto req=day;req.cursor=cursor;expect(!leaderboardArguments(req,args),"Noncanonical/missing UTC cursor accepted");
    }
    Snapshot decoded;total.cursor.clear();day.cursor.clear();
    expect(decodeLeaderboard(total,page(),decoded).isEmpty()&&validate(decoded,total).isEmpty()&&decoded.window==Window::AllTime&&decoded.nextCursor==QString(64,'c')+":1","Total decoded values or cursor changed");
    expect(decodeLeaderboard(day,today(),decoded).isEmpty()&&validate(decoded,day).isEmpty()&&decoded.leaderboardPeriod->utcStart==0&&decoded.nextCursor==QString(64,'c')+":1:0","Today decoder did not retain explicit interval");
    const auto sentinel=decoded;
    const QList<QPair<QString,QJsonValue>> badFields{{"period","total"},{"utc_start",1},{"utc_end",86401},{"utc_start",.5},{"history_complete",false},{"history_complete",1},{"event_time_source","ticket_issued"},{"validation_date_scope","ticket_issued"},{"sort","gold_desc"},{"genesis",QString(64,'b')},{"offset",.5},{"height",.5},{"next_offset",.5}};
    for(const auto& item:badFields){auto bad=today();bad.insert(item.first,item.second);Snapshot output=sentinel;expect(!decodeLeaderboard(day,bad,output).isEmpty()&&output.nextCursor==sentinel.nextCursor&&output.miners.size()==sentinel.miners.size(),"Malformed UTC response accepted or partial decode escaped");}
    for(const auto field:QStringList{"period","utc_start","utc_end","history_complete","event_time_source","validation_date_scope"}){auto bad=today();bad.remove(field);expect(!decodeLeaderboard(day,bad,decoded).isEmpty(),"Missing UTC provenance accepted");}
    expect(!decodeLeaderboard(total,today(),decoded).isEmpty(),"Today response accepted as Total");
    day.cursor=sentinel.nextCursor;expect(decodeLeaderboard(day,today(page(1,false)),decoded).isEmpty(),"Pinned previous UTC day cannot continue");
    expect(!decodeLeaderboard(day,today(page(1,false),86400),decoded).isEmpty(),"Continuation moved to another UTC day");
    auto moved=today(page(1,false));moved.insert("bestblockhash",QString(64,'d'));expect(!decodeLeaderboard(day,moved,decoded).isEmpty(),"Continuation moved to another anchor");
    auto unproven=sentinel;unproven.leaderboardPeriod.reset();day.cursor.clear();expect(!validate(unproven,day).isEmpty(),"Generic service can omit UTC provenance");

    // UI fixture callbacks are synthetic only. Native/RPC qualification remains
    // a separate real-process witness. No local clock is modified by this test.
    struct Pending{ReadRequest request;Services::Reply done;};QVector<Pending> pending;
    Services service;service.read=[&](ReadRequest req,Services::Reply done){pending.append({req,done});};
    ui::SocialView view(path,service);view.resize(1366,900);view.show();view.setIdentity(identity);view.selectSection(Section::Miners);app.processEvents();
    const auto answer=[&](const Pending& call,QJsonObject value){Snapshot out;const auto error=decodeLeaderboard(call.request,value,out);call.done(std::move(out),error);};
    answer(pending.last(),page());auto* table=control<QTableWidget>(view,"pgn-social-miners");auto* window=control<QComboBox>(view,"pgn-social-miner-window");auto* interval=control<QLabel>(view,"pgn-social-miner-interval");
    expect(table->rowCount()==1&&interval->text().isEmpty()&&window->currentIndex()==0,"Total presentation changed");
    view.refresh();const auto oldTotal=pending.last();window->setCurrentIndex(1);const auto firstDay=pending.last();
    expect(firstDay.request.window==Window::Day&&firstDay.request.cursor.isEmpty()&&table->rowCount()==0,"Changing period retained Total rows/cursor");
    answer(oldTotal,page());expect(table->rowCount()==0,"Late Total response replaced Today");
    answer(firstDay,today());expect(table->rowCount()==1&&interval->text().contains("1970-01-01")&&interval->text().contains("1970-01-02")&&interval->toolTip().contains("canonical",Qt::CaseInsensitive),"Fixed half-open UTC interval is not visible");
    auto* more=control<QPushButton>(view,"pgn-social-more-3");more->click();const auto wrongDay=pending.last();
    expect(wrongDay.request.cursor==QString(64,'c')+":1:0","UI pagination discarded UTC start");
    Snapshot wrong;auto wrongRequest=wrongDay.request;wrongRequest.cursor.clear();expect(decodeLeaderboard(wrongRequest,today(page(1,false),86400),wrong).isEmpty()==false,"Invalid fixture offset was not refused");
    wrong=sentinel;wrong.miners[0].id=QString(64,'4');wrong.leaderboardPeriod->utcStart=86400;wrong.nextCursor.clear();wrongDay.done(wrong,{});
    expect(table->rowCount()==1&&view.property("pgn_social_state_3")=="error"&&interval->text().contains("1970-01-01"),"Service page mixed UTC intervals or erased retained snapshot");
    more->click();wrong=sentinel;wrong.miners[0].id=QString(64,'4');wrong.leaderboardPeriod->height=101;wrong.nextCursor.clear();pending.last().done(wrong,{});
    expect(table->rowCount()==1&&view.property("pgn_social_state_3")=="error","Service changed height for the same immutable anchor");
    more->click();answer(pending.last(),today(page(1,false)));expect(table->rowCount()==2,"Pinned old-day valid continuation failed");
    view.refresh();auto newDay=today(page(0,false),86400);newDay.insert("bestblockhash",QString(64,'d'));answer(pending.last(),newDay);
    expect(table->rowCount()==1&&interval->text().contains("1970-01-03"),"Refresh did not establish the new UTC snapshot");
    view.refresh();const auto staleDay=pending.last();window->setCurrentIndex(0);answer(staleDay,newDay);
    expect(table->rowCount()==1&&interval->text().isEmpty(),"Returning to cached Total lost its rows or accepted late Today");
    answer(pending.last(),page());expect(table->rowCount()==1&&interval->text().isEmpty(),"Total failed after leaving Today");
    window->setCurrentIndex(1);
    expect(table->rowCount()==0,"A prior UTC day occupied the current window");
    const auto utcDay=QDateTime(QDateTime::currentDateTimeUtc().date(),QTime(0,0),Qt::UTC).toSecsSinceEpoch();
    auto freshDay=today(page(0,false),utcDay);answer(pending.last(),freshDay);
    const auto label=interval->text();
    expect(table->rowCount()==1&&!label.isEmpty(),"Fresh window did not populate");
    window->setCurrentIndex(0);
    const auto pendingTotal=pending.last();
    expect(table->rowCount()==1&&interval->text().isEmpty(),"Total was not visible during refresh");
    window->setCurrentIndex(1);
    expect(table->rowCount()==1&&interval->text()==label,"Returning to verified Today cleared its row while Core was pending");
    answer(pendingTotal,page());
    expect(table->rowCount()==1&&interval->text()==label,"Late Total replaced the cached Today interval");
    const auto advertised=[](QJsonObject value) {
        value.insert("supported_periods", QJsonArray{"today_utc", "month_utc", "week_utc", "hour_utc"});
        return value;
    };
    view.refresh();
    answer(pending.last(),advertised(freshDay));
    auto* totalButton=control<QPushButton>(view,"pgn-social-period-total");
    auto* todayButton=control<QPushButton>(view,"pgn-social-period-today");
    auto* monthButton=control<QPushButton>(view,"pgn-social-period-month");
    expect(monthButton->isVisible(),"The advertised This month button was hidden");

    monthButton->click();
    expect(pending.last().request.window==Window::Month&&table->rowCount()==0,
           "This month showed Today before receiving its own data");
    const QDate date=QDateTime::currentDateTimeUtc().date();
    const auto monthStart=QDateTime(QDate(date.year(),date.month(),1),QTime(0,0),Qt::UTC).toSecsSinceEpoch();
    auto freshMonth=advertised(today(page(0,false),monthStart));
    freshMonth.insert("period","month_utc");
    freshMonth.insert("utc_end",*leaderboardWindowEnd(Window::Month,monthStart));
    answer(pending.last(),freshMonth);
    const auto monthLabel=interval->text();
    expect(table->rowCount()==1&&!monthLabel.isEmpty()&&monthLabel!=label,
           "This month did not show its own UTC interval");

    totalButton->click();
    expect(table->rowCount()==1&&interval->text().isEmpty(),"Total cleared while its refresh was pending");
    todayButton->click();
    expect(table->rowCount()==1&&interval->text()==label,"Today cleared while its refresh was pending");
    monthButton->click();
    expect(table->rowCount()==1&&interval->text()==monthLabel,"This month cleared while its refresh was pending");
    answer(pending.last(),freshMonth);

    view.refresh();const auto stale=pending.last();view.setIdentity({identity.genesis,"SYNTHETIC-OTHER-ACCOUNT"});answer(stale,newDay);
    expect(table->rowCount()==0&&interval->text().isEmpty(),"Identity change retained old rows/UTC interval or accepted stale callback");
    view.close();
}

inline void runWarmPeriods(const QString& path){
    using namespace social;
    struct Pending{ReadRequest request;Services::Reply done;};
    QVector<Pending> pending;
    Services service;
    service.read=[&](ReadRequest request,Services::Reply done){
        if(request.section==Section::Miners)pending.append({request,std::move(done)});
    };
    ui::SocialView view(path,service);
    view.resize(1366,900);
    view.show();
    const Identity identity{QString(64,'a'),"SYNTHETIC-PERIOD-CACHE"};
    view.setIdentity(identity);
    view.selectSection(Section::Miners);
    auto* table=control<QTableWidget>(view,"pgn-social-miners");
    auto* interval=control<QLabel>(view,"pgn-social-miner-interval");
    auto* totalButton=control<QPushButton>(view,"pgn-social-period-total");
    auto* todayButton=control<QPushButton>(view,"pgn-social-period-today");
    auto* hourButton=control<QPushButton>(view,"pgn-social-period-hour");
    auto* weekButton=control<QPushButton>(view,"pgn-social-period-week");
    auto* monthButton=control<QPushButton>(view,"pgn-social-period-month");
    const auto wait=[&](qsizetype count){
        QElapsedTimer deadline;
        deadline.start();
        while(pending.size()<count&&deadline.elapsed()<2500){
            QEventLoop loop;
            QTimer::singleShot(25,&loop,&QEventLoop::quit);
            loop.exec();
        }
        expect(pending.size()==count,"Expected one background request before answering Core");
    };
    const auto answer=[&](const Pending& call,bool all=true){
        auto value=page(0,false);
        if(all)value.insert("supported_periods", QJsonArray{"today_utc","month_utc","week_utc","hour_utc"});
        const auto window=call.request.window;
        const auto now=QDateTime::currentDateTimeUtc();
        const auto day=now.toSecsSinceEpoch()/86400*86400;
        qint64 start=day;
        switch(window){
        case Window::Hour:start=now.toSecsSinceEpoch()/3600*3600;break;
        case Window::Week:start=day-(now.date().dayOfWeek()-1)*86400;break;
        case Window::Month:start=day-(now.date().day()-1)*86400;break;
        default:break;
        }
        if(window!=Window::AllTime){
            value=today(value,start);
            value.insert("period",leaderboardPeriodName(window));
            value.insert("utc_end",*leaderboardWindowEnd(window,start));
        }
        auto row=value.value("rows").toArray().first().toObject();
        row.insert("display_name",QString("Period %1").arg(static_cast<int>(window)));
        value.insert("rows",QJsonArray{row});
        Snapshot result;
        const auto error=decodeLeaderboard(call.request,value,result);
        expect(error.isEmpty(),"Clock-aligned period fixture is invalid");
        call.done(std::move(result),error);
    };
    const auto shows=[&](Window window){
        return table->rowCount()==1&&table->item(0,0)&&
            table->item(0,0)->text().contains(QString("Period %1").arg(static_cast<int>(window)));
    };
    wait(1);
    expect(pending[0].request.window==Window::AllTime,"Expected foreground Total before warmup");
    answer(pending[0]);
    expect(shows(Window::AllTime)&&interval->text().isEmpty(),"Expected authenticated first Total");
    wait(2);
    const auto warmDay=pending[1];
    expect(warmDay.request.window==Window::Day,"Today was not the first background period");
    todayButton->click();
    expect(pending.size()==2&&table->rowCount()==0,"Click sent a duplicate Today query or showed Total");
    answer(warmDay);
    expect(shows(Window::Day)&&!interval->text().isEmpty(),"First background response did not reach the selected period");

    wait(3);
    const auto warmHour=pending[2];
    expect(warmHour.request.window==Window::Hour,"Hour did not warm next");
    answer(warmHour);
    wait(4);
    const auto warmWeek=pending[3];
    expect(warmWeek.request.window==Window::Week,"Week did not warm next");
    answer(warmWeek);
    wait(5);
    const auto warmMonth=pending[4];
    expect(warmMonth.request.window==Window::Month,"Month did not warm next");
    answer(warmMonth);

    QElapsedTimer switches;
    switches.start();
    for(int round=0;round<5;++round){
        totalButton->click();expect(shows(Window::AllTime),"Total did not switch synchronously");
        monthButton->click();expect(shows(Window::Month),"Month did not switch synchronously");
        weekButton->click();expect(shows(Window::Week),"Week did not switch synchronously");
        hourButton->click();expect(shows(Window::Hour),"Hour did not switch synchronously");
        todayButton->click();expect(shows(Window::Day),"Today did not switch synchronously");
    }
    const auto switchNanos=switches.nsecsElapsed();
    expect(pending.size()==5,"A warm switch generated another backend request");
    view.refresh();
    wait(6);
    const auto manualToday=pending[5];
    warmDay.done({},"Repeated obsolete provider response");
    expect(shows(Window::Day)&&view.property("pgn_social_state_3")=="loading",
           "An older duplicate completion stole a newer refresh");
    answer(manualToday);
    expect(shows(Window::Day),"Explicit refresh failed after an old completion");
    view.refresh();
    wait(7);
    const auto stale=pending[6];
    view.setIdentity({identity.genesis,"SYNTHETIC-PERIOD-CACHE-OTHER"});
    answer(stale);
    expect(table->rowCount()==0&&interval->text().isEmpty(),"Old identity restored cached rows");

    const auto current=pending.last();
    expect(current.request.identity!=identity,"New identity did not request its ranking");
    answer(current,false);
    totalButton->click();
    if(pending.last().request.window==Window::AllTime)answer(pending.last(),false);
    const auto beforeUnsupported=pending.size();
    QEventLoop quiet;
    QTimer::singleShot(350,&quiet,&QEventLoop::quit);
    quiet.exec();
    expect(!monthButton->isVisible()&&!weekButton->isVisible()&&!hourButton->isVisible()&&
           pending.size()<=beforeUnsupported+1&&
           (pending.size()==beforeUnsupported||pending.last().request.window==Window::Day),
           "Warmup requested a period this Core did not advertise");
    qInfo("Verified 25 synchronous warm period switches in %.3f ms; zero extra backend requests.",
          double(switchNanos)/1e6);
}

}
