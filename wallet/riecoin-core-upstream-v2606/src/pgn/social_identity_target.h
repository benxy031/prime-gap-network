// Copyright (c) 2026 The PGN developers. MIT.
#pragma once
#include <pgn/social_identity_index.h>
#include <list>
#include <mutex>
#include <map>
#include <memory>

namespace pgn::social::identity::committed {
struct TargetResolution {
    std::string state,reason,social_script;
    CandidateAnchor anchor;std::optional<State> association;
    std::optional<std::string> display_name;
    size_t checked{},invalid{};bool complete{};
};
// Core-local reader only. Snapshot/reader/canonical must come from the Native
// provider, never a client-supplied author, username, alias or JSON snapshot.
class TargetResolver {
public:
    using ReadBlock=std::function<std::optional<pgn::core::VerifiedNativeRequestBlock>(const CandidateRef&)>;
private:
    struct Slot {
        Projection projection;std::string after;uint64_t generation{};
        CandidateAnchor completed,requested;uint64_t revision{};bool busy{};
    };
    CandidateIndex& index_;const size_t hot_limit_;
    std::map<std::string,std::shared_ptr<Slot>> hot_;std::list<std::string> lru_;
    std::mutex mutex_;
    std::shared_ptr<Slot> Acquire(const std::string& s,uint64_t generation){
        if(auto found=hot_.find(s);found!=hot_.end()){
            auto at=std::find(lru_.begin(),lru_.end(),s);if(at!=lru_.end())lru_.splice(lru_.end(),lru_,at);return found->second;
        }
        while(hot_.size()>=hot_limit_){
            auto victim=std::find_if(lru_.begin(),lru_.end(),[&](const auto& key){return !hot_.at(key)->busy;});
            if(victim==lru_.end())return {};
            hot_.erase(*victim);lru_.erase(victim);
        }
        auto slot=std::make_shared<Slot>();slot->generation=generation;lru_.push_back(s);try{hot_.emplace(s,slot);}catch(...){lru_.pop_back();throw;}return slot;
    }
public:
    TargetResolver(CandidateIndex& index,size_t hot_limit):index_(index),hot_limit_(hot_limit){Require(hot_limit>0&&hot_limit<=512,"Explicit bounded hot cache required");}
    TargetResolution Advance(const std::string& s,const pgn::core::NativeLeaderboardSnapshot& native,
        const ReadBlock& reader,const std::function<bool(const uint256&,int32_t)>& canonical,size_t proof_limit){
        Require(Hex(s,44)&&s.starts_with("0014")&&reader&&canonical&&proof_limit>0&&proof_limit<=100,"Bounded Native target request required");
        TargetResolution reply;reply.social_script=s;reply.anchor={native.height,native.tip};
        const auto generation=index_.Generation();std::shared_ptr<Slot> slot;Slot work;uint64_t revision{};
        {
            std::lock_guard lock(mutex_);slot=Acquire(s,generation);
            if(!slot||slot->busy){reply.state="pending";reply.reason="This target is queued; unrelated targets remain available";return reply;}
            if(slot->generation!=generation||slot->requested.height>native.height){slot->projection=Projection{};slot->after.clear();slot->generation=generation;slot->completed={};slot->requested={};++slot->revision;}
            work=*slot;revision=slot->revision;slot->busy=true;
        }
        try{
            if(!canonical(native.tip,native.height))throw AssociationContextUnavailable("Target Native anchor is not canonical");
            const auto page=index_.Read(s,reply.anchor,work.after,proof_limit,canonical);
            if(page.generation!=generation)throw AssociationContextUnavailable("Target index generation changed");
            for(const auto& reference:page.refs){
                // Archive failure is unavailable, never an invalid signature.
                const auto block=reader(reference);
                if(!block||block->Block()!=reference.block||block->Height()!=reference.height||block->Chain()!=native.genesis)
                    throw AssociationContextUnavailable("Authenticated target block is unavailable");
                try{
                    const auto event=Verify(native,*block,reference.output,canonical);
                    if(event.SocialScript()!=s)throw AssociationContextUnavailable("Candidate script differs from authenticated body");
                    work.projection.Apply(event);
                }catch(const InvalidAssociation&){++reply.invalid;}
                ++reply.checked;auto height=std::to_string(uint32_t(reference.height)),output=std::to_string(reference.output);
                work.after=std::string(10-height.size(),'0')+height+std::string(10-output.size(),'0')+output;
            }
            if(!canonical(native.tip,native.height)||index_.Generation()!=generation)
                throw AssociationContextUnavailable("Target context changed before publication");
            if(page.complete){
                work.completed=reply.anchor;reply.complete=true;
                if(const auto* state=work.projection.Find(s)){
                    reply.association=*state;reply.display_name=CurrentName(*state,native,canonical);
                    reply.state=reply.display_name?"linked":state->active?"unavailable":"unlinked";
                    if(state->active&&!reply.display_name)reply.reason="Current Native attribution or public label unavailable";
                }else reply.state="unlinked";
            }else{reply.state="pending";reply.reason="Target proof history is still being verified";}
            {
                std::lock_guard lock(mutex_);
                if(slot->revision!=revision||index_.Generation()!=generation)
                    throw AssociationContextUnavailable("Target cache generation changed");
                slot->projection=std::move(work.projection);slot->after=std::move(work.after);slot->completed=work.completed;
                slot->requested=reply.anchor;slot->generation=generation;slot->busy=false;++slot->revision;
            }
            return reply;
        }catch(const std::exception& error){
            // No partial page/projection/cursor is published after I/O, memory
            // or Native-context failure. Retry reads exactly the same page.
            std::lock_guard lock(mutex_);slot->busy=false;
            reply.state="unavailable";reply.reason=error.what();reply.complete=false;reply.association.reset();reply.display_name.reset();return reply;
        }
    }
    size_t HotEntries(){std::lock_guard lock(mutex_);return hot_.size();}
};
// Cache capacity is not an admission quota. CDB remains paged by S; restart
// reconstructs only requested targets, not a new full-chain scan per message.
// Persisted verified checkpoints/GC, Native RPC and Qt wiring remain gates.
}
