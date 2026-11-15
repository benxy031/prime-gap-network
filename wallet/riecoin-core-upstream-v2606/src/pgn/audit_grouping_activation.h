// MIT. Explicit, compiled existing-chain grouping schedule. No CLI activation.
#pragma once
#include <uint256.h>
#include <optional>
namespace pgn::core {
struct AuditGroupingActivation {uint256 genesis;int32_t height;};
inline std::optional<AuditGroupingActivation> ConfiguredAuditGroupingActivation(const uint256& chain){
    // Epoch3 maintenance: ordinary amounts and old receipts remain exact.
    const auto epoch3=*uint256::FromHex("710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314");
    if(chain==epoch3)return AuditGroupingActivation{epoch3,4096};
    return {};
}
inline bool AuditGroupingScheduled(const uint256& chain,int64_t height){
    const auto activation=ConfiguredAuditGroupingActivation(chain);
    return activation&&!activation->genesis.IsNull()&&activation->genesis==chain&&activation->height>0&&height>=activation->height;
}
}
