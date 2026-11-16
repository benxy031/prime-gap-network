// Copyright (c) 2026 The PGN developers. MIT; see COPYING.
#include <pgn/social_display_native.h>
#include <pgn/social_identity_snapshot.h>
#include <pgn/native_leaderboard.h>
#include <validation.h>
#include <sync.h>
#include <atomic>
namespace pgn::social::display {
namespace {std::atomic<ChainstateManager*> native_manager{nullptr};}
void SetChainman(ChainstateManager* manager){native_manager.store(manager);}
std::string NativeTip(){
 auto* manager=native_manager.load();if(!manager)return {};
 LOCK(cs_main);const auto* tip=manager->ActiveChain().Tip();return tip?tip->GetBlockHash().GetHex():std::string{};
}
Context NativeContext(){
 auto* manager=native_manager.load();if(!manager)throw ContextUnavailable("Native display source has not started");
 std::shared_ptr<const pgn::core::NativeLeaderboardSnapshot> snapshot;
 try{snapshot=pgn::core::ReadNativeSocialIdentitySnapshot(*manager);}
 catch(const UniValue&){throw ContextUnavailable("Authenticated Native name projection not ready");}
 catch(const std::exception&){throw ContextUnavailable("Authenticated Native name projection unavailable");}
 if(!snapshot)throw ContextUnavailable("Native display snapshot missing");
 Context out;out.genesis=snapshot->genesis.GetHex();out.tip=snapshot->tip.GetHex();out.height=snapshot->height;
 out.canonical=[manager](int32_t height,const std::string& hash){
  LOCK(cs_main);const auto* block=manager->ActiveChain()[height];return block&&block->GetBlockHash().GetHex()==hash;
 };
 // Immutable same-anchor source; no username matching or per-message scan.
 out.native=[snapshot](const std::string& raw){
  const auto data=ParseHex(raw);const CScript script{data.begin(),data.end()};
  const auto op=pgn::core::LeaderboardOperatorAt(snapshot->bindings,script,snapshot->height);
  const auto row=std::find_if(snapshot->rows.begin(),snapshot->rows.end(),[&](const auto& value){return value.operator_id==op;});
  return std::pair{op.GetHex(),row!=snapshot->rows.end()&&row->display_name?*row->display_name:std::string{}};
 };
 if(!out.canonical(out.height,out.tip))throw ContextUnavailable("Native display snapshot reorganized");
 return out;
}
}

