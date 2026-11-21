#include "mining/network_controller_recovery.h"
#include "mining/worker_control_arguments.h"
#include "ui/mining_attention_status.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QThread>
#include <QProcess>
#include <iostream>
#include <stdexcept>
#include <vector>
static void need(bool okay,const char* why){if(!okay)throw std::runtime_error(why);}
int main(int argc,char** argv)try{
 if(argc>1&&QString::fromLocal8Bit(argv[1])=="--transport"){std::string line;while(std::getline(std::cin,line))if(line=="quit")return 255;return 0;}
 QCoreApplication app(argc,argv);
 need(pgn::ui::miningAttentionStatus("Audit 2: Active parent changed after submission").contains("Network changed")&&!pgn::ui::miningAttentionStatus("Active parent changed").contains("retrying"),"Failure text claimed a retry without observing one");
 need(pgn::ui::miningAttentionStatus("Session index WAL budget reached").contains("storage unavailable")&&!pgn::ui::miningAttentionStatus("Session index WAL budget reached").contains("saved"),"Storage failure claimed an unverified durable save");
 need(pgn::ui::miningAttentionStatus("Invalid audit actor signature").contains("rejected"),"Signature rejection was hidden as success");
 need(pgn::ui::miningAttentionStatus({}).contains("Diagnostics"),"Missing diagnostic reason had no recovery route");
 const QStringList original{"-o","StrictHostKeyChecking=yes","-o","ServerAliveInterval=0","user@host","remote-program","--network"};
 const auto hardened=pgn::mining::workerControlArguments("C:\\Windows\\System32\\OpenSSH\\ssh.exe",original);
 need(hardened==QStringList({"-o","ServerAliveInterval=5","-o","ServerAliveCountMax=2","-o","ConnectTimeout=5"})+original,"SSH timeout arguments changed command/authentication boundary");
 need(pgn::mining::workerControlArguments("/usr/bin/ssh",original)==hardened,"Linux SSH did not receive protocol probes");
 need(pgn::mining::workerControlArguments("C:/PGN/horizon-engine-host.exe",original)==original,"Local worker arguments changed");
QProcess process;bool allowed=true;std::vector<bool> launches;int disconnected=0;
 pgn::mining::NetworkControllerRecovery recovery(&process,nullptr,20,100);
 recovery.mayReconnect=[&]{return allowed;};recovery.disconnected=[&]{++disconnected;};
 recovery.reconnect=[&](bool wanted){launches.push_back(wanted);recovery.launching();process.setProgram(QCoreApplication::applicationFilePath());process.setArguments({"--transport"});process.start();};
 const auto wait=[&](auto condition,int milliseconds=2000){QElapsedTimer clock;clock.start();while(!condition()&&clock.elapsed()<milliseconds){app.processEvents();QThread::msleep(2);}need(condition(),"Recovery did not reach bounded expected state");};
 recovery.intent(true);need(recovery.restartIfClosed(),"ON could not start an ended control transport");wait([&]{return process.state()==QProcess::Running;});need(launches.size()==1&&launches.back(),"User intent was lost");recovery.phase("Searching",true);
 process.write("quit\n");wait([&]{return launches.size()==2&&process.state()==QProcess::Running;});need(disconnected==1&&launches.back(),"Broken transport was not automatically restored");recovery.phase("Searching",true);
 recovery.intent(false);process.write("quit\n");wait([&]{return launches.size()==3&&process.state()==QProcess::Running;});need(!launches.back(),"OFF reconnect accidentally requested mining");recovery.phase("Paused",true);
 recovery.phase("Needs attention",true);process.write("quit\n");wait([&]{return process.state()==QProcess::NotRunning;});QElapsedTimer quiet;quiet.start();while(quiet.elapsed()<160){app.processEvents();QThread::msleep(2);}need(launches.size()==3&&!recovery.retryPending(),"Scientific or terminal fault was silently retried");
 recovery.intent(true);allowed=false;need(!recovery.restartIfClosed()&&launches.size()==3,"Another owner/closing guard was ignored");allowed=true;need(recovery.restartIfClosed(),"Explicit retry did not clear a terminal fault");wait([&]{return process.state()==QProcess::Running;});need(launches.size()==4,"Manual retry duplicated launch");
 recovery.phase("Needs attention",false);need(recovery.needsAttention(),"Fault did not remain visible");
 recovery.phase("Searching",false);need(recovery.needsAttention(),"Stale frame cleared fault");
 recovery.phase("Paused",true);need(recovery.needsAttention(),"Pause acknowledgement cleared fault");
 recovery.phase("Searching",true);need(!recovery.needsAttention()&&recovery.wanted(),"Fresh running work stayed hidden until OFF/ON");
 recovery.intent(false);recovery.phase("Needs attention",false);recovery.phase("Searching",true);
 need(recovery.needsAttention()&&!recovery.wanted(),"Late running observation overrode explicit OFF");
 recovery.close();process.write("quit\n");wait([&]{return process.state()==QProcess::NotRunning;});need(!recovery.retryPending(),"Closing restarted a control process");
 QProcess silent;pgn::mining::NetworkControllerRecovery silence(&silent,nullptr,20,100,60);int silences=0,starts=0;
 silence.onSilent=[&]{++silences;};silence.reconnect=[&](bool){++starts;silence.launching();silent.start(QCoreApplication::applicationFilePath(),{"--transport"});};
 silence.intent(true);need(silence.restartIfClosed(),"Silent fixture could not start");wait([&]{return silent.state()==QProcess::Running;});
 silence.phase("Searching",true);wait([&]{return silences==1;});
 need(starts==1&&silent.state()==QProcess::Running&&silence.wanted(),"Silence duplicated or killed a worker / lost ON");
 silence.phase("Paused",true);silence.intent(false);
 quiet.restart();while(quiet.elapsed()<160){app.processEvents();QThread::msleep(2);}
 need(!silence.wanted()&&starts==1&&silences==1,"An already stopped controller reported a live-data delay");
 silence.close();silent.write("quit\n");wait([&]{return silent.state()==QProcess::NotRunning;});
 QProcess stream;
 pgn::mining::NetworkControllerRecovery watchdog(&stream,nullptr,20,100,65);
 bool awaitingStop=false;
 int shown=0,cleared=0;
 watchdog.awaitingStop=[&]{return awaitingStop;};
 watchdog.onSilent=[&]{++shown;};
 watchdog.onSilenceCleared=[&]{++cleared;};
 watchdog.reconnect=[&](bool){watchdog.launching();stream.start(QCoreApplication::applicationFilePath(),{"--transport"});};
 watchdog.intent(true);
 need(watchdog.restartIfClosed(),"A dead transport could not start");
 wait([&]{return stream.state()==QProcess::Running;});
 watchdog.phase("Searching",true);
 wait([&]{watchdog.phase("Searching",false);return shown==1;});
 for(const auto& phase:QStringList{"Searching","Completing audits","Submitting reports","Waiting for storage","Report ready",""})
  watchdog.phase(phase,false);
 need(shown==1&&cleared==0,"Cached or phase-only observations cleared the live-data notice");
 watchdog.phase("Searching",true);
 need(cleared==1,"New live observations did not clear the notice");
 watchdog.intent(false);
 awaitingStop=true;
 need(shown==1,"A stop request reused an earlier waiting period");
 wait([&]{watchdog.phase("Searching",false);watchdog.intent(false);return shown==2;});
 watchdog.phase("Paused",false);
 watchdog.stopConfirmed();
 awaitingStop=false;
 need(cleared==2,"A phase-only stop confirmation did not clear the notice");
 quiet.restart();
 while(quiet.elapsed()<160){app.processEvents();QThread::msleep(2);}
 need(shown==2,"An idle OFF observer produced an alert");
 QElapsedTimer newStopClock;
 newStopClock.start();
 watchdog.intent(false,true);
 awaitingStop=true;
 wait([&]{return shown==3;});
 need(newStopClock.elapsed()>=55,"A new OFF request reused the idle transport age");
 watchdog.stopConfirmed();
 awaitingStop=false;
 need(cleared==3,"A new stop acknowledgement did not clear the notice");
 watchdog.intent(true);
 wait([&]{return shown==4;});
 watchdog.phase("Needs attention",false);
 need(cleared==4&&watchdog.needsAttention(),"An important error competed with a stale live-data notice");
 watchdog.close();stream.write("quit\n");
 wait([&]{return stream.state()==QProcess::NotRunning;});
 QProcess absent;pgn::mining::NetworkControllerRecovery failed(&absent,nullptr,20,100);int failedStarts=0;failed.reconnect=[&](bool){++failedStarts;failed.launching();absent.setProgram(QCoreApplication::applicationFilePath()+".missing");absent.start();};failed.intent(true);need(failed.restartIfClosed(),"Failed-start fixture not dispatched");wait([&]{return failed.needsAttention();});need(failedStarts==1&&!failed.retryPending(),"Missing executable entered a retry loop");failed.close();
 for(const auto& waitingPhase:QStringList{"Waiting for controller","Waiting for connection"})for(const bool wanted:{true,false}){
  QProcess contended;pgn::mining::NetworkControllerRecovery waiting(&contended,nullptr,20,100);int attempts=0;std::vector<bool> intents;
  waiting.reconnect=[&](bool on){++attempts;intents.push_back(on);waiting.launching();contended.setProgram(QCoreApplication::applicationFilePath());contended.setArguments({"--transport"});contended.start();};
  QObject::connect(&contended,&QProcess::started,&app,[&]{if(attempts<=2){waiting.phase(waitingPhase,true);contended.write("quit\n");}else waiting.phase("Searching",true);});
  waiting.intent(wanted);need(waiting.restartIfClosed(),"Contended transport did not start");wait([&]{return attempts==3&&contended.state()==QProcess::Running;});
  need(!waiting.needsAttention()&&intents==std::vector<bool>(3,wanted),"Temporary owner contention lost intent or became terminal");
  waiting.close();contended.write("quit\n");wait([&]{return contended.state()==QProcess::NotRunning;});need(!waiting.retryPending(),"Closed busy transport restarted");
 }
 for(const bool guardClosesBeforeExit:{true,false}){
  QProcess gated;bool eligible=true;std::vector<bool> intents;
  pgn::mining::NetworkControllerRecovery guarded(&gated,nullptr,20,100);
  guarded.mayReconnect=[&]{return eligible;};
  guarded.reconnect=[&](bool on){intents.push_back(on);guarded.launching();gated.start(QCoreApplication::applicationFilePath(),{"--transport"});};
  guarded.intent(true);need(guarded.restartIfClosed(),"Guarded reconnect fixture could not start");
  wait([&]{return gated.state()==QProcess::Running;});
  if(guardClosesBeforeExit)eligible=false;
  gated.write("quit\n");wait([&]{return gated.state()==QProcess::NotRunning;});
  eligible=false;
  QElapsedTimer held;held.start();while(held.elapsed()<140){app.processEvents();QThread::msleep(2);}
  need(intents.size()==1,"The transport started while the application guard was closed");
  need(guarded.retryPending(),"The transport lost its retry while the application guard was temporarily closed");
  guarded.intent(false);need(!guarded.restartIfClosed(),"An OFF request bypassed the application guard");
  need(guarded.retryPending(),"An OFF request lost its deferred delivery");
  eligible=true;
  wait([&]{return intents.size()==2&&gated.state()==QProcess::Running;});
  need(!intents.back(),"Deferred reconnect forgot the latest OFF request");
  guarded.close();gated.write("quit\n");wait([&]{return gated.state()==QProcess::NotRunning;});
  need(intents.size()==2&&!guarded.retryPending(),"The application close left a deferred controller retry");
 }
 std::cout<<"PASS owned transport recovery:ON after closed,automatic reconnect,OFF intent,semantic faults,ownership guard,manual retry,close, failed-start and deferred gate release; no GPU or Core touched\n";
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}
