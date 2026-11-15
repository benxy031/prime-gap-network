// MIT. Explicit, compiled existing-chain grouping schedule. No CLI activation.
#pragma once
#include <uint256.h>
#include <optional>
namespace pgn::core {
struct AuditGroupingActivation {uint256 genesis;int32_t height;};
inline std::optional<AuditGroupingActivation> ConfiguredAuditGroupingActivation(const uint256& chain){
    (void)chain;
    // QUALIFICATION_PIN: public schedules remain absent until qualified.
    return {};
}
inline bool AuditGroupingScheduled(const uint256& chain,int64_t height){
    const auto activation=ConfiguredAuditGroupingActivation(chain);
    return activation&&!activation->genesis.IsNull()&&activation->genesis==chain&&activation->height>0&&height>=activation->height;
}
}
