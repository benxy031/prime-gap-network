#include "social_moderation.h"
#include <algorithm>
#include <map>
#include <stdexcept>
#include <utility>

namespace pgn::social::moderation {
namespace {
bool Hex(const std::string& value,size_t length){return value.size()==length&&std::all_of(value.begin(),value.end(),[](unsigned char c){return c>='0'&&c<='9'||c>='a'&&c<='f';});}
std::string Field(const UniValue& value,const char* key){if(!value[key].isStr())throw std::runtime_error("String field required");return value[key].get_str();}
bool Channel(const std::string& value){return !value.empty()&&value.size()<=64&&((value[0]>='a'&&value[0]<='z')||(value[0]>='0'&&value[0]<='9'))&&std::all_of(value.begin(),value.end(),[](char c){return c>='a'&&c<='z'||c>='0'&&c<='9'||c=='/'||c=='_'||c=='-';});}
}
// Same ASCII metadata/safe-integer canonicalization as Core20 pgn/social.cpp.
// Body/reason text is not a control carrier. This first action has no free text.
std::string Canonical(const UniValue& value){
    if(value.isObject()){
        std::map<std::string,UniValue> sorted;const auto& keys=value.getKeys();const auto& entries=value.getValues();
        for(size_t i=0;i<keys.size();++i)if(!sorted.emplace(keys[i],entries[i]).second)throw std::runtime_error("Duplicate canonical key");
        std::string out="{";for(const auto& [key,entry]:sorted){if(out.size()>1)out+=",";out+=UniValue(key).write()+":"+Canonical(entry);}return out+"}";
    }
    if(value.isArray()){std::string out="[";for(const auto& entry:value.getValues()){if(out.size()>1)out+=",";out+=Canonical(entry);}return out+"]";}
    if(value.isStr()){const auto& text=value.get_str();if(!std::all_of(text.begin(),text.end(),[](unsigned char c){return c>=32&&c<=126;}))throw std::runtime_error("Canonical metadata must be ASCII");return value.write();}
    if(value.isNum()){const auto n=value.getInt<int64_t>();if(n<0||n>9007199254740991LL||std::to_string(n)!=value.getValStr())throw std::runtime_error("Safe canonical integer required");return value.write();}
    throw std::runtime_error("Unsupported canonical value");
}
Verifier::Verifier(std::string genesis,Digest digest,ProofVerifier verifier):genesis_(std::move(genesis)),digest_(std::move(digest)),proof_(std::move(verifier)){
    if(!Hex(genesis_,64)||!digest_||!proof_)throw std::invalid_argument("Explicit genesis and native crypto bindings required");
}
VerifiedAction Verifier::Verify(const std::string& message,const std::string& signature,int64_t now_ms,ProofUse use)const{
    if(message.size()<64||message.size()>MAX_PROOF_BYTES||signature.empty()||signature.size()>512)throw std::runtime_error("Bounded moderation proof required");
    const std::string domain="PGN/SOCIAL/MODERATION/1\n";
    if(!message.starts_with(domain))throw std::runtime_error("Moderation domain required");
    UniValue value;if(!value.read(message.substr(domain.size()))||!value.isObject()||Canonical(value)!=message.substr(domain.size()))throw std::runtime_error("Canonical moderation object required");
    const std::set<std::string> expected{"v","genesis","origin","channel","sender_script","signing_address","device","created_ms","nonce","event_id","policy_id","target_event","action"};
    const auto& keys=value.getKeys();if(keys.size()!=expected.size()||std::set<std::string>(keys.begin(),keys.end())!=expected)throw std::runtime_error("Unexpected moderation fields");
    if(!value["v"].isNum()||value["v"].getInt<int>()!=1||Field(value,"genesis")!=genesis_||Field(value,"origin")!="pgn-social://"+genesis_)throw std::runtime_error("Moderation genesis/version mismatch");
    VerifiedAction action;action.genesis=genesis_;action.channel=Field(value,"channel");action.sender_script=Field(value,"sender_script");action.signing_address=Field(value,"signing_address");action.policy_id=Field(value,"policy_id");action.target_event=Field(value,"target_event");action.event_id=Field(value,"event_id");action.nonce=Field(value,"nonce");
    if(Field(value,"action")!="hide")throw std::runtime_error("Unsupported moderation action; no role grant accepted");
    if(!Channel(action.channel)||!Hex(action.sender_script,44)||!action.sender_script.starts_with("0014")||!Hex(action.nonce,64)||!Hex(action.policy_id,64)||!Hex(action.target_event,64)||!Hex(action.event_id,64)||!Hex(Field(value,"device"),32))throw std::runtime_error("Invalid moderation context");
    if(!value["created_ms"].isNum())throw std::runtime_error("Integer timestamp required");action.created_ms=value["created_ms"].getInt<int64_t>();
    if(use==ProofUse::Admission&&action.created_ms>now_ms+60000)throw std::runtime_error("Moderation proof from the future");
    UniValue payload(UniValue::VOBJ);for(const auto& key:keys)if(key!="event_id")payload.pushKV(key,value[key]);
    if(digest_("PGN/SOCIAL/MODERATION-ID/1\n"+Canonical(payload))!=action.event_id)throw std::runtime_error("Moderation content ID mismatch");
    if(!proof_(action.sender_script,action.signing_address,message,signature))throw std::runtime_error("Native BIP322 proof refused");
    action.canonical_message=message;action.signature=signature;return action;
}
Decision Authorize(const PolicySnapshot& state,const VerifiedAction& action,const TargetContext* target){
    if(!state.available)return Decision::Unavailable;
    if(state.genesis!=action.genesis)return Decision::WrongPolicy;
    if(state.conflicted)return Decision::Conflicted;
    if(const auto at=state.ratified.find(action.event_id);at!=state.ratified.end()){
        const auto& mask=at->second;
        if(mask.channel!=action.channel||mask.target_event!=action.target_event)return Decision::WrongTarget;
        if(!target)return Decision::MissingTarget;
        return target->genesis==action.genesis&&target->channel==action.channel&&target->event_id==action.target_event?Decision::AllowedRatified:Decision::WrongTarget;
    }
    if(state.genesis!=action.genesis||state.id!=action.policy_id)return Decision::WrongPolicy;
    if(state.revoked.contains(action.sender_script))return Decision::Revoked;
    if(!state.moderators.contains(action.sender_script))return Decision::NotModerator;
    if(!target)return Decision::MissingTarget;
    if(target->genesis!=action.genesis||target->channel!=action.channel||target->event_id!=action.target_event)return Decision::WrongTarget;
    return Decision::Allowed;
}
const char* Reason(Decision value){switch(value){
    case Decision::Allowed:return "Authorized by current peer policy";
    case Decision::AllowedRatified:return "Historical effect ratified by signed authority checkpoint; no current role granted";
    case Decision::Unavailable:return "Verified moderator policy unavailable";
    case Decision::Conflicted:return "Moderator policy conflict; privileged actions unavailable";
    case Decision::WrongPolicy:return "Policy/genesis changed; refresh before signing";
    case Decision::NotModerator:return "Signing script has no moderator permission";
    case Decision::Revoked:return "Moderator signing script revoked";
    case Decision::MissingTarget:return "Signed target not present; no action applied";
    case Decision::WrongTarget:return "Target does not match the signed network/channel/event";
    }return "Permission denied";}
}
