// Copyright (c) 2026 The PGN developers. MIT.
#pragma once
#include <memory>
class ChainstateManager;
namespace pgn::core {
struct NativeLeaderboardSnapshot;
// Read-only projection of the existing Total cache, including every verified
// signing-script binding. Callers retain the immutable anchor and re-check its
// canonical membership when consuming asynchronous results. This is not a
// wallet identity oracle and grants no permissions.
std::shared_ptr<const NativeLeaderboardSnapshot>
ReadNativeSocialIdentitySnapshot(ChainstateManager& manager);
}
