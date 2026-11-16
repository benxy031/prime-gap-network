// Copyright (c) 2026 The PGN developers. MIT.
// Existing V2 certificate codec, shared by pending consent and Native inclusion.
// Parsing/signature proof is NOT a VerifiedNativeRequestBlock or role authority.
#pragma once
#include <pgn/social_identity_contract.h>
namespace pgn::social::identity::committed {
inline const std::string Tag="PGN-SOCIAL-LINK-2";
inline const std::string Prefix="PGN/SOCIAL/IDENTITY-LINK";
inline std::string EventId(const UniValue& statement){return Hash(Prefix+"-ID/2\n"+Canonical(statement));}
inline std::string Message(const UniValue& statement,const std::string& side){auto v=Obj();v.pushKV("event_id",EventId(statement));v.pushKV("statement",statement);return Prefix+"/"+side+"/2\n"+Canonical(v);}
inline CScript Envelope(const std::string& source){return CScript{}<<OP_RETURN<<std::vector<unsigned char>(Tag.begin(),Tag.end())<<std::vector<unsigned char>(source.begin(),source.end());}
class InvalidAssociation:public std::runtime_error {public:using std::runtime_error::runtime_error;};
class AssociationContextUnavailable:public std::runtime_error {public:using std::runtime_error::runtime_error;};
class AssociationContextReset:public std::runtime_error {public:using std::runtime_error::runtime_error;};
struct ConsentCertificate {
    UniValue statement;
    std::string source,id,action,social_script,operator_script,operator_id,previous,anchor,authorizer;
    int32_t anchor_height{};
    std::string social_proof,operator_proof,authorization_proof;
};
inline ConsentCertificate DecodeConsentCertificate(const std::string& source,const std::string& genesis){try{
    Require(source.size()<=MaxFrame,"Bounded V2 consent certificate required");
    const UniValue value=Parse(source); // Bound nesting before the JSON parser.
    Fields(value,{"statement","social_proof","operator_proof","authorizer_script","authorization_proof"});const auto& statement=value["statement"];
    Fields(statement,{"v","genesis","origin","scope","action","social_script","operator_script","operator_id","anchor_hash","anchor_height","previous_link","nonce"});
    ConsentCertificate out;out.source=source;out.statement=statement;out.id=EventId(statement);out.action=statement["action"].get_str();out.social_script=statement["social_script"].get_str();out.operator_script=statement["operator_script"].get_str();out.operator_id=statement["operator_id"].get_str();out.previous=statement["previous_link"].get_str();out.anchor=statement["anchor_hash"].get_str();out.anchor_height=statement["anchor_height"].getInt<int32_t>();out.authorizer=value["authorizer_script"].get_str();out.social_proof=value["social_proof"].get_str();out.operator_proof=value["operator_proof"].get_str();out.authorization_proof=value["authorization_proof"].get_str();
    Require(statement["v"].getInt<int>()==2&&statement["genesis"].get_str()==genesis&&statement["origin"].get_str()=="pgn-social://"+genesis&&statement["scope"].get_str()==Scope,"Persistent association version/network/scope mismatch");
    const auto script=[](const std::string& s){return Hex(s,44)&&s.starts_with("0014");};
    Require(script(out.social_script)&&script(out.operator_script)&&Hex(out.operator_id,64)&&Hex(out.previous,64)&&Hex(statement["nonce"].get_str(),64),"Persistent association identity bound");
    const auto anchor=uint256::FromHex(out.anchor);Require(anchor&&anchor->GetHex()==out.anchor&&out.anchor_height>=0,"Association Native source anchor required");
    if(out.action=="bind")Require(out.authorizer.empty()&&out.authorization_proof.empty(),"Binding requires two distinct domain consents, not a transition proof");
    else if(out.action=="withdraw")Require(out.previous!=std::string(64,'0')&&(out.authorizer==out.social_script||out.authorizer==out.operator_script)&&out.social_proof.empty()&&out.operator_proof.empty(),"Withdrawal requires one current participant transition proof");
    else throw std::runtime_error("Unsupported persistent association action");
    return out;
}catch(const std::bad_alloc&){throw;}catch(const std::exception& error){throw InvalidAssociation(error.what());}
}
struct ConsentCoverage {bool social{},operator_key{},transition{};bool complete{};};
inline ConsentCoverage CheckConsentProofs(const ConsentCertificate& proof,bool require_complete){try{
    ConsentCoverage result;
    if(proof.action=="bind"){
        if(!proof.social_proof.empty()){VerifyProof(proof.social_script,proof.social_proof,Message(proof.statement,"SOCIAL-CONSENT"));result.social=true;}
        if(!proof.operator_proof.empty()){VerifyProof(proof.operator_script,proof.operator_proof,Message(proof.statement,"OPERATOR-CONSENT"));result.operator_key=true;}
        result.complete=result.social&&result.operator_key;
    }else{
        if(!proof.authorization_proof.empty()){VerifyProof(proof.authorizer,proof.authorization_proof,Message(proof.statement,"STATE-TRANSITION"));result.transition=true;}
        result.complete=result.transition;
    }
    Require(!require_complete||result.complete,"Explicit consent signatures are incomplete");return result;
}catch(const std::bad_alloc&){throw;}catch(const std::exception& error){throw InvalidAssociation(error.what());}
}
}
