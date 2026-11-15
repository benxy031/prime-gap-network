// Copyright (c) 2026 The PGN developers. MIT.
#pragma once
#include <pgn/social_identity_target.h>
#include <map>

namespace pgn::social::identity::committed {
inline bool TargetScript(const UniValue& value) {
    return value.isStr() && Hex(value.get_str(),44) && value.get_str().starts_with("0014");
}
inline UniValue ResolutionJSON(const TargetResolution& result) {
    auto out=Obj();out.pushKV("social_script",result.social_script);out.pushKV("state",result.state);
    out.pushKV("complete",result.complete);out.pushKV("reason",result.reason);
    out.pushKV("checked",uint64_t(result.checked));out.pushKV("invalid",uint64_t(result.invalid));
    if(result.display_name)out.pushKV("display_name",*result.display_name);
    if(result.association){const auto& a=*result.association;
        out.pushKV("operator_id",a.operator_id);out.pushKV("operator_script",a.operator_script);
        out.pushKV("binding_head",a.head);out.pushKV("binding_active",a.active);
        out.pushKV("binding_height",a.committed.height);out.pushKV("binding_block",a.committed.block.GetHex());
        out.pushKV("binding_output",uint64_t(a.committed.output));
    }
    return out;
}
// This is request validation, not identity authority. One malformed element
// cannot prevent unrelated targets from being resolved. Duplicates share work.
inline UniValue ResolveBatch(const UniValue& scripts,const std::function<TargetResolution(const std::string&)>& resolve) {
    Require(scripts.isArray()&&scripts.size()>0&&scripts.size()<=100&&bool(resolve),"One through100 Social scripts required");
    UniValue results(UniValue::VARR);std::map<std::string,UniValue> completed;
    for(size_t i=0;i<scripts.size();++i){
        UniValue item;
        if(!TargetScript(scripts[i])){item=Obj();item.pushKV("state","invalid");item.pushKV("complete",false);item.pushKV("reason","Canonical lower-case P2WPKH Social script required");}
        else {
            const auto& s=scripts[i].get_str();
            auto prior=completed.find(s);
            if(prior!=completed.end())item=prior->second;
            else {
                try{item=ResolutionJSON(resolve(s));}
                catch(const std::exception&){item=Obj();item.pushKV("social_script",s);item.pushKV("state","unavailable");item.pushKV("complete",false);item.pushKV("reason","Target verification unavailable; retry without changing identity");}
                completed.emplace(s,item);
            }
        }
        item.pushKV("input_index",uint64_t(i));results.push_back(item);
    }
    return results;
}
}
