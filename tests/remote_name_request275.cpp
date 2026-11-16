#include "ui/social_remote_name_signer.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <iostream>
int main(int argc,char** argv){
 QCoreApplication app(argc,argv);if(argc!=2)return 2;QFile file(QString::fromLocal8Bit(argv[1]));
 if(!file.open(QIODevice::ReadOnly)||file.size()>32768)return 3;
 const auto request=QJsonDocument::fromJson(file.readAll()).object();
 const pgn::social::Identity identity{request["genesis"].toString(),request["social_script"].toString()};
 QElapsedTimer clock;clock.start();QObject owner;bool completed{};
 pgn::ui::requestRemoteNameConsent(&owner,request["profile"].toString(),identity,pgn::social::NameLinkSide::Native,request["native_script"].toString(),request["certificate"].toString().toUtf8(),[]{return true;},[&](QString signature,QString error){
  completed=true;const QJsonObject result{{"PASS",error.isEmpty()&&!signature.isEmpty()},{"signature",signature},{"error",error},{"elapsed_ms",clock.elapsed()}};
  std::cout<<QJsonDocument(result).toJson(QJsonDocument::Compact).constData()<<std::endl;const int exit=error.isEmpty()?0:1;QTimer::singleShot(0,&app,[&app,exit]{app.exit(exit);});
 });
 QTimer::singleShot(30000,&app,[&]{if(!completed)app.exit(4);});return app.exec();
}
