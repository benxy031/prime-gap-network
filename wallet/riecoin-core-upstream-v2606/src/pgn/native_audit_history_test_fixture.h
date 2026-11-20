#pragma once

enum class NativeAuditHistoryScenario { Self, Retained };

int RunNativeAuditHistoryFixture(int argc, char** argv, NativeAuditHistoryScenario scenario);
