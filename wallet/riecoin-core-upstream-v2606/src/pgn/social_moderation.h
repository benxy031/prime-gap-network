// Private candidate; no public bootstrap or feature activation.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <univalue.h>

namespace pgn::social::moderation {
inline constexpr size_t MAX_PROOF_BYTES=24576;
enum class ProofUse { Admission, StoredHistory };
using Digest=std::function<std::string(const std::string&)>;
// Production binding MUST derive the address from script and call the existing
// native MessageVerifyBIP322. A permissive/missing callback is never a default.
using ProofVerifier=std::function<bool(const std::string& script,const std::string& address,const std::string& message,const std::string& signature)>;
std::string Canonical(const UniValue&);
struct PolicySnapshot;
struct TargetContext;
enum class Decision;

class VerifiedAction {
    friend class Verifier;
    friend Decision Authorize(const PolicySnapshot&,const VerifiedAction&,const TargetContext*);
    VerifiedAction()=default;
    std::string genesis,channel,sender_script,signing_address,policy_id,target_event,event_id,nonce;
    std::string canonical_message,signature;
    int64_t created_ms{};
public:
    const std::string& Genesis()const{return genesis;}
    const std::string& Channel()const{return channel;}
    const std::string& SenderScript()const{return sender_script;}
    const std::string& PolicyId()const{return policy_id;}
    const std::string& TargetEvent()const{return target_event;}
    const std::string& EventId()const{return event_id;}
    const std::string& Nonce()const{return nonce;}
    const std::string& CanonicalMessage()const{return canonical_message;}
    const std::string& Signature()const{return signature;}
    int64_t CreatedMs()const{return created_ms;}
};
class Verifier {
    std::string genesis_;
    Digest digest_;
    ProofVerifier proof_;
public:
    Verifier(std::string genesis,Digest digest,ProofVerifier verifier);
    VerifiedAction Verify(const std::string& message,const std::string& signature,int64_t now_ms,ProofUse use=ProofUse::Admission)const;
};

// INTERNAL trusted projection, never populated from an RPC caller's roles or
// display name. Its signed-policy loader/transition is a separate obligation.
// The first component test supplies an explicitly synthetic trust anchor.
struct PolicySnapshot {
    struct RatifiedMask {std::string channel,target_event,action_id;};
    std::string genesis,id;
    uint64_t generation{};
    std::set<std::string> moderators,revoked;
    bool available{},conflicted{};
    std::map<std::string,RatifiedMask> ratified;
};
struct TargetContext {std::string genesis,channel,event_id;};
enum class Decision { Allowed,AllowedRatified,Unavailable,Conflicted,WrongPolicy,NotModerator,Revoked,MissingTarget,WrongTarget };
Decision Authorize(const PolicySnapshot&,const VerifiedAction&,const TargetContext*);
const char* Reason(Decision);
// Authorize is called again under the same commit serialization used for
// policy changes and the durable batch. Signature-cache success alone cannot
// bypass it, including on retry or stored-history reconstruction.
}
