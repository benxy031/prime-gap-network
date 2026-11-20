#pragma once

#include <QByteArray>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <optional>

namespace pgn::wallet {

inline constexpr qint64 CoreBundleTemplateLimit = 32768;

inline std::optional<QByteArray> readCoreBundleTemplate(const QString& root) {
    QFile file(root + "/Core/core-template.json");
    if (!file.exists()) return std::nullopt;
    if (!file.open(QIODevice::ReadOnly)) return QByteArray{};
    return file.read(CoreBundleTemplateLimit + 1);
}

inline QJsonObject parseCoreBundleTemplate(const QByteArray& contents) {
    if (contents.size() > CoreBundleTemplateLimit) return {};
    QJsonParseError error;
    const auto document = QJsonDocument::fromJson(contents, &error);
    if (error.error != QJsonParseError::NoError || !document.isObject()) return {};
    const auto root = document.object();
    if (root.value("schema") != "PGN_CORE_BUNDLE_V1" || !root.value("deployment").isObject())
        return {};
    return root;
}

}
