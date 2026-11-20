#include "ui/mining_cycle_progress.h"
#include "ui/mining_results_view.h"
#include "ui/mining_ticket_progress.h"
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QTableWidget>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

void verify(const QJsonArray& frames, QSize size) {
    QTemporaryDir root;
    require(root.isValid(), "test wallet-free data directory");
    QWidget host;
    host.resize(size);
    pgn::ui::MiningTicketProgress rail(&host);
    rail.setGeometry(0, 0, size.width(), 110);
    auto* results = pgn::ui::createMiningResultsView(root.path(), &host);
    results->setGeometry(0, 110, size.width(), size.height() - 110);
    rail.onWorkSelected = [results](const QJsonObject& frame) {
        pgn::ui::presentMiningTicketData(results, frame);
    };
    host.show();
    auto* best = results->findChild<QTableWidget*>("pgn-results-best");
    auto* recent = results->findChild<QTableWidget*>("pgn-results-recent");
    require(best && recent, "real result widgets absent");
    for (const auto& entry : frames) {
        const auto step = entry.toObject();
        const auto input = step.value("frame").toObject();
        const auto projected = pgn::ui::currentAssignmentPresentation(input);
        rail.present(projected);
        QApplication::processEvents();
        QImage image(rail.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        rail.render(&image);
        const auto expected = step.value("positions").toString();
        if (projected.value("positions").toString() != expected)
            throw std::runtime_error("Progress or identity discarded at " + step.value("step").toString().toStdString());
        if (expected.isEmpty()) {
            require(!projected.contains("ticket_id"), "old assignment was retained");
            require(!rail.property("pgn_ticket_progress_fraction").isValid(), "old progress remains visible");
        } else {
            require(projected.value("ticket_id") == input.value("ticket_id"), "audit identity changed");
            require(rail.property("pgn_ticket_progress_fraction").isValid(), "rail progress is blank for the current audit");
        }
        if (step.contains("cycle_fraction")) {
            const auto observed = rail.property("pgn_ticket_cycle_fraction");
            require(observed.isValid(), "cycle progress is blank at the archive handoff");
            require(std::abs(observed.toDouble() - step.value("cycle_fraction").toDouble()) < 1e-12,
                    "whole ticket progress changed at the archive handoff");
        }
        const int rows = step.value("rows").toInt();
        const auto populated = [](const QTableWidget* table) {
            int count = 0;
            for (int row = 0; row < table->rowCount(); ++row)
                if (const auto* cell = table->item(row, 2); cell && !cell->text().isEmpty()) ++count;
            return count;
        };
        require(populated(best) == rows && populated(recent) == rows, "rendered result rows were dropped or duplicated");
        if (rows) {
            require(results->property("pgn_results_points").toInt() == step.value("count").toInt(),
                    "selected-scope total changed at an internal boundary");
            require(best->item(0, 2)->text() == step.value("top").toString(), "largest result changed unexpectedly");
        }
        if (step.value("boundary").toBool()) {
            require(projected.value("source_stale") == QJsonValue(true), "missing observation treated as fresh");
            require(projected.value("speed_rate").isNull() && projected.value("eta_rate").isNull(),
                    "boundary displayed live rates without a sample");
            require(!rail.property("pgn_ticket_cycle_eta_seconds").isValid(), "boundary invented cycle ETA");
        }
    }
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("PGN-Isolated-Partition-QA");
    QApplication::setApplicationName("AuditPartitionWidgets");
    QStandardPaths::setTestModeEnabled(true);
    try {
        require(argc == 2, "expected the actual producer frame fixture");
        QFile file(QString::fromLocal8Bit(argv[1]));
        require(file.open(QIODevice::ReadOnly), "cannot read audit fixture");
        const auto frames = QJsonDocument::fromJson(file.readAll()).array();
        require(frames.size() >= 8, "audit fixture misses transitions");
        verify(frames, {1366, 768});
        verify(frames, {1060, 600});
        std::cout << "PASS " << frames.size() << " producer frames at both sizes\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
