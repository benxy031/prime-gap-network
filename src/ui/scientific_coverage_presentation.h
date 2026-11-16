#pragma once
#include <QJsonObject>
#include <QString>
namespace pgn::ui {
struct ScientificCoveragePresentation { QString text, tooltip; bool available{}; QString richText; };
ScientificCoveragePresentation scientificCoveragePresentation(const QJsonObject& snapshot);
}
