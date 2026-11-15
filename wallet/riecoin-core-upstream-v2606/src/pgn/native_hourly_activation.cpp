#include <pgn/native_hourly_activation.h>
namespace pgn::core {
std::optional<NativeHourlyMeritActivation> ConfiguredNativeHourlyMerit(const uint256& chain) {
    static const auto epoch3=*uint256::FromHex("710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314");
    if(chain==epoch3)return NativeHourlyMeritActivation{epoch3,1794614400};
    return std::nullopt;
}
}
