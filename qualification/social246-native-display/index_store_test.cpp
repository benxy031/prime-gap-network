#include <pgn/social_display_store.h>
#include <key.h>
#include <fstream>
#include <iostream>
#include <chrono>
#include <cstdlib>
using namespace pgn::social::display;
static unsigned checks;
static void expect(bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);}
static Context context(const UniValue& fixture){
 Context c;c.genesis=fixture["genesis"].get_str();c.tip=fixture["anchor"].get_str();c.height=fixture["height"].getInt<int>();
 c.canonical=[height=c.height,hash=c.tip](int h,const std::string& v){return h==height&&v==hash;};
 c.native=[key=fixture["operator_script"].get_str(),op=fixture["operator_id"].get_str()](const std::string& s){return std::pair{s==key?op:std::string(64,'0'),s==key?std::string("SYNTHETIC index operator"):std::string{}};};return c;
}
static std::string state(Store& store,const std::string& s){return store.Lookup({s})["results"][0]["state"].get_str();}
static uint64_t bytes(const fs::path& path){uint64_t sum{};if(fs::exists(path))for(const auto& p:fs::recursive_directory_iterator(path))if(p.is_regular_file())sum+=p.file_size();return sum;}
static void seed(const fs::path& path,const UniValue& f,bool reverse){
 CDBWrapper db(DBParams{.path=path,.cache_bytes=2*1024*1024,.memory_only=false,.wipe_data=false});CDBBatch b(db);
 b.Write(std::string("genesis"),f["genesis"].get_str());b.Write(std::string("sequence"),uint64_t{4});b.Write(std::string("generation"),std::string(64,'a'));
 const int order[2][4]={{3,1,0,2},{2,3,0,1}};for(int i=0;i<4;++i)b.Write(std::pair{uint8_t{'e'},std::string(19,'0')+std::to_string(i+1)},f["migration"][order[reverse?1:0][i]].get_str());
 pgn::social::CommitSocialBatch(db,b);
}
static void migration(Store& store,const UniValue& f){
 expect(state(store,f["migration_script"].get_str())=="conflict","Out-of-order migration lost fork or left a detached child");
 auto page=store.Page(0,"");expect(page.exists("certificate"),"Migration replica root missing");
 const auto root=DecodeConsentCertificate(page["certificate"].get_str(),f["genesis"].get_str());expect(root.previous==std::string(64,'0'),"Migration cursor sends child before parent");
 const auto generation=page["generation"].get_str();uint64_t cursor=page["next_cursor"].getInt<uint64_t>();int count=1;
 while(page["more"].get_bool()){page=store.Page(cursor,generation);expect(page.exists("certificate"),"Migration replica child missing");cursor=page["next_cursor"].getInt<uint64_t>();++count;}
 expect(count==4,"Migration replica proof count changed");
}
int main(int argc,char** argv){
 ECC_Context ecc;
 if(argc!=4)return 2;std::ifstream input(argv[1],std::ios::binary);std::string raw((std::istreambuf_iterator<char>(input)),{});if(raw.size()>4*1024*1024)return 2;
 UniValue f;if(!f.read(raw))return 2;const fs::path root=fs::u8path(argv[2]);const auto mode=std::string(argv[3]);const auto genesis=f["genesis"].get_str();const auto c=context(f);Source source=[c]{return c;};
 try{
  if(mode=="seed-crash"){fs::create_directories(root);seed(root/"interrupted",f,false);return 0;}
  if(mode=="crash"){
   DisplayIndexCommittedTestHook=[]{std::_Exit(77);};Store store(root/"interrupted",genesis,source);store.Lookup({f["migration_script"].get_str()});return 3;
  }
  if(mode=="resume"){
   Store store(root/"interrupted",genesis,source);migration(store,f);std::cout<<"RESUME PASS "<<checks<<"\n";return 0;
  }
  if((mode!="test"&&mode!="smoke")||fs::exists(root))return 2;fs::create_directories(root);
  uint64_t reads16{},reads1000{},logical{},disk_before{},disk_after{};const auto started=std::chrono::steady_clock::now();
  {
   Store store(root/"linear",genesis,source);const auto& events=f["events"].getValues();expect(events.size()>=32,"At least32 real signature transitions required");
   for(size_t i=0;i<events.size();++i){const auto r=store.Admit(events[i].get_str());expect(r.state=="accepted_local"&&r.appended,"Honest indexed transition rejected");
    if(i==0)disk_before=bytes(root/"linear");
    if(i==15||i+1==events.size()){const auto before=store.ProofReadsSinceStart();expect(state(store,f["social_script"].get_str())=="withdrawn","K withdrawal lost");const auto count=store.ProofReadsSinceStart()-before;expect(count<=4,"Resolution work grew with retained history");if(i==15)reads16=count;else reads1000=count;}
   }
   expect(store.Admit(events.front().get_str()).state=="accepted_local","Known proof replay not idempotent");expect(state(store,f["social_script"].get_str())=="withdrawn","Replay resurrected withdrawn display");logical=store.LogicalBytesSinceStart();
  }
  disk_after=bytes(root/"linear");
  if(mode=="smoke"){Store store(root/"linear",genesis,source);expect(state(store,f["social_script"].get_str())=="withdrawn","Restart lost withdrawal");std::cout<<"SMOKE PASS "<<checks<<" transitions="<<f["events"].size()<<" proof_reads16="<<reads16<<" proof_reads_end="<<reads1000<<" disk_before="<<disk_before<<" disk_after="<<disk_after<<"\n";return 0;}
  {Store store(root/"linear",genesis,source);expect(state(store,f["social_script"].get_str())=="withdrawn","1000-transition restart lost withdrawal");for(const auto& v:f["fork"].getValues())expect(store.Admit(v.get_str()).state=="accepted_local","Fork proof rejected");expect(state(store,f["social_script"].get_str())=="conflict","Late fork of retained history not detected");}
  {
   Store sender(root/"parents",genesis,source),receiver(root/"receiver",genesis,source);const auto& records=f["migration"].getValues();
   expect(sender.Admit(records[1].get_str()).state=="pending_parent","Missing parent was provisionally admitted");expect(!sender.Page(0,"").exists("certificate"),"Missing parent advanced sync cursor");
   expect(sender.Admit(records[0].get_str()).state=="accepted_local","Parent not admitted");
   for(int n=0;n<1000;++n)expect(sender.Admit(f["missing_parent"].get_str()).state=="pending_parent","Transient missing-parent retry changed state");
   expect(state(sender,f["migration_script"].get_str())=="linked","Traffic retry erased current name");
   expect(sender.Admit(records[1].get_str()).state=="accepted_local","Child retry failed after parent");
   auto page=sender.Page(0,"");expect(receiver.Admit(page["certificate"].get_str()).state=="accepted_local","Replica parent lost");page=sender.Page(page["next_cursor"].getInt<uint64_t>(),page["generation"].get_str());expect(receiver.Admit(page["certificate"].get_str()).state=="accepted_local","Replica child lost");expect(state(receiver,f["migration_script"].get_str())=="linked","Replica parent/child resolution failed");
  }
  for(bool reverse:{false,true}){const auto path=root/(reverse?"migration-b":"migration-a");seed(path,f,reverse);{Store store(path,genesis,source);migration(store,f);}{Store store(path,genesis,source);migration(store,f);}}
  {
   const auto path=root/"exception";seed(path,f,false);bool first=true;Store store(path,genesis,source);DisplayIndexCommittedTestHook=[&]{if(first){first=false;throw std::runtime_error("EXPLICIT test interruption after durable index commit");}};
   bool failed=false;try{store.Lookup({f["migration_script"].get_str()});}catch(const std::exception&){failed=true;}expect(failed,"Injected index interruption did not run");DisplayIndexCommittedTestHook={};migration(store,f);
  }
  const auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
  std::cout<<"INDEX PASS "<<checks<<" elapsed="<<elapsed<<" proof_reads16="<<reads16<<" proof_reads1000="<<reads1000<<" disk_before="<<disk_before<<" disk_after="<<disk_after<<" logical_estimate="<<logical<<"\n";return 0;
 }catch(const std::exception& error){std::cerr<<"INDEX FAIL after "<<checks<<": "<<error.what()<<"\n";return 1;}
}
