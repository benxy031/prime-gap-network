// Copyright (c) 2026 The PGN developers
// Distributed under the MIT software license; see COPYING.
#pragma once
#include <pgn/social_moderation.h>
#include <functional>
#include <memory>
#include <optional>
#include <util/fs.h>

namespace pgn::social {
// Private qualification only. No production trust anchor or quorum is selected.
// The implementation refuses every privileged operation without a regtest-only
// explicit synthetic anchor. No display name is an input to authorization.
class AccessStore {
    class Impl;
    std::unique_ptr<Impl> impl;
public:
    using Lookup=std::function<std::optional<moderation::TargetContext>(const std::string&)>;
    AccessStore(const fs::path& directory,Lookup);
    ~AccessStore();
    void Load();
    void Commit(const std::string& message,const std::string& signature);
    UniValue Read(uint64_t cursor,unsigned limit=1);
    bool Hidden(const std::string& channel,const std::string& event);
    std::string Root();
};
bool IsAccessProof(const std::string& message);
bool PrivateQualificationEnabled();
}
