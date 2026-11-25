#undef NDEBUG
// The published testnet must stay byte identical, and a suffix must build a
// separate chain from the same policies. Both halves are checked here because a
// regression in either direction breaks one of the two chains.
#include <pgn/testnet_definition.h>
#include <pgn/gold_epoch_policy.h>
#include <util/translation.h>
#include <iostream>
#include <stdexcept>
#include <string_view>
extern const TranslateFn G_TRANSLATION_FUN=[](const char* text){return std::string(text);};
using namespace pgn::core;
static void need(bool yes,const char* reason){if(!yes)throw std::runtime_error(reason);}
// The rewards blob is the only place the split between a miner and its auditors is
// committed, so it is read straight back out of the genesis a chain would run.
static std::optional<GoldEpochDefinition> EpochRules(const CBlock& genesis){
 opcodetype op;Bytes tag,payload;
 for(const auto& output:genesis.vtx.front()->vout){
  auto cursor=output.scriptPubKey.begin();
  if(!output.scriptPubKey.GetOp(cursor,op)||op!=OP_RETURN||
     !output.scriptPubKey.GetOp(cursor,op,tag)||tag!=Bytes{GoldEpochPolicyTag.begin(),GoldEpochPolicyTag.end()}||
     !output.scriptPubKey.GetOp(cursor,op,payload)||cursor!=output.scriptPubKey.end())continue;
  return DecodeGoldEpochPolicy(payload);}
 return {};}
int main(){try{
 const auto published=BuildPgnTestnetDefinition();
 const auto empty=BuildPgnTestnetDefinition(std::string_view{});
 need(published.pinned_genesis==empty.pinned_genesis&&published.magic==empty.magic,
      "Default argument is not an empty suffix");
 const auto local=BuildPgnTestnetDefinition("phase0");
 need(local.pinned_genesis!=published.pinned_genesis,"Suffix did not change the genesis");
 need(local.genesis.GetHash()==local.pinned_genesis,"Local genesis hash is not pinned to itself");
 need(NativeMainParameterDefinitionValid(local),"Local chain definition is invalid");
 need(local.magic!=published.magic,"Local chain shares the published network magic");
 need(local.peer_port!=published.peer_port&&local.rpc_port!=published.rpc_port,
      "Local chain shares the published default ports");
 need(local.addresses.hrp==published.addresses.hrp,"Local chain changed the address policy");
 const auto other=BuildPgnTestnetDefinition("phase1");
 need(other.pinned_genesis!=local.pinned_genesis,"Two suffixes produced the same chain");
 need(other.magic!=local.magic,"Two suffixes produced the same network magic");
 // The auditor earns a quarter of the miner's share on a counted chain, and it has
 // to hold in every era - halving applies to both sides, so the ratio cannot drift.
 const auto local_rules=EpochRules(local.genesis);
 need(local_rules.has_value(),"Local genesis lost its rewards policy");
 const auto per_gold=[](const GoldEpochGrant& g){return g.gold+g.silver+g.bronze+g.red+g.audit;};
 for(uint64_t golds:{uint64_t{0},uint64_t{1},uint64_t{5},uint64_t{9},
                     uint64_t{local_rules->era_golds},uint64_t{2*local_rules->era_golds+3}}){
  const auto grant=QuoteGoldEpoch(*local_rules,golds);
  need(grant.has_value(),"Local rewards quote failed");
  need(grant->audit*4==grant->gold,"The auditor's share is not a quarter of the miner's");}

 // The published chain keeps the split it committed, which is why its genesis is
 // untouched: 171 to the miner and an audit pot that is not a quarter of it.
 const auto published_rules=EpochRules(published.genesis);
 need(published_rules.has_value(),"Published genesis lost its rewards policy");
 const auto published_grant=QuoteGoldEpoch(*published_rules,0);
 need(published_grant.has_value(),"Published rewards quote failed");
 need(published_grant->gold==171*PGN_ATOMIC_UNITS_PER_COIN,"The published miner share moved");
 need(published_grant->audit!=published_grant->gold/4,
      "The published chain silently adopted the counted split");

 // The split moved money inside the same budget, it did not create any.
 for(uint64_t golds:{uint64_t{0},uint64_t{7},uint64_t{local_rules->era_golds+2}}){
  const auto counted_grant=QuoteGoldEpoch(*local_rules,golds);
  const auto published_grant_at=QuoteGoldEpoch(*published_rules,golds);
  need(counted_grant.has_value()&&published_grant_at.has_value(),"Rewards quote failed");
  need(per_gold(*counted_grant)==per_gold(*published_grant_at),
       "The per-gold budget changed instead of its split");}
 std::cout<<"local chain definition ok"<<std::endl;return 0;
}catch(const std::exception&e){std::cerr<<"local chain definition failed: "<<e.what()<<std::endl;return 1;}}
