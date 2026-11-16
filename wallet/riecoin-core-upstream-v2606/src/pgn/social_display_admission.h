#pragma once
#include <string_view>
namespace pgn::social {
// An ambiguous durable write may have changed the index even when appended is
// false. Re-read after uncertainty; ordinary known-proof replays stay cached.
inline bool DisplayAdmissionInvalidates(bool appended,std::string_view state){return appended||state=="uncertain";}
}
