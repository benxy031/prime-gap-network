#include "ui/social_view.h"
#include <QApplication>
#include <QTableWidget>
#include <QPushButton>
#include <QTemporaryDir>
#include <QEventLoop>
#include <iostream>
int main(int argc,char** argv){
 QApplication app(argc,argv);QTemporaryDir directory;int count=2;unsigned checks{};
 const auto check=[&](bool ok,const char* why){++checks;if(!ok)throw std::runtime_error(why);};
 pgn::social::Services services;services.read=[&](pgn::social::ReadRequest r,auto done){
  pgn::social::Snapshot s;s.identity=r.identity;s.section=r.section;s.window=r.window;s.source="SYNTHETIC GEOMETRY TEST ONLY; no network data";s.authenticated=true;s.observedUtc=QDateTime::currentDateTimeUtc();
  if(r.section==pgn::social::Section::Miners)for(int i=0;i<count;++i){pgn::social::Miner m;m.id=QString("test-%1").arg(i);m.name="Synthetic geometry fixture";s.miners.append(m);}done(std::move(s),{});
 };
 pgn::ui::SocialView view(directory.path()+"/drafts.json",std::move(services));view.setIdentity({QString(64,'1'),"0014"+QString(40,'2')});view.selectSection(pgn::social::Section::Miners);view.show();
 try{
  for(int rows:{2,18})for(auto size:{QSize(1366,900),QSize(1920,1080)}){
   count=rows;view.resize(size);view.refresh();for(int turn=0;turn<8;++turn)app.processEvents(QEventLoop::AllEvents);
   auto* table=view.findChild<QTableWidget*>("pgn-social-miners");auto* footer=view.findChild<QPushButton*>("pgn-social-more-3");
   check(table&&table->rowCount()==rows,"Fixture rows missing");check(table->height()==table->parentWidget()->height(),"Table did not fill its allocated slot");check(footer&&qAbs(footer->mapTo(&view,QPoint()).y()-table->mapTo(&view,QPoint(0,table->height())).y())<=12,"Unused gap between table and footer");check(table->width()>=view.width()-80,"Table did not use available width");
   check(footer&&footer->mapTo(&view,QPoint()).y()>=view.height()-100,"Pagination did not stay at bottom");
   for(int i=0;i<rows;++i)check(table->rowHeight(i)==36,"Rows stretched to fill spare space");
   check(table->horizontalScrollBarPolicy()==Qt::ScrollBarAsNeeded,"Horizontal clipping policy changed");
  }
 }catch(const std::exception& e){std::cerr<<e.what()<<" after "<<checks<<"\n";return 1;}
 std::cout<<"SOCIAL AVAILABLE HEIGHT PASS "<<checks<<"; geometry fixture only\n";return 0;
}
