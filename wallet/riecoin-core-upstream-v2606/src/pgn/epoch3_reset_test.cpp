#undef NDEBUG
#include <pgn/testnet_definition.h>
#include <pgn/base_main_definition.h>
#include <pgn/testnet_service_activation.h>
#include <pgn/audit_operator_policy.h>
#include <pgn/native_hourly_activation.h>
#include <pgn/profile_policy_codec_internal.h>
#include <util/translation.h>
#include <iostream>
#include <stdexcept>
extern const TranslateFn G_TRANSLATION_FUN=[](const char* text){return std::string(text);};
using namespace pgn::core;
static void need(bool yes,const char* reason){if(!yes)throw std::runtime_error(reason);}
int main(){try{
 const auto epoch=BuildPgnTestnetDefinition();
 need(NativeMainParameterDefinitionValid(epoch),"Invalid native epoch definition");
 need(epoch.pinned_genesis.GetHex()=="710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314","Native genesis differs from independently reconstructed wire");
 need(epoch.magic==MessageStartChars{0xd7,0x54,0x4e,0x33}&&epoch.peer_port==48951&&epoch.rpc_port==48952,"Epoch network isolation changed");
 const auto target=ReadPinnedNativeRetarget(epoch.genesis,epoch.pinned_genesis,epoch.policy);
 need(target.policy&&target.policy->initial_q16==1802240&&target.policy->minimum_q16==1802240,"27.5 initial/floor not pinned in genesis");
 std::optional<ScienceVerificationDefinition> science;
 for(const auto& output:epoch.genesis.vtx.front()->vout){auto at=output.scriptPubKey.begin();opcodetype op;Bytes tag,payload;
  if(output.scriptPubKey.GetOp(at,op)&&op==OP_RETURN&&output.scriptPubKey.GetOp(at,op,tag)&&tag==Bytes{SciencePolicyEnvelopeTag.begin(),SciencePolicyEnvelopeTag.end()}&&output.scriptPubKey.GetOp(at,op,payload))
   science=science_policy_codec::Decode(payload,{0,epoch.policy.maximum_payload_bytes,epoch.policy.maximum_integer_bytes,0});
 }
 need(science&&science->evidence.target_numerator*65536==Integer{target.policy->initial_q16}*science->evidence.target_denominator,"Scientific proof threshold differs from the genesis controller");
 const auto hourly=ConfiguredNativeHourlyMerit(epoch.pinned_genesis);
 need(hourly&&hourly->genesis==epoch.pinned_genesis&&hourly->median_time==1794614400,"Hourly rule not bound to fresh epoch");
 need(PgnTestnetServiceActivation(epoch.pinned_genesis).has_value(),"Fresh epoch service unavailable");
 Bytes monetary_payload;
 for(const auto& output:epoch.genesis.vtx.front()->vout){auto at=output.scriptPubKey.begin();opcodetype op;Bytes tag,payload;
  if(output.scriptPubKey.GetOp(at,op)&&op==OP_RETURN&&output.scriptPubKey.GetOp(at,op,tag)&&tag==Bytes{MonetaryPolicyEnvelopeTag.begin(),MonetaryPolicyEnvelopeTag.end()}&&output.scriptPubKey.GetOp(at,op,payload))monetary_payload=payload;
 }
 need(!monetary_payload.empty(),"Pinned monetary payload unavailable");
 HashWriter monetary{TaggedHash("PGN/CANDIDATE/NATIVE-MONETARY-POLICY/1")};monetary<<epoch.pinned_genesis<<epoch.pinned_genesis<<monetary_payload;
 need(PgnTestnetServiceActivation(epoch.pinned_genesis)->monetary_policy==monetary.GetHash(),"Audit settlement activation belongs to another epoch policy");
 const auto own=ConfiguredNativeAuditOperatorRules(epoch.pinned_genesis);
 need(own&&own->height==1,"Self rechecks not active from fresh epoch");
 const auto old=*uint256::FromHex("0ccd1f2349ed75f9cf4d0279871a204e107683e5baa1dd6641c4e6f08a25a17d");
 need(!ConfiguredNativeHourlyMerit(old)&&ConfiguredNativeAuditOperatorRules(old)->height==16640,"Old history was reinterpreted");
 const auto main=BuildBaseMainDefinition();
 need(!PgnTestnetServiceActivation(main.pinned_genesis)&&!ConfiguredNativeHourlyMerit(main.pinned_genesis)&&!ConfiguredNativeAuditOperatorRules(main.pinned_genesis),"Mainnet rule contamination");
 std::cout<<"{\"PASS\":true,\"fresh_genesis\":\"710b38654faa9f45a759ef589e818839dcc775ceebe16f7830a306ed6f8fd314\",\"initial_and_floor\":27.5,\"hourly_enabled\":true,\"old_history_unchanged\":true,\"mainnet_unchanged\":true}\n";
 return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
