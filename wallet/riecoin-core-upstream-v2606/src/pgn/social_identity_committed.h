// Private Native component candidate. No public bootstrap, RPC or activation.
// V2 is persistent only through verified canonical coinbase state; V1 leases
// retain their original expiry. Reuses Native BIP322 and authenticated archive.
#pragma once
#include <pgn/social_identity_certificate.h>
#include <pgn/native_leaderboard.h>
#include <pgn/ticket_request_confirmation.h>
#include <functional>
#include <optional>

namespace pgn::social::identity::committed {
struct Position {int32_t height{};uint32_t output{};uint256 block;};
class VerifiedEvent {
    friend VerifiedEvent Verify(const pgn::core::NativeLeaderboardSnapshot&,const pgn::core::VerifiedNativeRequestBlock&,uint32_t,const std::function<bool(const uint256&,int32_t)>&);
    friend class Projection;
    std::string id,source,action,social_script,operator_script,operator_id,previous,anchor,authorizer;
    int32_t anchor_height{};Position position;
public:
    const std::string& Id()const{return id;}
    const Position& At()const{return position;}
    const std::string& SocialScript()const{return social_script;}
};
class TargetResolver;
inline VerifiedEvent Verify(const pgn::core::NativeLeaderboardSnapshot& native,const pgn::core::VerifiedNativeRequestBlock& block,uint32_t output,const std::function<bool(const uint256&,int32_t)>& canonical){
    if(!canonical||block.Chain()!=native.genesis||native.height<block.Height()||!canonical(native.tip,native.height)||!canonical(block.Block(),block.Height()))
        throw AssociationContextUnavailable("Canonical Native context required");
    if(!block.Coinbase()||output>=block.Coinbase()->vout.size())
        throw AssociationContextUnavailable("Verified Native reference unavailable");
    // Only pure certificate parsing/crypto is converted to InvalidAssociation.
    // Native I/O/canonical-context failures and allocation failures propagate.
    const auto event=[&]()->VerifiedEvent{
        try{
    const auto& txout=block.Coinbase()->vout[output];Require(txout.nValue==0&&txout.scriptPubKey.size()<=MaxFrame,"Bounded zero-valued association output required");
    const auto& script=txout.scriptPubKey;auto cursor=script.begin();opcodetype op;std::vector<unsigned char> tag,payload;
    Require(script.GetOp(cursor,op)&&op==OP_RETURN&&script.GetOp(cursor,op,tag)&&std::string(tag.begin(),tag.end())==Tag&&script.GetOp(cursor,op,payload)&&cursor==script.end(),"Exact association envelope required");
    const std::string source(payload.begin(),payload.end());Require(script==Envelope(source),"Canonical association script required");
    const auto certificate=DecodeConsentCertificate(source,native.genesis.GetHex());
    VerifiedEvent event;event.source=certificate.source;event.id=certificate.id;event.action=certificate.action;event.social_script=certificate.social_script;event.operator_script=certificate.operator_script;event.operator_id=certificate.operator_id;event.previous=certificate.previous;event.anchor=certificate.anchor;event.anchor_height=certificate.anchor_height;event.authorizer=certificate.authorizer;event.position={block.Height(),output,block.Block()};
    Require(event.anchor_height<block.Height(),"Association must refer to a known prior canonical Native anchor");
    if(event.action=="bind"){
        const auto raw=ParseHex(event.operator_script);const CScript key{raw.begin(),raw.end()};
        for(const auto height:{event.anchor_height,block.Height()})Require(pgn::core::LeaderboardOperatorAt(native.bindings,key,height).GetHex()==event.operator_id,"Native operator mismatch at historical source/commit anchor");
        Require(std::any_of(native.rows.begin(),native.rows.end(),[&](const auto& row){return row.operator_id.GetHex()==event.operator_id;}),"Recognized productive Native operator required");
    }
    CheckConsentProofs(certificate,true);
            return event;
        }catch(const std::bad_alloc&){throw;}
        catch(const std::exception& error){throw InvalidAssociation(error.what());}
    }();
    const auto anchor=*uint256::FromHex(event.anchor);
    if(!canonical(native.tip,native.height)||!canonical(block.Block(),block.Height()))
        throw AssociationContextUnavailable("Native branch changed during verification");
    if(!canonical(anchor,event.anchor_height))
        throw InvalidAssociation("Association source anchor is not canonical");
    if(!canonical(native.tip,native.height)||!canonical(block.Block(),block.Height()))
        throw AssociationContextUnavailable("Native branch changed before verification publication");
    return event;
}
struct State {std::string social_script,operator_script,operator_id,head,anchor;int32_t anchor_height{};Position committed;bool active{};};
enum class ApplyResult {Applied,AlreadyCurrent,StaleParent,WrongParticipant,OutOfOrder};
class Projection {
    friend class TargetResolver;
    std::map<std::string,State> states_;std::optional<Position> last_;
    int32_t processed_height_{-1};uint256 processed_hash_;
    ApplyResult Apply(const VerifiedEvent& event){
        const auto known=states_.find(event.social_script);
        if(known!=states_.end()&&known->second.head==event.id)return ApplyResult::AlreadyCurrent;
        if(last_&&(event.position.height<last_->height||(event.position.height==last_->height&&event.position.output<=last_->output)))return ApplyResult::OutOfOrder;
        last_=event.position;
        if(known==states_.end()){
            if(event.action!="bind"||event.previous!=std::string(64,'0'))return ApplyResult::StaleParent;
        }else{
            if(event.previous!=known->second.head)return ApplyResult::StaleParent;
            if(event.action=="withdraw"&&(!known->second.active||event.operator_script!=known->second.operator_script||event.operator_id!=known->second.operator_id))return ApplyResult::WrongParticipant;
            if(event.action=="bind"&&known->second.active)return ApplyResult::StaleParent;
        }
        states_.insert_or_assign(event.social_script,State{event.social_script,event.operator_script,event.operator_id,event.id,event.anchor,event.anchor_height,event.position,event.action=="bind"});return ApplyResult::Applied;
    }
public:
    struct BlockResult {size_t applied{},duplicate{},stale{},invalid{};};
    // Whole authenticated coinbases only. P2P arrival order cannot choose which
    // child wins within a block. Reorg requires a new canonical replay instance.
    BlockResult ApplyBlock(const pgn::core::NativeLeaderboardSnapshot& native,const pgn::core::VerifiedNativeRequestBlock& block,const std::function<bool(const uint256&,int32_t)>& canonical){
        Require(block.Height()==processed_height_+1&&(processed_height_<0||canonical(processed_hash_,processed_height_)),"Contiguous canonical association replay required");
        Require(canonical(native.tip,native.height)&&canonical(block.Block(),block.Height()),"Canonical Native block required before association replay");
        BlockResult result;std::vector<VerifiedEvent> events;
        for(uint32_t output=0;output<block.Coinbase()->vout.size();++output){
            const auto& script=block.Coinbase()->vout[output].scriptPubKey;auto cursor=script.begin();opcodetype op;std::vector<unsigned char> tag;
            if(!script.GetOp(cursor,op)||op!=OP_RETURN||!script.GetOp(cursor,op,tag)||std::string(tag.begin(),tag.end())!=Tag)continue;
            try{events.push_back(Verify(native,block,output,canonical));}catch(const InvalidAssociation&){++result.invalid;}
        }
        Require(canonical(native.tip,native.height)&&canonical(block.Block(),block.Height())&&(processed_height_<0||canonical(processed_hash_,processed_height_)),"Native branch changed during association verification");
        for(const auto& event:events){const auto applied=Apply(event);if(applied==ApplyResult::Applied)++result.applied;else if(applied==ApplyResult::AlreadyCurrent)++result.duplicate;else ++result.stale;}
        processed_height_=block.Height();processed_hash_=block.Block();return result;
    }
    const State* Find(const std::string& s)const{const auto it=states_.find(s);return it==states_.end()?nullptr:&it->second;}
};
inline std::optional<std::string> CurrentName(const State& state,const pgn::core::NativeLeaderboardSnapshot& native,const std::function<bool(const uint256&,int32_t)>& canonical){
    const auto anchor=uint256::FromHex(state.anchor);if(!canonical||!state.active||!anchor||state.committed.height>native.height||!canonical(native.tip,native.height)||!canonical(state.committed.block,state.committed.height)||!canonical(*anchor,state.anchor_height))return {};
    const auto raw=ParseHex(state.operator_script);const CScript key{raw.begin(),raw.end()};if(pgn::core::LeaderboardOperatorAt(native.bindings,key,native.height).GetHex()!=state.operator_id)return {};
    for(const auto& row:native.rows)if(row.operator_id.GetHex()==state.operator_id)return row.display_name;return {};
}
} // namespace pgn::social::identity::committed
