#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>
#include <deque>
#include <utility>

namespace pgn::wallet {
class CoreRpcDiagnosticHistory {
    static constexpr std::size_t Limit=48;
    static constexpr std::size_t FirstFailureLimit=5;
    struct Entry { QJsonObject event; bool firstFailure; };
    std::deque<Entry> entries_;
    std::size_t firstFailures_{};
public:
    void append(QJsonObject event){
        const auto outcome=event.value("outcome");
        const bool preserve=event.value("channel")=="inspection"&&outcome.isString()
            &&!outcome.toString().isEmpty()&&outcome!="ok"&&firstFailures_<FirstFailureLimit
            &&std::none_of(entries_.begin(),entries_.end(),[&](const Entry& entry){
                return entry.firstFailure&&entry.event.value("outcome")==outcome;
            });
        if(entries_.size()==Limit){
            // The bounded first failures remain available to late exports.
            const auto oldest=std::find_if(entries_.begin(),entries_.end(),[](const Entry& entry){
                return !entry.firstFailure;
            });
            entries_.erase(oldest);
        }
        entries_.push_back({std::move(event),preserve});
        if(preserve)++firstFailures_;
    }
    QJsonArray snapshot()const{
        QJsonArray result;
        for(const auto& entry:entries_)result.append(entry.event);
        return result;
    }
};
}
