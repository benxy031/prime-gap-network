// Private Native component candidate. No public bootstrap, RPC or activation.
// V2 is persistent only through verified canonical coinbase state; V1 leases
// retain their original expiry. Reuses Native BIP322 and authenticated archive.
#pragma once
#include <pgn/social_identity_contract.h>
#include <pgn/native_leaderboard.h>
#include <pgn/ticket_request_confirmation.h>
#include <functional>
#include <optional>

namespace pgn::social::identity::committed {
inline const std::string Tag="PGN-SOCIAL-LINK-2";
inline const std::string Prefix="PGN/SOCIAL/IDENTITY-LINK";
inline std::string EventId(const UniValue& statement){return Hash(Prefix+"-ID/2\n"+Canonical(statement));}
inline std::string Message(const UniValue& statement,const std::string& side){auto v=Obj();v.pushKV("event_id",EventId(statement));v.pushKV("statement",statement);return Prefix+"/"+side+"/2\n"+Canonical(v);}
inline CScript Envelope(const std::string& source){return CScript{}<<OP_RETURN<<std::vector<unsigned char>(Tag.begin(),Tag.end())<<std::vector<unsigned char>(source.begin(),source.end());}
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
class InvalidAssociation:public std::runtime_error {public:using std::runtime_error::runtime_error;};
class AssociationContextUnavailable:public std::runtime_error {public:using std::runtime_error::runtime_error;};
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
    UniValue value;Require(value.read(source)&&value.isObject()&&Canonical(value)==source,"Canonical association certificate required");
    Fields(value,{"statement","social_proof","operator_proof","authorizer_script","authorization_proof"});const auto& statement=value["statement"];
    Fields(statement,{"v","genesis","origin","scope","action","social_script","operator_script","operator_id","anchor_hash","anchor_height","previous_link","nonce"});
    VerifiedEvent event;event.source=source;event.id=EventId(statement);event.action=statement["action"].get_str();event.social_script=statement["social_script"].get_str();event.operator_script=statement["operator_script"].get_str();event.operator_id=statement["operator_id"].get_str();event.previous=statement["previous_link"].get_str();event.anchor=statement["anchor_hash"].get_str();event.anchor_height=statement["anchor_height"].getInt<int32_t>();event.authorizer=value["authorizer_script"].get_str();event.position={block.Height(),output,block.Block()};
    Require(statement["v"].getInt<int>()==2&&statement["genesis"].get_str()==native.genesis.GetHex()&&statement["origin"].get_str()=="pgn-social://"+native.genesis.GetHex()&&statement["scope"].get_str()==Scope,"Persistent association version/network/scope mismatch");
    const auto script_ok=[](const std::string& s){return Hex(s,44)&&s.starts_with("0014");};
    Require(script_ok(event.social_script)&&script_ok(event.operator_script)&&Hex(event.operator_id,64)&&Hex(event.previous,64)&&Hex(statement["nonce"].get_str(),64),"Persistent association identity bound");
    const auto anchor=uint256::FromHex(event.anchor);Require(anchor&&anchor->GetHex()==event.anchor&&event.anchor_height>=0&&event.anchor_height<block.Height(),"Association must refer to a known prior canonical Native anchor");
    if(event.action=="bind"){
        Require(event.authorizer.empty()&&value["authorization_proof"].get_str().empty(),"Initial/new binding uses two explicit consents");
        // Committed history is authenticated at source and inclusion. Current
        // membership is checked separately by CurrentName; otherwise a later
        // enrollment would erase the restart lineage and prevent withdrawal.
        const auto raw=ParseHex(event.operator_script);const CScript key{raw.begin(),raw.end()};
        for(const auto height:{event.anchor_height,block.Height()})Require(pgn::core::LeaderboardOperatorAt(native.bindings,key,height).GetHex()==event.operator_id,"Native operator mismatch at historical source/commit anchor");
        Require(std::any_of(native.rows.begin(),native.rows.end(),[&](const auto& row){return row.operator_id.GetHex()==event.operator_id;}),"Recognized productive Native operator required");
        VerifyProof(event.social_script,value["social_proof"].get_str(),Message(statement,"SOCIAL-CONSENT"));VerifyProof(event.operator_script,value["operator_proof"].get_str(),Message(statement,"OPERATOR-CONSENT"));
    }else if(event.action=="withdraw"){
        Require(event.previous!=std::string(64,'0')&&(event.authorizer==event.social_script||event.authorizer==event.operator_script)&&value["social_proof"].get_str().empty()&&value["operator_proof"].get_str().empty(),"Withdrawal needs a current participant transition proof");
        VerifyProof(event.authorizer,value["authorization_proof"].get_str(),Message(statement,"STATE-TRANSITION"));
    }else throw std::runtime_error("Unsupported persistent association action");

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
