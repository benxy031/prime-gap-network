#include <pgn/leaderboard_utc_period.h>
#include <iostream>
#include <string>
using namespace pgn::core;
int main(){std::string mode,name;int64_t value;
 while(std::cin>>mode>>name>>value){try{
  const auto period=ParseLeaderboardPeriod(name);if(!period)throw std::invalid_argument("Unknown period");
  const auto w=mode=="at"?LeaderboardUTCWindow::At(*period,value):LeaderboardUTCWindow::FromStart(value,*period);
  if(w.Contains(*w.utc_start-1)||!w.Contains(*w.utc_start)||!w.Contains(w.End()-1)||w.Contains(w.End()))return 2;
  std::cout<<*w.utc_start<<' '<<w.End()<<'\n';
 }catch(const std::exception&){std::cout<<"ERR\n";}}
}
