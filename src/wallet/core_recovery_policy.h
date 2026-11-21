#pragma once
#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

namespace pgn::wallet {
// Operational recovery headroom, not a consensus or miner-admission threshold.
// CheckDiskSpace() has a 50 MiB floor; init.cpp's periodic check adds 50 MiB.
class CoreRecoveryPolicy {
public:
 enum class State { Idle, Running, WaitingDisk, WaitingMemory, Backoff, Launching, Suppressed, Attention };
 static constexpr std::int64_t MiB=1024*1024;
 static constexpr std::int64_t NativeFloor=50*MiB;
 static constexpr std::int64_t NativePeriodicFloor=NativeFloor+50*MiB;
 static constexpr std::int64_t LowWater=NativePeriodicFloor+256*MiB;
 static constexpr std::int64_t ResumeWater=NativePeriodicFloor+512*MiB;
 static constexpr std::int64_t MemoryCommitReserve=1024*MiB;
 static constexpr std::int64_t MemoryPhysicalReserve=768*MiB;
 static constexpr unsigned RequiredHighProbes=3,MaximumStarts=3;
 static constexpr std::int64_t StableReadyResetMs=60000;
 std::uint64_t explicitStart(){++generation_;wanted_=true;state_=State::Idle;starts_=high_=0;readySince_=-1;return generation_;}
 void suppress(){++generation_;wanted_=false;state_=State::Suppressed;high_=0;readySince_=-1;}
 bool permits(std::uint64_t generation)const{return wanted_&&generation==generation_;}
 std::uint64_t generation()const{return generation_;}
 State state()const{return state_;}
 unsigned starts()const{return starts_;}
 bool waiting()const{return state_==State::WaitingDisk||state_==State::WaitingMemory||state_==State::Backoff;}
 void attention(){state_=State::Attention;high_=0;}
 void ready(std::int64_t now){
  if(!wanted_)return;
  state_=State::Running;
  if(readySince_<0)readySince_=now;
  if(now-readySince_>=StableReadyResetMs)starts_=0;
 }
 bool diskExit(bool knownDiskFull,bool sameContext,std::int64_t now){
  return resourceExit(knownDiskFull,sameContext,now,State::WaitingDisk);
 }
 bool memoryRecovery()const{return resourceWait_==State::WaitingMemory;}
 void deferMemoryLaunch(){
  if(state_==State::Launching&&memoryRecovery()){
   --starts_;
   high_=0;
   state_=resourceWait_;
  }
 }
 bool memoryExit(bool knownOutOfMemory,bool sameContext,std::int64_t now){
  return resourceExit(knownOutOfMemory,sameContext,now,State::WaitingMemory);
 }
 // One fresh observation per completed probe. Unknown space/presence never
 // authorizes a launch. A changed identity closes this recovery episode.
 bool probe(std::int64_t now,bool fresh,bool sameContext,bool ownerAbsent,std::int64_t available,bool memoryReady=false){
  if(!wanted_||!waiting())return false;
  if(!sameContext){attention();return false;}
  if(now<=lastProbe_)return false;
  lastProbe_=now;
  if(!fresh||!ownerAbsent||available<ResumeWater||
     (resourceWait_==State::WaitingMemory&&!memoryReady)){
   high_=0;state_=resourceWait_;return false;
  }
  high_=std::min(high_+1,RequiredHighProbes);
  if(now<nextAttempt_){state_=State::Backoff;return false;}
  if(high_<RequiredHighProbes)return false;
  if(starts_>=MaximumStarts){attention();return false;}
  ++starts_;state_=State::Launching;return true;
 }
private:
 bool resourceExit(bool confirmed,bool sameContext,std::int64_t now,State wait){
  readySince_=-1;high_=0;lastProbe_=-1;
  if(!wanted_){state_=State::Suppressed;return false;}
  if(!confirmed||!sameContext||starts_>=MaximumStarts){attention();return false;}
  static constexpr std::int64_t delays[]{5000,15000,30000};
  nextAttempt_=now+delays[starts_];
  resourceWait_=state_=wait;
  return true;
 }
 State state_{State::Idle},resourceWait_{State::WaitingDisk};
 bool wanted_{};
 std::uint64_t generation_{};unsigned starts_{},high_{};
 std::int64_t nextAttempt_{},lastProbe_{-1},readySince_{-1};
};

enum class CoreDatabaseFailure { None, Chainstate, BlockIndex };

// A terminal startup message written by Core. The caller supplies only
// bytes appended during the observed run; the path itself is never displayed.
inline bool coreRefusedDataDirectory(std::string_view output){
 constexpr std::string_view prefix="Error: Specified data directory \"";
 constexpr std::string_view suffix="\" does not exist.";
 while(!output.empty()){
  const auto newline=output.find('\n');
  auto line=output.substr(0,newline);
  if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
  if(line.size()>prefix.size()+suffix.size()&&line.starts_with(prefix)&&line.ends_with(suffix))return true;
  if(newline==std::string_view::npos)break;
  output.remove_prefix(newline+1);
 }
 return false;
}

// Bounded incremental evidence from this owned run only. The caller binds the
// file identity/offset and invalidates evidence on rotation, truncation or loss.
class CoreFaultEvidence {
public:
 void invalidate(){usable_=false;}
 void feed(std::string_view bytes,bool final=false){
  if(!usable_)return;
  pending_.append(bytes);
  std::size_t end;
  while((end=pending_.find('\n'))!=std::string::npos){line(std::string_view(pending_).substr(0,end));pending_.erase(0,end+1);}
  if(pending_.size()>8192){invalidate();return;}
  if(final&&!pending_.empty()){line(pending_);pending_.clear();}
 }
 bool knownOutOfMemory()const{return usable_&&memory_&&!disk_&&!databaseCorruption_&&!otherFatal_;}
 bool knownDiskFull()const{return usable_&&disk_&&!memory_&&!otherFatal_&&!databaseCorruption_&&shutdown_;}
 CoreDatabaseFailure databaseFailure()const{
  return usable_&&!disk_&&!memory_&&!otherFatal_&&databaseCorruption_&&shutdown_?databaseFailure_:CoreDatabaseFailure::None;
 }
 bool usable()const{return usable_;}
private:
 void line(std::string_view text){
  if(!text.empty()&&text.back()=='\r')text.remove_suffix(1);
  const auto prefix=[&](std::string_view s){return text.substr(0,s.size())==s;};
  const auto space=text.find(' ');
  // Only the native timestamp/severity envelope is stripped. A marker quoted
  // in arbitrary log text is not a shutdown-cause witness.
  const bool stamped=space>=20&&space<=40&&text.size()>space&&text[4]=='-'&&text[7]=='-'&&text[10]=='T'&&text[space-1]=='Z';
  if(stamped)text.remove_prefix(space+1);
  const bool error=prefix("[error] ");
  if(prefix("[error] "))text.remove_prefix(8);
  else if(prefix("[info] "))text.remove_prefix(7);
  if(prefix("Error: "))text.remove_prefix(7);
  if(text.empty())return;
  memory_=stamped&&error&&text=="Out of memory. Terminating.";
  const bool disk=text=="Shutting down due to lack of disk space!"||text=="Disk space is too low!"||
   text=="A fatal internal error occurred, see debug.log for details: Disk space is too low!";
  if(disk){disk_=true;shutdown_=false;}
  if((prefix("A fatal internal error occurred")||prefix("Corrupt block found")||prefix("System error while "))&&!disk)otherFatal_=true;
  if(prefix("Fatal LevelDB error: Corruption:")){
   databaseCorruption_=true;shutdown_=false;
  }
  if(databaseCorruption_){
   CoreDatabaseFailure type=CoreDatabaseFailure::None;
   if(text=="Error opening coins database.")type=CoreDatabaseFailure::Chainstate;
   else if(text=="Error opening block database.")type=CoreDatabaseFailure::BlockIndex;
   if(type!=CoreDatabaseFailure::None){
    if(databaseFailure_!=CoreDatabaseFailure::None&&databaseFailure_!=type)otherFatal_=true;
    databaseFailure_=type;
   }
  }
  if(text=="Block index and chainstate loaded"){
   databaseCorruption_=false;
   databaseFailure_=CoreDatabaseFailure::None;
  }
  if(text=="Shutdown done"&&(disk_||databaseCorruption_))shutdown_=true;
 }
 bool usable_{true},disk_{},otherFatal_{},shutdown_{},databaseCorruption_{},memory_{};
 CoreDatabaseFailure databaseFailure_{CoreDatabaseFailure::None};
 std::string pending_;
};
}
