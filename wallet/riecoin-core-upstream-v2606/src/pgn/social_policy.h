#pragma once
#include "social_moderation.h"
#include <memory>
#include <optional>
#include <vector>
#include <utility>

namespace pgn::social::moderation {
inline constexpr size_t MAX_POLICY_BYTES=16384;
struct PolicyDocument {
    std::string genesis,id,previous;
    uint64_t epoch{};
    unsigned threshold{};
    std::set<std::string> moderators;
    std::map<std::string,PolicySnapshot::RatifiedMask> ratified;
    std::string canonical;
};
class PolicyAttestation {
    friend class PolicyVerifier;
    friend class PolicyTracker;
    std::shared_ptr<const PolicyDocument> policy;
    std::string sender_script,canonical_message,signature,nonce,event_id;
    int64_t created_ms{};
    PolicyAttestation(std::shared_ptr<const PolicyDocument> document,std::string sender,std::string message,std::string sig,std::string request_nonce,std::string event,int64_t created):policy(std::move(document)),sender_script(std::move(sender)),canonical_message(std::move(message)),signature(std::move(sig)),nonce(std::move(request_nonce)),event_id(std::move(event)),created_ms(created){}
public:
    const PolicyDocument& Document()const{return *policy;}
    const std::string& SenderScript()const{return sender_script;}
    const std::string& CanonicalMessage()const{return canonical_message;}
    const std::string& Signature()const{return signature;}
    const std::string& Nonce()const{return nonce;}
    const std::string& EventId()const{return event_id;}
    int64_t CreatedMs()const{return created_ms;}
};
class PolicyVerifier {
    std::string genesis_;Digest digest_;ProofVerifier proof_;
public:
    PolicyVerifier(std::string genesis,Digest digest,ProofVerifier verifier);
    // Parsing alone is not trust. Only the private regtest bootstrap loader
    // may establish an initial anchor, and attested transitions extend it.
    PolicyDocument ParseDocument(const UniValue&)const;
    PolicyAttestation Verify(const std::string&,const std::string&,int64_t now_ms,ProofUse use=ProofUse::Admission)const;
};
enum class PolicyResult {Pending,Applied,AlreadyApplied,Rejected,Conflicted};
class PolicyTracker {
    PolicyDocument current_;
    std::optional<PolicyDocument> previous_;
    bool conflict_{};
    uint64_t generation_{1};
    // One latest vote per current authority script, never a global user cap.
    // Unknown scripts cannot allocate a proposal or evict another signer's vote.
    std::map<std::string,PolicyAttestation> votes_;
    std::vector<PolicyAttestation> certificate_;
public:
    explicit PolicyTracker(PolicyDocument explicitly_trusted_bootstrap);
    PolicyResult Observe(const PolicyAttestation&);
    PolicySnapshot Snapshot()const;
    const PolicyDocument& Current()const{return current_;}
    const std::vector<PolicyAttestation>& Certificate()const{return certificate_;}
    bool Conflicted()const{return conflict_;}
    // Persistence is performed by the Native adapter's atomic batch BEFORE
    // publishing this staged tracker. Observe itself does not imply durability.
};
}
