// Copyright (c) 2026 The PGN developers. MIT.
// One immutable testnet schedule shared by work service and audit settlement.
#pragma once
#include <uint256.h>
#include <optional>
namespace pgn::core {
struct TestnetServiceActivation {
    uint256 genesis,monetary_policy;
    static constexpr int32_t height=64;
    static constexpr int32_t first_assignment_height=0;
    static constexpr uint16_t page_outputs=128;
};
inline std::optional<TestnetServiceActivation> PgnTestnetServiceActivation(const uint256& chain){
    static const auto genesis=*uint256::FromHex("0ccd1f2349ed75f9cf4d0279871a204e107683e5baa1dd6641c4e6f08a25a17d");
    static const auto money=*uint256::FromHex("89135cb022c8afb18cc0df2581d1ef8e97df0d7fbc4be3700ca4c49c7fbf3e4a");
    static const auto epoch3=*uint256::FromHex("710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314");
    static const auto epoch3_money=*uint256::FromHex("6621129976cc09e887e88dac20fd43f55da0e7ef28787ec7396375d2c5b3619c");
    if(chain==epoch3)return TestnetServiceActivation{epoch3,epoch3_money};
    if(chain!=genesis)return {};
    return TestnetServiceActivation{genesis,money};
}
}
