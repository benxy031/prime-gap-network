#pragma once
#include "social/social_data.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QSaveFile>
#include <QTimeZone>

namespace pgn::social {
namespace leaderboard_cache {
inline constexpr qint64 maxBytes = 512 * 1024;
inline constexpr auto schema = "PGN_PUBLIC_LEADERBOARD_CACHE_V1";
inline constexpr std::array<std::pair<const char*, Counter Miner::*>,6> counters{{
    {"audits", &Miner::validatedAudits}, {"reported", &Miner::ticketsReported},
    {"validated", &Miner::ticketsValidated}, {"positions", &Miner::validatedSearchPositions},
    {"last_height", &Miner::lastActivityHeight}, {"largest_gap", &Miner::largestRecordedGap}
}};
inline constexpr std::array windows{Window::AllTime, Window::Day, Window::Week, Window::Month, Window::Hour};

inline QString path(const QString& directory, const QString& genesis, Window window)
{
    const auto period = leaderboardPeriodName(window);
    if (directory.isEmpty() || period.isEmpty() || !Identity{genesis, "public"}.valid()) return {};
    return QDir(directory).filePath(genesis + "/" + period + ".json");
}

inline QJsonObject read(const QString& filename)
{
    QFile file(filename);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maxBytes) return {};
    const auto bytes = file.read(maxBytes + 1);
    if (bytes.size() > maxBytes) return {};
    return QJsonDocument::fromJson(bytes).object();
}

// Store only the first page of the already validated public table. Wallet
// identity, aliases, roles, pagination and signing metadata are excluded.
inline void save(const QString& directory, const QString& genesis, const Snapshot& view)
{
    const auto filename = path(directory, genesis, view.window);
    if (filename.isEmpty() || view.identity.genesis != genesis || !view.authenticated ||
        view.section != Section::Miners || !view.leaderboardPeriod || !view.observedUtc.isValid()) return;
    const auto& period = *view.leaderboardPeriod;
    QJsonArray rows, available;
    for (const auto window : view.leaderboardWindows) available.append(leaderboardPeriodName(window));
    for (const auto& miner : view.miners) {
        QJsonObject row{{"id", miner.id}, {"name", miner.name}, {"largest_gap_reason", miner.largestGapReason}};
        for (const auto& [key, member] : counters)
            if ((miner.*member).value) row.insert(key, *(miner.*member).value);
        QJsonArray rosettes;
        for (std::size_t i = 0; i < miner.rosettes.size(); ++i) {
            const auto& count = miner.rosettes[i].value;
            rosettes.append(count ? QJsonObject{{"value", *count}} : QJsonObject{{"reason", miner.rosetteReasons[i]}});
        }
        row.insert("rosettes", rosettes);
        rows.append(row);
    }
    const auto observed = view.observedUtc.toMSecsSinceEpoch();
    QJsonObject data{{"schema", schema}, {"genesis", genesis}, {"period", leaderboardPeriodName(view.window)},
                     {"anchor", period.anchor}, {"height", period.height}, {"total", period.total},
                     {"observed_ms", observed}, {"rows", rows}, {"windows", available},
                     {"largest_gap", view.largestGapSupported}};
    if (period.utcStart) data.insert("utc_start", *period.utcStart);
    const auto bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (bytes.size() > maxBytes || !QDir().mkpath(QFileInfo(filename).absolutePath())) return;
    QLockFile lock(filename + ".lock");
    if (!lock.tryLock(250)) return;
    const auto prior = read(filename);
    const auto priorObserved = prior.value("observed_ms").toInteger();
    if (prior.value("schema") == schema && priorObserved > observed &&
        priorObserved <= QDateTime::currentMSecsSinceEpoch()) return;
    QSaveFile file(filename);
    if (file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size()) file.commit();
}

inline std::optional<Snapshot> load(const QString& directory, const QString& genesis, Window window,
                                    const QDateTime& now = QDateTime::currentDateTimeUtc())
{
    const auto filename = path(directory, genesis, window);
    if (filename.isEmpty()) return {};
    const auto data = read(filename);
    const auto observed = data.value("observed_ms").toInteger(-1);
    const auto rows = data.value("rows");
    if (data.value("schema") != schema || data.value("genesis") != genesis ||
        data.value("period") != leaderboardPeriodName(window) || observed <= 0 ||
        observed > now.toMSecsSinceEpoch() || !rows.isArray() || rows.toArray().size() > 100) return {};
    LeaderboardPeriod period{data.value("anchor").toString(), data.value("height").toInteger(-1), {},
                             data.value("total").toInteger(-1)};
    if (data.contains("utc_start")) period.utcStart = data.value("utc_start").toInteger(-1);
    if (!validLeaderboardPeriod(period, window) || period.total < rows.toArray().size()) return {};
    if (period.utcStart) {
        const auto wall = now.toSecsSinceEpoch();
        const auto end = *leaderboardWindowEnd(window, *period.utcStart);
        if (wall < *period.utcStart || wall >= end || observed / 1000 < *period.utcStart || observed / 1000 >= end) return {};
    }
    const auto savedAt = QDateTime::fromMSecsSinceEpoch(observed, QTimeZone::utc());
    const QString source = "Last verified PGN ranking · block " + QString::number(period.height) + " · " + period.anchor;
    const Provenance provenance{genesis, source, savedAt, true};
    Snapshot view;
    view.identity = {genesis, "public"};
    view.section = Section::Miners;
    view.window = window;
    view.observedUtc = savedAt;
    view.authenticated = true;
    view.leaderboardPeriod = period;
    view.source = source;
    view.largestGapSupported = data.value("largest_gap").toBool();
    const auto available = data.value("windows").toArray();
    for (const auto candidate : windows)
        if (available.contains(leaderboardPeriodName(candidate))) view.leaderboardWindows.append(candidate);
    for (const auto& item : rows.toArray()) {
        if (!item.isObject()) return {};
        const auto row = item.toObject();
        Miner miner;
        miner.id = row.value("id").toString();
        miner.name = row.value("name").toString();
        if (!publicLabel(miner.name) || !QRegularExpression("^[0-9a-f]{64}$").match(miner.id).hasMatch()) return {};
        miner.largestGapReason = row.value("largest_gap_reason").toString().left(256);
        for (const auto& [key, member] : counters) {
            const auto value = row.value(key);
            if (value.isString()) miner.*member = {value.toString(), provenance};
            else if (row.contains(key)) return {};
        }
        const auto rosettes = row.value("rosettes").toArray();
        if (rosettes.size() != qsizetype(miner.rosettes.size())) return {};
        for (std::size_t i = 0; i < miner.rosettes.size(); ++i) {
            const auto rosette = rosettes.at(i).toObject();
            if (rosette.value("value").isString()) miner.rosettes[i] = {rosette.value("value").toString(), provenance};
            else miner.rosetteReasons[i] = rosette.value("reason").toString().left(256);
        }
        view.miners.append(std::move(miner));
    }
    if (!validate(view, {Section::Miners, view.identity, window}, savedAt).isEmpty()) return {};
    view.identity.subject.clear();
    view.authenticated = false;
    return view;
}
}
}
