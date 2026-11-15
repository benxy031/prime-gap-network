// Copyright (c) 2026 The PGN developers. MIT.
// Read-only statistics of disclosed records; never reward eligibility or coins.
#pragma once
#include <pgn/merit_arithmetic.h>
#include <univalue.h>
#include <array>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace pgn::core {
struct LeaderboardRosetteTotals {
    std::array<uint64_t,5> counts{}; // Q independently of exclusive G/S/B/R.
    std::array<bool,5> complete{true,true,true,true,true};
    UniValue Json()const{
        static constexpr const char* names[]{"q","g","s","b","r"};
        UniValue out{UniValue::VOBJ};
        for(size_t i=0;i<5;++i){UniValue item{UniValue::VOBJ};item.pushKV("complete",complete[i]);
            if(complete[i])item.pushKV("count",std::to_string(counts[i]));
            else item.pushKV("reason","Disclosed record, threshold or exact classification unavailable");
            out.pushKV(names[i],item);
        }
        return out;
    }
};
static_assert(sizeof(LeaderboardRosetteTotals)<=56,"Keep persistent per-miner medal summaries compact");
struct LeaderboardRosettes:LeaderboardRosetteTotals {
    std::set<std::pair<Integer,Integer>> seen;
    void Add(size_t index){
        if(counts[index]==std::numeric_limits<uint64_t>::max())complete[index]=false;
        else ++counts[index];
    }
    static std::optional<Integer> Decimal(const UniValue& value) {
        if(!value.isStr())return {};
        const auto text=value.get_str();
        if(text.empty()||text.size()>80||(text.size()>1&&text[0]=='0'))return {};
        Integer out{};for(const char c:text){if(c<'0'||c>'9')return {};out=out*10+(c-'0');}
        return out;
    }
    void Unavailable(size_t first=0){for(size_t i=first;i<complete.size();++i)complete[i]=false;}
    // Call chronologically across ALL qualified reports, including those
    // outside the requested time window, so a replay cannot earn a second row.
    // The caller authenticates report inclusion, owner, root and qualification.
    template<class Hash> void Observe(std::string_view disclosure,std::string_view root,
                                      bool in_window,Hash&& hash) {
        if(disclosure.size()>65536||root.size()!=64||hash(disclosure)!=root){if(in_window)Unavailable();return;}
        UniValue body;
        if(!body.read(std::string{disclosure})||!body.isObject()||
           !body["schema"].isStr()||body["schema"].get_str()!="PGN_NETWORK_RANGE_REPORT_V1"||
           !body["computed"].isObject()||!body["computed"]["best"].isArray()){
            if(in_window)Unavailable();return;
        }
        const auto& records=body["computed"]["best"];
        if(records.size()>100){if(in_window)Unavailable();return;}
        const auto numerator=Decimal(body["target_numerator"]),denominator=Decimal(body["target_denominator"]);
        const bool target_ok=numerator&&denominator&&*denominator>0&&*numerator>3*(*denominator);
        std::set<std::pair<Integer,Integer>> local;
        for(const auto& record:records.getValues()){
            const auto lower=Decimal(record["lower_prime"]),upper=Decimal(record["upper_prime"]);
            if(!lower||!upper||*lower<3||*upper<=*lower||!record["gap"].isNum()){
                if(in_window)Unavailable();return;
            }
            const auto gap=record["gap"].getInt<int64_t>();
            if(gap<=0||gap>(int64_t{1}<<32)||gap%2||*lower%2!=1||*upper!=*lower+gap||
               !local.emplace(*lower,Integer{gap}).second){if(in_window)Unavailable();return;}
        }
        std::erase_if(local,[&](const auto& key){return !seen.insert(key).second;});
        if(!in_window||local.empty())return;
        // ln is monotone. These two directed endpoints decide every ordinary
        // record in a narrow scientific range; only an interval-straddling
        // record needs its own logarithm. This is exact, never smoothing.
        const auto minimum=naturalLogBounds(local.begin()->first,{192,128},{1024,256,256});
        const auto maximum=local.begin()->first==local.rbegin()->first?minimum:
            naturalLogBounds(local.rbegin()->first,{192,128},{1024,256,256});
        for(const auto& key:local){
            std::optional<LogBounds> exact;
            const auto compare=[&](const Integer& n,const Integer& d){
                const Integer value=(key.second*d)<<minimum.fractional_bits;
                if(value>=n*maximum.upper)return MeritDecision::Meets;
                if(value<n*minimum.lower)return MeritDecision::Below;
                if(!exact)exact=naturalLogBounds(key.first,{192,128},{1024,256,256});
                if(value>=n*exact->upper)return MeritDecision::Meets;
                if(value<n*exact->lower)return MeritDecision::Below;
                return MeritDecision::Indeterminate;
            };
            const auto q=compare(Integer{40},Integer{1});
            if(q==MeritDecision::Meets)Add(0);
            else if(q==MeritDecision::Indeterminate)complete[0]=false;
            if(!target_ok){Unavailable(1);continue;}
            for(size_t tier=0;tier<4;++tier){
                const auto verdict=compare(*numerator-Integer{tier}*(*denominator),*denominator);
                if(verdict==MeritDecision::Meets){Add(tier+1);break;}
                if(verdict==MeritDecision::Indeterminate){Unavailable(1);break;}
            }
        }
    }
};
} // namespace pgn::core
