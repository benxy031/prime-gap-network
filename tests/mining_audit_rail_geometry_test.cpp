#include "ui/mining_cycle_progress.h"
#ifdef PGN_WIDGET_GEOMETRY
#include "ui/mining_ticket_progress.h"
#include <QApplication>
#include <QImage>
#endif

#include <cmath>
#include <iostream>

using pgn::ui::CycleProgress;
using pgn::ui::miningCycleProgress;

namespace {
int failures = 0;

void check(bool valid, const char* label) {
    if (valid) return;
    ++failures;
    std::cerr << "FAIL " << label << '\n';
}

bool near(double a, double b) { return std::abs(a - b) < 1e-12; }

template<class Progress>
std::optional<double> displayMarker(const Progress& progress) {
    if constexpr (requires { progress.markerFraction; }) return progress.markerFraction;
    return progress.fraction;
}

QJsonObject ref(char id, int slot) {
    return {{"request", QString(64, '9')}, {"audit_id", QString(64, id)},
            {"slot", slot}, {"attempt", 1}};
}

QJsonObject sample(QJsonArray regular, int index, int done, bool search,
                   QString auditId = {}, QJsonArray extras = {},
                   bool catchUp = false, QString active = {}) {
    QJsonArray ids;
    for (const auto& extra : extras) ids.append(extra.toObject().value("audit_id"));
    return {
        {"phase", "Searching"}, {"work_role", search ? "search" : "audit"},
        {"ticket_id", search ? QString(64, 'e') : auditId},
        {"positions_total", "20"}, {"positions", QString::number(done)},
        {"audit_service", QJsonObject{{"cycle", QJsonObject{
            {"schema", "PGN_MINING_CYCLE_VIEW_V1"}, {"request_id", QString(64, 'd')},
            {"ticket_number", "2607"}, {"active_index", index}, {"search_positions", "20"},
            {"physical_audits", true}, {"audits", regular}, {"catch_up", catchUp},
            {"active_audit_id", active.isEmpty() ? auditId : active},
            {"extra_audit_ids", ids}, {"extra_audit_refs", extras}
        }}}}
    };
}
}

int main(int argc, char** argv) {
#ifdef PGN_WIDGET_GEOMETRY
    QApplication app(argc, argv);
#else
    (void)argc;
    (void)argv;
#endif
    const QJsonObject absent{{"state", "unavailable"}};
    const QJsonObject pending{{"state", "pending"}};
    const auto ref1 = ref('a', 0);
    const auto ref2 = ref('b', 1);
    const auto extra = ref('c', 0);
    const auto id1 = ref1.value("audit_id").toString();
    const auto id2 = ref2.value("audit_id").toString();
    const auto extraId = extra.value("audit_id").toString();
    const QJsonObject assigned1{{"state", "assigned"}, {"audit_ref", ref1}};
    const QJsonObject assigned2{{"state", "assigned"}, {"audit_ref", ref2}};
    const QJsonObject committed2{{"state", "committed"}, {"audit_ref", ref2}};

    const auto second = miningCycleProgress(sample({absent, assigned2}, 1, 10, false, id2));
    check(second.slices.size() == 3, "slot 0 waiver retains two regular slots");
    check(second.slices[0].skipped && !second.slices[0].pendingAssignment,
          "slot 0 waiver has its own presentation");
    check(second.slices[1].active && second.phase == "Audit 2/2", "active slot uses exact index 1");
    check(near(displayMarker(second).value_or(-1), .075), "slot 1 midpoint is .075");
    check(near(second.fraction, .025), "waiver does not fabricate computed progress");

    const auto first = miningCycleProgress(sample({assigned1, absent}, 0, 10, false, id1));
    check(first.slices.size() == 3 && first.slices[0].active && first.slices[1].skipped,
          "slot 1 waiver preserves active slot 0");
    check(near(displayMarker(first).value_or(-1), .025), "slot 0 midpoint is .025");
    const auto searching = miningCycleProgress(sample({absent, committed2}, 2, 0, true));
    check(near(displayMarker(searching).value_or(-1), .1), "search starts after both regular slots");

    const auto preAssignment = miningCycleProgress(sample({pending, assigned2}, 1, 10, false, id2));
    check(preAssignment.slices[0].pendingAssignment && !preAssignment.slices[0].skipped,
          "pending remains distinct from unavailable");
    check(near(displayMarker(preAssignment).value_or(-1), .075), "pending preserves the same geometry");

    const auto catchUp = miningCycleProgress(sample({absent, committed2}, 0, 10, false, extraId, {extra}, true));
    check(catchUp.slices.size() == 4 && catchUp.slices[0].skipped &&
          catchUp.slices[2].role == "catch_up" && catchUp.slices[2].active,
          "two regular slots and one exact catch-up remain separate");
    check(near(displayMarker(catchUp).value_or(-1), .125 / 1.05), "catch-up midpoint is inside its own section");
    const auto afterCatchUp = miningCycleProgress(sample({absent, committed2}, 2, 0, true, {}, {extra}));
    check(near(displayMarker(afterCatchUp).value_or(-1), .15 / 1.05), "search follows all three audit sections");

    const auto noAudit = miningCycleProgress(sample({absent, absent}, 2, 0, true));
    check(noAudit.slices.size() == 3 && near(displayMarker(noAudit).value_or(-1), .1),
          "both waived slots retain width in the first search frame");

    const auto foreign = miningCycleProgress(sample({absent, assigned2}, 1, 10, false, id1, {}, false, id2));
    check(!displayMarker(foreign).has_value(), "a mismatched active attempt cannot move the point");
    const auto displaced = miningCycleProgress(sample({assigned1, assigned2}, 1, 10, false, id1));
    check(!displayMarker(displaced).has_value(), "a matching identity cannot override the active index");

#ifdef PGN_WIDGET_GEOMETRY
    pgn::ui::MiningTicketProgress rail;
    rail.resize(992, 120);
    const auto markerAt=[&](double fraction) {
        QImage image(rail.size(), QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::black);
        rail.render(&image);
        return image.pixelColor(qRound(16. + 960. * fraction),84) == QColor("#69efc5");
    };
    rail.present(sample({absent, assigned2}, 1, 10, false, id2));
    check(markerAt(.075) && !markerAt(.025), "native widget draws the point in slot 1");
    check(rail.toolTip().contains("Audit 1/2 skipped because no eligible independent work was available."),
          "native widget explains why the first slot was waived");
    rail.present(sample({absent, committed2}, 2, 0, true));
    check(markerAt(.1), "native widget draws the first search point after both audit slots");
    rail.present(sample({absent, committed2}, 0, 10, false, extraId, {extra}, true));
    check(markerAt(.125 / 1.05), "native widget draws the active catch-up point in the third audit slot");
#endif
    if (failures) return 1;
    std::cout << "PASS: waived slots 0 and 1, pending, catch-up, search transition and exact active identity\n";
}
