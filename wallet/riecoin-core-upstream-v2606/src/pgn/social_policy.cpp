#include "social_policy.h"
#include <algorithm>
#include <stdexcept>

namespace pgn::social::moderation {
namespace {
bool Hex(const std::string& s,size_t length){return s.size()==length&&std::all_of(s.begin(),s.end(),[](unsigned char c){return(c>='0'&&c<='9')||(c>='a'&&c<='f');});}
std::string Text(const UniValue& v,const char* key){if(!v[key].isStr())throw std::runtime_error("Policy string required");return v[key].get_str();}
bool Script(const std::string& s){return Hex(s,44)&&s.starts_with("0014");}
bool Channel(const std::string& s){return !s.empty()&&s.size()<=64&&((s[0]>='a'&&s[0]<='z')||(s[0]>='0'&&s[0]<='9'))&&std::all_of(s.begin(),s.end(),[](char c){return(c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='/'||c=='_'||c=='-';});}
void Keys(const UniValue& v,const std::set<std::string>& expected){const auto& k=v.getKeys();if(k.size()!=expected.size()||std::set<std::string>(k.begin(),k.end())!=expected)throw std::runtime_error("Unexpected policy fields");}
bool SameMask(const PolicySnapshot::RatifiedMask& a,const PolicySnapshot::RatifiedMask& b){return a.action_id==b.action_id&&a.target_event==b.target_event&&a.channel==b.channel;}
}
PolicyVerifier::PolicyVerifier(std::string genesis,Digest digest,ProofVerifier proof):genesis_(std::move(genesis)),digest_(std::move(digest)),proof_(std::move(proof)){if(!Hex(genesis_,64)||!digest_||!proof_)throw std::invalid_argument("Explicit policy crypto/genesis bindings required");}
PolicyDocument PolicyVerifier::ParseDocument(const UniValue& value)const{
    if(!value.isObject())throw std::runtime_error("Policy object required");
    Keys(value,{"v","genesis","origin","scope","epoch","previous_policy","moderators","threshold","ratified_masks"});
    const auto canonical=Canonical(value);if(canonical.size()>MAX_POLICY_BYTES)throw std::runtime_error("Policy exceeds transport document budget");
    if(value["v"].getInt<int>()!=1||Text(value,"genesis")!=genesis_||Text(value,"origin")!="pgn-social://"+genesis_||Text(value,"scope")!="social")throw std::runtime_error("Policy context mismatch");
    PolicyDocument document;document.genesis=genesis_;document.canonical=canonical;document.id=digest_("PGN/SOCIAL/POLICY-ID/1\n"+canonical);document.previous=Text(value,"previous_policy");
    if(!Hex(document.previous,64)||!value["epoch"].isNum()||!value["threshold"].isNum()||!value["moderators"].isArray()||!value["ratified_masks"].isArray())throw std::runtime_error("Invalid policy structure");
    document.epoch=value["epoch"].getInt<uint64_t>();const auto threshold=value["threshold"].getInt<uint64_t>();
    std::string previous;
    for(const auto& entry:value["moderators"].getValues()){if(!entry.isStr()||!Script(entry.get_str())||(!previous.empty()&&entry.get_str()<=previous))throw std::runtime_error("Sorted distinct moderator scripts required");previous=entry.get_str();document.moderators.insert(previous);}
    if(!threshold||threshold>document.moderators.size())throw std::runtime_error("Explicit valid threshold required");document.threshold=static_cast<unsigned>(threshold);
    previous.clear();
    for(const auto& entry:value["ratified_masks"].getValues()){
        if(!entry.isObject())throw std::runtime_error("Mask object required");Keys(entry,{"action_id","channel","target_event"});
        PolicySnapshot::RatifiedMask mask{Text(entry,"channel"),Text(entry,"target_event"),Text(entry,"action_id")};
        if(!Hex(mask.action_id,64)||!Hex(mask.target_event,64)||!Channel(mask.channel)||(!previous.empty()&&mask.action_id<=previous))throw std::runtime_error("Sorted distinct ratified masks required");
        previous=mask.action_id;document.ratified.emplace(mask.action_id,std::move(mask));
    }
    return document;
}
PolicyAttestation PolicyVerifier::Verify(const std::string& message,const std::string& signature,int64_t now_ms,ProofUse use)const{
    const std::string domain="PGN/SOCIAL/POLICY-ATTEST/1\n";
    if(message.size()>MAX_PROOF_BYTES||signature.empty()||signature.size()>512||!message.starts_with(domain))throw std::runtime_error("Bounded policy attestation required");
    UniValue envelope;if(!envelope.read(message.substr(domain.size()))||!envelope.isObject()||Canonical(envelope)!=message.substr(domain.size()))throw std::runtime_error("Canonical policy attestation required");
    Keys(envelope,{"v","genesis","origin","sender_script","signing_address","device","created_ms","nonce","event_id","policy"});
    const auto sender=Text(envelope,"sender_script"),address=Text(envelope,"signing_address");
    if(envelope["v"].getInt<int>()!=1||Text(envelope,"genesis")!=genesis_||Text(envelope,"origin")!="pgn-social://"+genesis_||!Script(sender)||!Hex(Text(envelope,"device"),32)||!Hex(Text(envelope,"nonce"),64)||!Hex(Text(envelope,"event_id"),64))throw std::runtime_error("Policy attestation context mismatch");
    const auto created=envelope["created_ms"].getInt<int64_t>();if(use==ProofUse::Admission&&created>now_ms+60000)throw std::runtime_error("Policy attestation from future");
    auto policy=std::make_shared<const PolicyDocument>(ParseDocument(envelope["policy"]));
    UniValue payload(UniValue::VOBJ);for(const auto& key:envelope.getKeys())if(key!="event_id")payload.pushKV(key,envelope[key]);
    if(digest_("PGN/SOCIAL/POLICY-ATTEST-ID/1\n"+Canonical(payload))!=Text(envelope,"event_id"))throw std::runtime_error("Policy attestation content ID mismatch");
    if(!proof_(sender,address,message,signature))throw std::runtime_error("Native policy BIP322 proof refused");
    return {std::move(policy),sender,message,signature,Text(envelope,"nonce"),Text(envelope,"event_id"),created};
}
PolicyTracker::PolicyTracker(PolicyDocument bootstrap):current_(std::move(bootstrap)){
    if(current_.epoch!=0||current_.moderators.empty()||!current_.threshold||current_.threshold>current_.moderators.size()||current_.previous!=std::string(64,'0'))throw std::invalid_argument("Explicit epoch-zero trust anchor required");
}
PolicyResult PolicyTracker::Observe(const PolicyAttestation& attestation){
    if(!attestation.policy)return PolicyResult::Rejected;
    const auto& candidate=*attestation.policy;
    if(candidate.genesis!=current_.genesis)return PolicyResult::Rejected;
    if(candidate.id==current_.id)return PolicyResult::AlreadyApplied;
    const PolicyDocument* parent=nullptr;
    if(candidate.epoch==current_.epoch+1&&candidate.previous==current_.id)parent=&current_;
    else if(previous_&&candidate.epoch==current_.epoch&&candidate.previous==previous_->id)parent=&*previous_;
    if(!parent||!parent->moderators.contains(attestation.sender_script))return PolicyResult::Rejected;
    if(conflict_)return PolicyResult::Conflicted; // Authorized evidence, no implicit fork resolution.
    // First tranche is revocation-only. Rotation/recovery must add new-key
    // consent, not silently accept a different script or a matching username.
    for(const auto& script:candidate.moderators)if(!parent->moderators.contains(script))return PolicyResult::Rejected;
    for(const auto& [id,mask]:parent->ratified){const auto at=candidate.ratified.find(id);if(at==candidate.ratified.end()||!SameMask(mask,at->second))return PolicyResult::Rejected;}
    votes_.insert_or_assign(attestation.sender_script,attestation);
    std::set<std::string> old_signers,new_signers;std::vector<PolicyAttestation> certificate;
    for(const auto& [script,vote]:votes_)if(vote.policy->id==candidate.id){old_signers.insert(script);if(candidate.moderators.contains(script))new_signers.insert(script);certificate.push_back(vote);}
    if(old_signers.size()<parent->threshold||new_signers.size()<candidate.threshold)return PolicyResult::Pending;
    if(candidate.epoch==current_.epoch){conflict_=true;++generation_;return PolicyResult::Conflicted;}
    previous_=current_;current_=candidate;certificate_=std::move(certificate);conflict_=false;++generation_;
    // Retain the previous epoch's distinct signed certificate for detecting a
    // certified fork; memory remains proportional to the moderator set.
    return PolicyResult::Applied;
}
PolicySnapshot PolicyTracker::Snapshot()const{
    PolicySnapshot out{current_.genesis,current_.id,generation_,current_.moderators,{},true,conflict_,current_.ratified};
    if(previous_)for(const auto& script:previous_->moderators)if(!current_.moderators.contains(script))out.revoked.insert(script);
    return out;
}
}
