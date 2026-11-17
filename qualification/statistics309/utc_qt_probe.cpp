#include "social/social_data.h"
#include <pgn/leaderboard_utc_period.h>
#include <QCoreApplication>
#include <iostream>
int main(int argc,char** argv){QCoreApplication app(argc,argv);std::string mode,name;int64_t value;
 while(std::cin>>mode>>name>>value){try{
  const auto native=pgn::core::ParseLeaderboardPeriod(name);if(!native)throw std::invalid_argument("Unknown period");
  const auto period=mode=="at"?pgn::core::LeaderboardUTCWindow::At(*native,value):pgn::core::LeaderboardUTCWindow::FromStart(value,*native);
  std::optional<pgn::social::Window> selected;
  for(const auto window:{pgn::social::Window::Day,pgn::social::Window::Week,pgn::social::Window::Month,pgn::social::Window::Hour})if(pgn::social::leaderboardPeriodName(window).toStdString()==name)selected=window;
  if(!selected)throw std::invalid_argument("Qt period missing");
  const auto end=pgn::social::leaderboardWindowEnd(*selected,*period.utc_start);
  if(!end||*end!=period.End()||!pgn::social::validLeaderboardPeriod({QString(64,'a'),0,*period.utc_start},*selected))return 3;
  std::cout<<*period.utc_start<<' '<<*end<<'\n';
 }catch(const std::exception&){std::cout<<"ERR\n";}}
}
