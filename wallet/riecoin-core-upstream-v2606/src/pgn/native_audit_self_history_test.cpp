#include "native_audit_history_test_fixture.h"

int main(int argc, char** argv) {
    return RunNativeAuditHistoryFixture(argc, argv, NativeAuditHistoryScenario::Self);
}
