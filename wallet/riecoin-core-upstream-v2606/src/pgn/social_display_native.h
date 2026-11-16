// Copyright (c) 2026 The PGN developers. MIT; see COPYING.
#pragma once
#include <pgn/social_display_store.h>
class ChainstateManager;
namespace pgn::social::display {
void SetChainman(ChainstateManager*);
Context NativeContext();
std::string NativeTip();
}

