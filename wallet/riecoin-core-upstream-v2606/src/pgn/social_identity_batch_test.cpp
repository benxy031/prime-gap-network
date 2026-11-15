// Private request isolation test. Native authority verification is covered by
// the real BIP322/Native archive target test; callbacks here test batch plumbing.
#include <pgn/social_identity_batch.h>
#include <iostream>
using namespace pgn::social::identity::committed;
int main(){try{
    size_t checks=0;const auto expect=[&](bool ok,const char* text){if(!ok)throw std::runtime_error(text);++checks;std::cout<<"PASS "<<text<<'\n';};
    const auto a="0014"+std::string(40,'a'),b="0014"+std::string(40,'b');
    UniValue targets(UniValue::VARR);targets.push_back(a);targets.push_back(42);targets.push_back(b);targets.push_back(a);targets.push_back("Wizz");
    std::map<std::string,unsigned> calls;
    const auto results=ResolveBatch(targets,[&](const auto& s){++calls[s];TargetResolution r;r.social_script=s;r.state="unlinked";r.complete=true;return r;});
    expect(results.size()==5&&results[1]["state"].get_str()=="invalid"&&results[4]["state"].get_str()=="invalid","Malformed element and username remain invalid, not an identity");
    expect(results[0]["complete"].get_bool()&&results[2]["complete"].get_bool(),"Malformed input does not prevent unrelated identities resolving");
    expect(calls[a]==1&&calls[b]==1&&results[3]["input_index"].getInt<int>()==3,"Repeated target shares work while retaining exact input index");
    UniValue fail(UniValue::VARR);fail.push_back(a);fail.push_back(b);
    const auto separate=ResolveBatch(fail,[&](const auto& s)->TargetResolution{if(s==a)throw std::runtime_error("missing archive");TargetResolution r;r.social_script=s;r.state="unlinked";r.complete=true;return r;});
    expect(separate[0]["state"].get_str()=="unavailable"&&!separate[0].exists("display_name")&&separate[1]["complete"].get_bool(),"Unavailable proof for A does not authorize A or block B");
    auto bad=UniValue(UniValue::VARR);for(unsigned i=0;i<101;++i)bad.push_back(a);bool rejected=false;try{ResolveBatch(bad,[](const auto&){return TargetResolution{};});}catch(const std::exception&){rejected=true;}
    expect(rejected,"Request limit100 is independent of global identity population");
    UniValue cap(UniValue::VARR);for(unsigned i=0;i<100;++i)cap.push_back(a);
    expect(ResolveBatch(cap,[](const auto& s){TargetResolution r;r.social_script=s;r.state="pending";return r;}).size()==100,"Exact request bound100 remains usable");
    expect(!TargetScript(UniValue(a+"00"))&&!TargetScript(UniValue("0014"+std::string(40,'A'))),"Noncanonical and oversized scripts rejected");
    TargetResolution r;r.social_script=a;r.state="linked";r.complete=true;r.display_name="<b>same name</b>";r.association=State{a,b,std::string(64,'c'),std::string(64,'d'),std::string(64,'e'),1,Position{2,3,uint256::FromHex(std::string(64,'f')).value()},true};
    const auto json=ResolutionJSON(r);expect(json["display_name"].get_str()=="<b>same name</b>"&&json["binding_output"].getInt<int>()==3&&!json.exists("role"),"Name is data with provenance, never a role or executable markup");
    std::cout<<"CHECKS "<<checks<<"\nLIMIT Batch plumbing only; real Native crypto is a separate fixture.\n";return 0;
}catch(const std::exception& e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
