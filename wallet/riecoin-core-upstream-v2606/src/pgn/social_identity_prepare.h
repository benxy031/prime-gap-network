// Copyright (c) 2026 The PGN developers. MIT.
#pragma once
#include <pgn/social_identity_committed.h>
namespace pgn::social::identity::committed {
inline void CheckConsentNativeContext(const ConsentCertificate& proof,const pgn::core::NativeLeaderboardSnapshot& native,const std::function<bool(const uint256&,int32_t)>& canonical,bool require_current=true){
    if(!canonical||!canonical(native.tip,native.height))throw AssociationContextUnavailable("Current Native source is unavailable");
    const auto anchor=uint256::FromHex(proof.anchor);
    if(!anchor||proof.anchor_height>native.height)throw AssociationContextUnavailable("Consent source anchor is ahead of available Native context");
    if(!canonical(*anchor,proof.anchor_height))throw AssociationContextReset("Consent source anchor was reorganized; keep the packet and recheck canonical context");
    if(proof.action=="bind"){
        const auto raw=ParseHex(proof.operator_script);const CScript key{raw.begin(),raw.end()};
        if(pgn::core::LeaderboardOperatorAt(native.bindings,key,proof.anchor_height).GetHex()!=proof.operator_id)throw InvalidAssociation("Native K cannot borrow another operator at the signed source anchor");
        if(require_current&&pgn::core::LeaderboardOperatorAt(native.bindings,key,native.height).GetHex()!=proof.operator_id)throw AssociationContextReset("Current Native key attribution changed; keep the packet");
        if(require_current&&!std::any_of(native.rows.begin(),native.rows.end(),[&](const auto& row){return row.operator_id.GetHex()==proof.operator_id;}))throw AssociationContextUnavailable("Confirmed productive Native operator unavailable");
    }
    if(!canonical(native.tip,native.height))throw AssociationContextUnavailable("Native source changed during consent validation");
}
struct PreparedConsent {UniValue statement;std::string event_id,social_challenge,operator_challenge,transition_challenge;std::optional<std::string> display_name;};
inline PreparedConsent PrepareConsent(const pgn::core::NativeLeaderboardSnapshot& native,const std::string& s,const std::string& k,const std::string& action,const std::string& previous,const std::optional<State>& current,bool target_complete,const std::string& nonce,const std::function<bool(const uint256&,int32_t)>& canonical){
    if(!target_complete||!canonical||!canonical(native.tip,native.height))throw AssociationContextUnavailable("Complete canonical Native target state required before signing");
    Require(Hex(s,44)&&s.starts_with("0014")&&Hex(k,44)&&k.starts_with("0014")&&Hex(previous,64)&&Hex(nonce,64),"Canonical consent identities/head/nonce required");
    if(current&&(current->committed.height>native.height||!canonical(current->committed.block,current->committed.height)))throw AssociationContextUnavailable("Native target lineage needs canonical refresh");
    Require(!current||current->social_script==s,"Native target script mismatch");
    const auto expected=current?current->head:std::string(64,'0');Require(previous==expected,"Native association head changed; prepare again");
    std::string o;
    if(action=="bind"){
        Require(!current||!current->active,"Withdraw the current association before linking another operator");const auto raw=ParseHex(k);o=pgn::core::LeaderboardOperatorAt(native.bindings,CScript{raw.begin(),raw.end()},native.height).GetHex();
        Require(std::any_of(native.rows.begin(),native.rows.end(),[&](const auto& row){return row.operator_id.GetHex()==o;}),"Confirmed productive Native key required");
    }else if(action=="withdraw"){
        Require(current&&current->active&&current->social_script==s&&current->operator_script==k,"Current Native lineage participant required");o=current->operator_id;
    }else throw std::runtime_error("Unsupported consent action");
    auto statement=Obj();statement.pushKV("v",2);statement.pushKV("genesis",native.genesis.GetHex());statement.pushKV("origin","pgn-social://"+native.genesis.GetHex());statement.pushKV("scope",Scope);statement.pushKV("action",action);statement.pushKV("social_script",s);statement.pushKV("operator_script",k);statement.pushKV("operator_id",o);statement.pushKV("anchor_hash",native.tip.GetHex());statement.pushKV("anchor_height",native.height);statement.pushKV("previous_link",previous);statement.pushKV("nonce",nonce);
    PreparedConsent out{statement,EventId(statement),Message(statement,"SOCIAL-CONSENT"),Message(statement,"OPERATOR-CONSENT"),Message(statement,"STATE-TRANSITION"),{}};
    if(action=="bind")out.transition_challenge.clear();else{out.social_challenge.clear();out.operator_challenge.clear();}
    for(const auto& row:native.rows)if(row.operator_id.GetHex()==o){out.display_name=row.display_name;break;}
    if(!canonical(native.tip,native.height))throw AssociationContextUnavailable("Native source changed while preparing consent");return out;
}
struct SubmissionConsent {ConsentCertificate certificate;ConsentCoverage coverage;bool already_included{};};
struct ConsentContext {ConsentCertificate certificate;bool already_included{};};
inline ConsentContext ValidateSubmissionContext(const std::string& source,const pgn::core::NativeLeaderboardSnapshot& native,const std::optional<State>& current,bool target_complete,const std::function<bool(const uint256&,int32_t)>& canonical){
    if(!target_complete)throw AssociationContextUnavailable("Native target history incomplete; operation preserved");
    auto proof=DecodeConsentCertificate(source,native.genesis.GetHex());
    if(current&&(current->committed.height>native.height||!canonical(current->committed.block,current->committed.height)))throw AssociationContextUnavailable("Current Native lineage unavailable; operation preserved");
    Require(!current||current->social_script==proof.social_script,"Submission target differs from Native lineage");
    const bool included=current&&current->head==proof.id;
    CheckConsentNativeContext(proof,native,canonical,!included);
    if(!included){
        if(proof.previous!=(current?current->head:std::string(64,'0')))throw AssociationContextReset("Native association head changed; do not overwrite it");
        if(proof.action=="bind"){if(current&&current->active)throw AssociationContextReset("An active association must be withdrawn explicitly");}
        else if(!current||!current->active||current->social_script!=proof.social_script||current->operator_script!=proof.operator_script||current->operator_id!=proof.operator_id)throw AssociationContextReset("Withdrawal does not match the current Native lineage");
    }
    return {std::move(proof),included};
}
inline SubmissionConsent ValidateSubmission(const std::string& source,const pgn::core::NativeLeaderboardSnapshot& native,const std::optional<State>& current,bool target_complete,bool require_complete,const std::function<bool(const uint256&,int32_t)>& canonical){
    auto context=ValidateSubmissionContext(source,native,current,target_complete,canonical);auto proof=std::move(context.certificate);const bool included=context.already_included;
    auto coverage=CheckConsentProofs(proof,require_complete);
    if(!canonical||!canonical(native.tip,native.height))throw AssociationContextUnavailable("Native context changed after consent verification; operation preserved");
    return {std::move(proof),coverage,included};
}
}
