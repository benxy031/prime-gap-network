// Copyright (c) 2026 The PGN developers. MIT. Read-only RPC calendar bounds.
#pragma once
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string_view>
namespace pgn::core {
enum class LeaderboardUTCPeriod { Day,Hour,Week,Month };
inline std::optional<LeaderboardUTCPeriod> ParseLeaderboardPeriod(std::string_view text){
    if(text=="today_utc")return LeaderboardUTCPeriod::Day;
    if(text=="hour_utc")return LeaderboardUTCPeriod::Hour;
    if(text=="week_utc")return LeaderboardUTCPeriod::Week;
    if(text=="month_utc")return LeaderboardUTCPeriod::Month;
    return {};
}
struct LeaderboardUTCWindow {
    std::optional<int64_t> utc_start; // absent = existing Total semantics
    LeaderboardUTCPeriod period{LeaderboardUTCPeriod::Day};
    const char* Name()const{
        if(!utc_start)return "total";
        switch(period){case LeaderboardUTCPeriod::Day:return "today_utc";
        case LeaderboardUTCPeriod::Hour:return "hour_utc";
        case LeaderboardUTCPeriod::Week:return "week_utc";
        case LeaderboardUTCPeriod::Month:return "month_utc";}
        throw std::invalid_argument("Unknown UTC period");
    }
    static LeaderboardUTCWindow FromStart(int64_t start,LeaderboardUTCPeriod period=LeaderboardUTCPeriod::Day){
        int64_t span=86400;
        switch(period){
        case LeaderboardUTCPeriod::Hour:span=3600;break;
        case LeaderboardUTCPeriod::Week:span=7*86400;break;
        case LeaderboardUTCPeriod::Day:break;
        case LeaderboardUTCPeriod::Month:{
            // Gregorian years1970..9999; next-month midnight may be in10000.
            if(start<0||start>253402214400LL||start%86400)throw std::invalid_argument("Canonical UTC month required");
            const std::chrono::year_month_day date{std::chrono::sys_days{std::chrono::days{start/86400}}};
            if(!date.ok()||unsigned(date.day())!=1)throw std::invalid_argument("Canonical UTC month required");
            return {start,period};
        }
        default:throw std::invalid_argument("Unknown UTC period");
        }
        // At Unix epoch the current Monday week starts on1969-12-29.
        const int64_t minimum=period==LeaderboardUTCPeriod::Week?-3*86400:0;
        if(start<minimum||start% (period==LeaderboardUTCPeriod::Hour?3600:86400)
            ||start>std::numeric_limits<int64_t>::max()-span
            ||(period==LeaderboardUTCPeriod::Week&&(start/86400+3)%7))
            throw std::invalid_argument("Canonical UTC calendar boundary required");
        return {start,period};
    }
    static LeaderboardUTCWindow At(LeaderboardUTCPeriod period,int64_t now){
        if(now<0)throw std::invalid_argument("UTC request clock unavailable");
        if(period==LeaderboardUTCPeriod::Hour)return FromStart(now-now%3600,period);
        const int64_t day=now/86400;
        if(period==LeaderboardUTCPeriod::Week)return FromStart((day-(day+3)%7)*86400,period);
        if(period==LeaderboardUTCPeriod::Month){
            if(now>253402300799LL)throw std::invalid_argument("UTC month outside supported Gregorian years");
            const std::chrono::year_month_day date{std::chrono::sys_days{std::chrono::days{day}}};
            const auto first=std::chrono::sys_days{date.year()/date.month()/std::chrono::day{1}};
            return FromStart(first.time_since_epoch().count()*86400,period);
        }
        if(period!=LeaderboardUTCPeriod::Day)throw std::invalid_argument("Unknown UTC period");
        return FromStart(day*86400,period);
    }
    static LeaderboardUTCWindow TodayAt(int64_t now){return At(LeaderboardUTCPeriod::Day,now);}
    int64_t End()const{
        if(!utc_start)throw std::logic_error("Total has no calendar endpoint");
        if(period==LeaderboardUTCPeriod::Month){
            const std::chrono::year_month_day date{std::chrono::sys_days{std::chrono::days{*utc_start/86400}}};
            return std::chrono::sys_days{(date.year()/date.month()+std::chrono::months{1})/std::chrono::day{1}}.time_since_epoch().count()*86400;
        }
        return *utc_start+(period==LeaderboardUTCPeriod::Hour?3600:period==LeaderboardUTCPeriod::Week?7*86400:86400);
    }
    bool Contains(int64_t timestamp)const{return !utc_start||(timestamp>=*utc_start&&timestamp<End());}
    bool operator==(const LeaderboardUTCWindow&)const=default;
};
} // namespace pgn::core
