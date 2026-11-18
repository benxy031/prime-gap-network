#pragma once
#include <QJsonArray>
#include <QNetworkReply>
#include <QJsonObject>
#include <algorithm>
#include <deque>
#include <utility>

namespace pgn::wallet {
class CoreRpcDiagnosticHistory {
    static constexpr std::size_t Limit=48;
    static constexpr std::size_t FirstFailureLimit=5;
    struct Entry { QJsonObject event; bool firstFailure; bool firstLinkTimeout; };
    std::deque<Entry> entries_;
    std::size_t firstFailures_{};
public:
    void append(QJsonObject event){
        const auto outcome=event.value("outcome");
        const bool inspection=event.value("channel")=="inspection";
        const bool preserve=inspection&&outcome.isString()
            &&!outcome.toString().isEmpty()&&outcome!="ok"&&firstFailures_<FirstFailureLimit
            &&std::none_of(entries_.begin(),entries_.end(),[&](const Entry& entry){
                return entry.firstFailure&&entry.event.value("outcome")==outcome;
            });
        // Connection-count and chain reads can produce the same timeout.
        // Keep the first timeout for each method that validates the Core link.
        const auto method=event.value("method");
        const bool linkTimeout=inspection&&(method=="getblockchaininfo"||method=="getblockhash")
            &&(outcome=="total_timeout"||(outcome=="transport_error"
                &&event.value("transport_code")==int(QNetworkReply::TimeoutError)));
        const bool preserveLinkTimeout=linkTimeout
            &&std::none_of(entries_.begin(),entries_.end(),[&](const Entry& entry){
                return entry.firstLinkTimeout&&entry.event.value("method")==method;
            });
        if(entries_.size()==Limit){
            // At most five first failures and two chain timeouts are retained.
            const auto oldest=std::find_if(entries_.begin(),entries_.end(),[](const Entry& entry){
                return !entry.firstFailure&&!entry.firstLinkTimeout;
            });
            entries_.erase(oldest);
        }
        entries_.push_back({std::move(event),preserve,preserveLinkTimeout});
        if(preserve)++firstFailures_;
    }
    QJsonArray snapshot()const{
        QJsonArray result;
        for(const auto& entry:entries_)result.append(entry.event);
        return result;
    }
};
}
