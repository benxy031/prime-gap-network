// Copyright (c) 2026 The PGN developers. MIT; see COPYING.
#pragma once
#include <common/signmessage.h>
#include <crypto/sha256.h>
#include <key_io.h>
#include <script/solver.h>
#include <univalue.h>
#include <util/strencodings.h>
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace pgn::social::identity {
inline constexpr int64_t WorkingLifetimeMs = 86400000; // Explicit provisional Root policy.
inline constexpr int64_t FutureSkewMs = 60000, SafeInteger = 9007199254740991LL;
inline constexpr size_t MaxFrame = 24576;
inline const std::string Domain = "PGN/SOCIAL/IDENTITY-LINK";
inline const std::string Scope = "display_link_for_script_history";
inline void Require(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
inline bool Hex(const std::string& text, size_t n) {
    return text.size()==n && std::all_of(text.begin(),text.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});
}
inline std::string Hash(const std::string& text) {
    unsigned char out[32]; CSHA256().Write(reinterpret_cast<const unsigned char*>(text.data()),text.size()).Finalize(out);return HexStr(out);
}
inline UniValue Obj(){return UniValue{UniValue::VOBJ};}
inline std::string Canonical(const UniValue& value,unsigned depth=0) {
    Require(depth<=5,"Identity document depth exceeded");
    if(value.isObject()){
        std::map<std::string,UniValue> fields;
        const auto& keys=value.getKeys();const auto& values=value.getValues();
        for(size_t i=0;i<keys.size();++i)Require(fields.emplace(keys[i],values[i]).second,"Duplicate identity key");
        std::string out="{";
        for(const auto& [key,item]:fields){if(out.size()>1)out+=",";out+=Canonical(UniValue{key},depth+1)+":"+Canonical(item,depth+1);}return out+"}";
    }
    if(value.isStr()){
        const auto& s=value.get_str();Require(std::all_of(s.begin(),s.end(),[](unsigned char c){return c>=32&&c<=126;}),"Identity metadata must be printable ASCII");return value.write();
    }
    if(value.isNum()){
        const auto n=value.getInt<int64_t>();Require(n>=0&&n<=SafeInteger&&std::to_string(n)==value.getValStr(),"Safe identity integer required");return value.write();
    }
    throw std::runtime_error("Unsupported identity document type");
}
inline UniValue Parse(const std::string& source){
    Require(!source.empty()&&source.size()<=MaxFrame,"Bounded identity frame required");
    unsigned depth=0;bool quoted=false,escaped=false;
    for(unsigned char c:source){Require(c>=32&&c<=126,"Identity source must be ASCII");if(quoted){if(escaped)escaped=false;else if(c=='\\')escaped=true;else if(c=='"')quoted=false;}
        else if(c=='"')quoted=true;else if(c=='{'||c=='[')Require(++depth<=5,"Identity parser depth exceeded");else if(c=='}'||c==']'){Require(depth>0,"Identity document structure invalid");--depth;}}
    UniValue out;Require(out.read(source)&&out.isObject()&&Canonical(out)==source,"Canonical identity object required");return out;
}
inline void Fields(const UniValue& value,const std::set<std::string>& expected){
    Require(value.isObject(),"Identity object required");const auto keys=value.getKeys();Require(keys.size()==expected.size()&&std::set<std::string>(keys.begin(),keys.end())==expected,"Unexpected identity fields");
}
struct Policy {
    const int64_t max_lifetime_ms;
    explicit Policy(int64_t value):max_lifetime_ms(value){Require(value>0&&value<=SafeInteger,"Explicit identity lifetime policy required");}
    UniValue Document() const {auto p=Obj();p.pushKV("v",1);p.pushKV("max_lifetime_ms",max_lifetime_ms);p.pushKV("future_skew_ms",FutureSkewMs);p.pushKV("proof_encoding","bip322_simple_unprefixed_p2wpkh");p.pushKV("scope",Scope);return p;}
    std::string Id()const{return Hash(Domain+"-POLICY/1\n"+Canonical(Document()));}
};
inline std::string BindingId(const UniValue& c){return Hash(Domain+"-ID/1\n"+Canonical(c));}
inline std::string BindingMessage(const UniValue& c,bool social){auto p=Obj();p.pushKV("binding_id",BindingId(c));p.pushKV("statement",c);return Domain+(social?"/SOCIAL-CONSENT/1\n":"/OPERATOR-CONSENT/1\n")+Canonical(p);}
inline std::string WithdrawalId(const UniValue& c){return Hash(Domain+"-WITHDRAW-ID/1\n"+Canonical(c));}
inline void VerifyProof(const std::string& script,const std::string& proof,const std::string& message){
    Require(Hex(script,44)&&script.starts_with("0014"),"Native P2WPKH identity required");
    Require(!proof.empty()&&proof.size()<=512,"Bounded identity proof required");const auto raw=DecodeBase64(proof);
    Require(raw&&raw->size()>=100&&raw->size()<=140&&(*raw)[0]==2&&EncodeBase64(*raw)==proof,"Canonical Native SIMPLE proof required");
    const auto bytes=ParseHex(script);CScript signing{bytes.begin(),bytes.end()};CTxDestination destination;
    Require(ExtractDestination(signing,destination),"Native identity destination unavailable");
    // Destination/script are derived, never accepted as an asserted client address.
    auto witness=*raw;Require(MessageVerifyBIP322(destination,witness,message,MessageVerificationResult::ERR_ADDRESS_NO_KEY)==MessageVerificationResult::OK,"Native identity signature refused");
}
struct Record {
    std::string source,id,event_id,social_script,operator_script,operator_id,anchor;
    int32_t height{};int64_t created{},expires{};bool withdrawal{};
};
class Contract {
    const Policy policy_;const std::string genesis_;
    void Context(const UniValue& c)const{
        Require(c["v"].getInt<int>()==1&&c["policy_id"].get_str()==policy_.Id(),"Identity policy/version mismatch");
        Require(c["genesis"].get_str()==genesis_&&c["origin"].get_str()=="pgn-social://"+genesis_,"Identity network mismatch");
        Require(Hex(c["nonce"].get_str(),64)&&Canonical(c).size()<=2048,"Identity nonce/statement bound invalid");
    }
    Record Binding(const UniValue& b)const{
        Fields(b,{"kind","binding_id","statement","social_proof","operator_proof"});Require(b["kind"].get_str()=="binding","Identity binding kind required");const auto& c=b["statement"];
        Fields(c,{"v","policy_id","genesis","origin","social_script","operator_script","operator_id","anchor_hash","anchor_height","created_ms","not_after_ms","nonce","scope"});Context(c);
        const auto s=c["social_script"].get_str(),k=c["operator_script"].get_str(),o=c["operator_id"].get_str(),anchor=c["anchor_hash"].get_str();
        Require(Hex(o,64)&&Hex(anchor,64)&&c["scope"].get_str()==Scope,"Identity association scope invalid");
        const auto height=c["anchor_height"].getInt<int32_t>();const auto created=c["created_ms"].getInt<int64_t>(),expires=c["not_after_ms"].getInt<int64_t>();
        Require(height>=0&&expires>created&&expires-created<=policy_.max_lifetime_ms,"Identity lifetime/height invalid");
        const auto id=BindingId(c);Require(id==b["binding_id"].get_str(),"Identity binding ID mismatch");
        VerifyProof(s,b["social_proof"].get_str(),BindingMessage(c,true));VerifyProof(k,b["operator_proof"].get_str(),BindingMessage(c,false));
        return {Canonical(b),id,id,s,k,o,anchor,height,created,expires,false};
    }
public:
    Contract(Policy policy,std::string genesis):policy_(policy),genesis_(std::move(genesis)){Require(Hex(genesis_,64),"Identity genesis required");}
    const Policy& GetPolicy()const{return policy_;}const std::string& Genesis()const{return genesis_;}
    Record Verify(const std::string& source)const{
        const auto value=Parse(source);if(value["kind"].get_str()=="binding")return Binding(value);
        Fields(value,{"kind","withdrawal_id","binding","statement","proof"});Require(value["kind"].get_str()=="withdrawal","Identity record kind invalid");auto out=Binding(value["binding"]);const auto& q=value["statement"];
        Fields(q,{"v","policy_id","genesis","origin","binding_id","revoker_script","nonce","created_ms"});Context(q);
        const auto revoker=q["revoker_script"].get_str();Require(q["binding_id"].get_str()==out.id&&(revoker==out.social_script||revoker==out.operator_script),"Identity withdrawal authority mismatch");
        const auto id=WithdrawalId(q);Require(id==value["withdrawal_id"].get_str(),"Identity withdrawal ID mismatch");VerifyProof(revoker,value["proof"].get_str(),Domain+"/WITHDRAW/1\n"+Canonical(q));
        out.source=source;out.event_id=id;out.withdrawal=true;return out;
    }
};
} // namespace pgn::social::identity
