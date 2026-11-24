#undef NDEBUG
// The published testnet must stay byte identical, and a suffix must build a
// separate chain from the same policies. Both halves are checked here because a
// regression in either direction breaks one of the two chains.
#include <pgn/testnet_definition.h>
#include <util/translation.h>
#include <iostream>
#include <stdexcept>
#include <string_view>
extern const TranslateFn G_TRANSLATION_FUN=[](const char* text){return std::string(text);};
using namespace pgn::core;
static void need(bool yes,const char* reason){if(!yes)throw std::runtime_error(reason);}
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
 std::cout<<"local chain definition ok"<<std::endl;return 0;
}catch(const std::exception&e){std::cerr<<"local chain definition failed: "<<e.what()<<std::endl;return 1;}}
