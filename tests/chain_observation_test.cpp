#include "wallet/chain_observation.h"
#include <QCoreApplication>
#include <iostream>
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);using namespace pgn::wallet;
 CoreProfile profile;profile.genesisHash=QString(64,'b');
 const CoreEpoch epoch{42,"134041921234567890",1};
 const auto utc=QDateTime::fromString("2026-10-11T11:00:00.123Z",Qt::ISODateWithMs);
 const QJsonObject chain{{"chain","main"},{"blocks",0},{"initialblockdownload",false}};
 const auto result=projectChainObservation(epoch,profile,chain,QString(64,'B'),utc);
 if(!result.observation||!result.error.isEmpty())return 1;
 const auto& value=*result.observation;
 if(value.height!=0||value.genesis!=profile.genesisHash||value.observedUtc!=utc||
    !value.synchronizing.has_value()||*value.synchronizing||!value.owner.matches(epoch))return 2;
 if(!value.freshAt(utc)||!value.freshAt(utc.addMSecs(30000))||value.freshAt(utc.addMSecs(30001))||
    value.freshAt(utc.addMSecs(-1))||value.freshAt(utc,-1))return 3;
 auto missingSync=chain;missingSync.remove("initialblockdownload");
 const auto unknown=projectChainObservation(epoch,profile,missingSync,profile.genesisHash,utc);
 if(!unknown.observation||unknown.observation->synchronizing.has_value())return 4;
 auto badChain=chain;badChain.insert("chain","regtest");
 if(projectChainObservation(epoch,profile,badChain,profile.genesisHash,utc).observation||
    projectChainObservation(epoch,profile,chain,QString(64,'c'),utc).observation||
    projectChainObservation({},profile,chain,profile.genesisHash,utc).observation||
    projectChainObservation(epoch,profile,chain,profile.genesisHash,{}).observation)return 5;
 for(const auto& height:{QJsonValue(),QJsonValue("0"),QJsonValue(-1),QJsonValue(0.5),QJsonValue(9007199254740992.0),QJsonValue(true)}){
  auto invalid=chain;invalid.insert("blocks",height);
  if(projectChainObservation(epoch,profile,invalid,profile.genesisHash,utc).observation)return 6;
 }
 auto missingHeight=chain;missingHeight.remove("blocks");
 if(projectChainObservation(epoch,profile,missingHeight,profile.genesisHash,utc).observation)return 7;
 auto invalid=value;invalid.genesis="unbound";if(invalid.valid()||invalid.freshAt(utc))return 8;
 auto quiet=chain;quiet.insert("blocks",10);quiet.insert("headers",10);quiet.insert("initialblockdownload",true);quiet.insert("time",utc.addDays(-2).toSecsSinceEpoch());
 auto quietValue=projectChainObservation(epoch,profile,quiet,profile.genesisHash,utc);if(!quietValue.observation)return 9;
 const QString endpoint="127.0.0.1:32100";
 const QJsonObject peer{{"addr",endpoint},{"connection_type","manual"},{"pgn_announced_height",10},{"pgn_syncing",false}};
 const auto caughtUp=projectNativeSynchronization(*quietValue.observation,QJsonArray{peer},endpoint);
 if(caughtUp.synchronizing!=false||caughtUp.coreInitialBlockDownload!=true||!caughtUp.nativePeerCaughtUp||!caughtUp.oldTipAt(utc)||!caughtUp.owner.matches(epoch))return 10;
 for(const auto& pendingPeer:{QJsonObject{{"addr",endpoint},{"connection_type","manual"},{"pgn_announced_height",11},{"pgn_syncing",false}},QJsonObject{{"addr",endpoint},{"connection_type","manual"},{"pgn_announced_height",10},{"pgn_syncing",true}}}){
  const auto held=projectNativeSynchronization(*quietValue.observation,QJsonArray{pendingPeer},endpoint);if(held.synchronizing!=true||held.nativePeerCaughtUp)return 11;
 }
 auto headersAhead=*quietValue.observation;headersAhead.headerHeight=11;
 if(projectNativeSynchronization(headersAhead,QJsonArray{peer},endpoint).synchronizing!=true)return 12;
 auto noHeaders=*quietValue.observation;noHeaders.headerHeight.reset();
 if(projectNativeSynchronization(noHeaders,QJsonArray{peer},endpoint).synchronizing)return 13;
 for(const auto& badHeight:{QJsonValue(),QJsonValue("10"),QJsonValue(true),QJsonValue(-1),QJsonValue(.5),QJsonValue(9007199254740992.0)}){
  auto bad=peer;bad.insert("pgn_announced_height",badHeight);const auto held=projectNativeSynchronization(*quietValue.observation,QJsonArray{bad},endpoint);if(held.synchronizing||held.nativePeerCaughtUp)return 14;
 }
 for(const auto& badSync:{QJsonValue(),QJsonValue("false"),QJsonValue(0)}){
  auto bad=peer;bad.insert("pgn_syncing",badSync);if(projectNativeSynchronization(*quietValue.observation,QJsonArray{bad},endpoint).synchronizing)return 15;
 }
 if(projectNativeSynchronization(*quietValue.observation,{},endpoint).synchronizing!=true)return 16;
 auto missingCoreFlag=*quietValue.observation;missingCoreFlag.coreInitialBlockDownload.reset();if(projectNativeSynchronization(missingCoreFlag,QJsonArray{peer},endpoint).synchronizing)return 17;
 auto fresh=caughtUp;fresh.tipTime=utc.toSecsSinceEpoch();if(fresh.oldTipAt(utc))return 18;
 auto inbound=peer;inbound.insert("connection_type","inbound");if(projectNativeSynchronization(*quietValue.observation,QJsonArray{inbound},endpoint).synchronizing)return 19;
 if(projectNativeSynchronization(*quietValue.observation,QJsonArray{peer},{}).synchronizing)return 20;
 auto other=peer;other.insert("addr","127.0.0.1:32101");if(projectNativeSynchronization(*quietValue.observation,QJsonArray{other},endpoint).synchronizing!=true)return 21;
 auto measured=peer;measured.insert("synced_headers",12);measured.insert("inflight",QJsonArray{11,12});
 const auto diagnostic=projectNativeSynchronization(*quietValue.observation,QJsonArray{measured},endpoint);
 const QJsonObject expected{{"announced_height",10},{"syncing",false},{"synced_headers",12},{"inflight_count",2},{"first_inflight_height",11}};
 if(diagnostic.nativePeerDiagnostic!=expected||!diagnostic.nativePeerCaughtUp)return 22;
 const auto malformed=projectNativeSynchronization(diagnostic,QJsonArray{measured,"malformed"},endpoint);
 if(!malformed.nativePeerDiagnostic.isEmpty()||malformed.synchronizing||malformed.nativePeerCaughtUp)return 23;
 const auto duplicate=projectNativeSynchronization(diagnostic,QJsonArray{measured,peer},endpoint);
 if(!duplicate.nativePeerDiagnostic.isEmpty()||!duplicate.nativePeerCaughtUp||duplicate.synchronizing!=false)return 24;
 auto partial=peer;partial.insert("inflight",QJsonArray{1.5});partial.insert("synced_blocks",9007199254740992.0);
 const auto sanitized=projectNativeSynchronization(diagnostic,QJsonArray{partial},endpoint);
 if(sanitized.nativePeerDiagnostic.size()!=2||sanitized.nativePeerDiagnostic.contains("addr"))return 25;
 GoldBlockStatus gold;
 auto goldTip=value;
 goldTip.height=100;goldTip.tipHash=QString(64,'c');
 QJsonObject tally{{"genesis",goldTip.genesis},{"height",100},{"bestblockhash",goldTip.tipHash},{"gold_blocks",8}};
 gold.observe(goldTip);
 if(gold.reading(goldTip,utc))return 26;
 gold.observeState(tally,epoch,utc);
 const auto exactGold=gold.reading(goldTip,utc);
 if(!exactGold||exactGold->count!=8||exactGold->height!=100||!exactGold->atTip)return 27;
 auto forward=goldTip;
 forward.height=101;forward.tipHash=QString(64,'d');forward.observedUtc=utc.addSecs(10);
 gold.observe(forward);
 const auto lastGold=gold.reading(forward,forward.observedUtc);
 if(!lastGold||lastGold->count!=8||lastGold->atTip)return 28;
 auto newer=tally;
 newer["height"]=101;newer["bestblockhash"]=forward.tipHash;newer["gold_blocks"]=101.5;
 gold.observeState(newer,epoch,forward.observedUtc);
 if(gold.reading(forward,forward.observedUtc)->count!=8)return 29;
 auto later=forward;later.observedUtc=utc.addSecs(91);
 if(gold.reading(later,later.observedUtc))return 30;
 newer["gold_blocks"]=9;
 gold.observeState(newer,epoch,forward.observedUtc);
 const auto refreshed=gold.reading(forward,forward.observedUtc);
 if(!refreshed||!refreshed->atTip||refreshed->count!=9)return 31;
 auto fork=forward;fork.tipHash=QString(64,'e');
 auto replacement=newer;replacement["bestblockhash"]=fork.tipHash;replacement["gold_blocks"]=7;
 const auto earlierBranch=gold.branch();
 gold.observeState(replacement,epoch,forward.observedUtc);
 if(!gold.observe(fork))return 32;
 const auto adopted=gold.reading(fork,forward.observedUtc);
 if(!adopted||!adopted->atTip||adopted->count!=7)return 33;
 gold.observeConfirmedState(tally,epoch,earlierBranch,forward.observedUtc);
 if(gold.reading(fork,forward.observedUtc)->count!=7)return 34;
 auto restarted=fork;restarted.owner={43,"134041921234567891",2};
 gold.observe(restarted);
 gold.observeConfirmedState(replacement,epoch,earlierBranch,forward.observedUtc);
 if(gold.reading(restarted,forward.observedUtc))return 35;
 for(bool rewinds:{false,true}){
  GoldBlockStatus reorg;
  reorg.observe(goldTip);
  reorg.observeState(tally,epoch,utc);
  auto lead=forward;lead.height=102;
  if(reorg.observe(lead)||!reorg.reading(lead,lead.observedUtc))return 36;
  auto next=lead;next.height=rewinds?101:102;next.tipHash=QString(64,'f');
  if(!reorg.observe(next)||reorg.reading(next,next.observedUtc))return 37;
 }
 GoldBlockStatus skipped;
 skipped.observe(goldTip);
 const auto branch=skipped.branch();
 skipped.observeState(newer,epoch,utc);
 auto ahead=forward;ahead.height=102;ahead.tipHash=QString(64,'f');
 skipped.observe(ahead);
 if(skipped.reading(ahead,ahead.observedUtc))return 38;
 skipped.observeConfirmedState(newer,epoch,branch,ahead.observedUtc);
 const auto confirmed=skipped.reading(ahead,ahead.observedUtc);
 if(!confirmed||confirmed->atTip||confirmed->height!=101||confirmed->count!=9)return 39;
 std::cout<<"PASS verified and delayed Gold counts, advancing and replaced tips, owned chain identity, exact heights, stale rejection, quiet-native catchup preserving raw CoreIBD, pending/unknown/foreignbridge holds\n";
}
