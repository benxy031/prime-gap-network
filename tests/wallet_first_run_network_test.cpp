#include "wallet/core_runtime.h"
#include "wallet/core_bundle.h"
#include "wallet/setup_deployment.h"
#include "ui/wallet_management_panel.h"
#include <QApplication>
#include <QLineEdit>
#include <QPushButton>
#include <QTimer>
#include <QFile>
#include <QJsonDocument>
#include <iostream>

int main(int argc,char** argv){
 QApplication app(argc,argv);if(argc!=2)return 2;
 QFile input(QString::fromLocal8Bit(argv[1]));if(!input.open(QIODevice::ReadOnly)||input.size()>32768)return 3;
 const auto cfg=QJsonDocument::fromJson(input.readAll()).object();
 const auto root=cfg.value("application_root").toString(),config=cfg.value("runtime_directory").toString(),data=cfg.value("data_directory").toString();
 const auto bundle=pgn::wallet::readCoreBundle(root,"test");if(!bundle.error.isEmpty()){std::cerr<<bundle.error.toStdString();return 4;}
 pgn::wallet::CoreRuntime runtime(root,nullptr,true,config);pgn::ui::WalletManagementPanel panel(runtime.walletManager());
 panel.present("",{},false);panel.show();
 bool okay=false,stopped=false,ready=false,requesting=false;int height=0;QString status;QJsonArray statusHistory;
 runtime.onStatus=[&](const QString& s){status=s;if(statusHistory.isEmpty()||statusHistory.last()!=s)statusHistory.append(s);while(statusHistory.size()>16)statusHistory.removeFirst();};
 runtime.onReady=[&](qint64){ready=true;};
 runtime.onStopped=[&]{stopped=true;app.quit();};
 runtime.onObservation=[&](const std::optional<pgn::wallet::ChainObservation>& observed){
  if(!observed)return;height=std::max(height,int(observed->height));
  if(!ready||requesting||height<cfg.value("minimum_height").toInt()||!observed->nativePeerCaughtUp||!observed->peers||*observed->peers<1||!observed->coreInitialBlockDownload||*observed->coreInitialBlockDownload)return;
  requesting=true;runtime.walletRequest("getwalletinfo",{},[&](QJsonValue value,QString error){okay=error.isEmpty()&&value.toObject().value("walletname")=="network_fixture";runtime.stop();});
 };
 panel.onApplySetup=[&](const pgn::wallet::WalletSetupSelection& selected){
  auto deployment=bundle.deployment;deployment.insert("rpc_port",48974);deployment.insert("peer_port",48975);
  const auto selection=pgn::wallet::selectedWalletDeployment(deployment,selected,root);
  const auto error=pgn::wallet::publishWalletSetup(config,selection);panel.finishSetup(error);
  if(!error.isEmpty()){status=error;app.quit();return;}runtime.launch();
 };
 panel.findChild<QLineEdit*>("pgn-wallet-data-directory")->setText(data);
 panel.findChild<QLineEdit*>("pgn-wallet-name")->setText("network_fixture");
 QTimer::singleShot(0,&panel,[&]{panel.findChild<QPushButton*>("pgn-wallet-create-initial")->click();});
 QTimer::singleShot(90000,&app,[&]{runtime.stop();QTimer::singleShot(8000,&app,&QCoreApplication::quit);});
 app.exec();
 const QJsonObject result{{"PASS",okay&&stopped},{"native_wallet_ready",ready},{"observed_height",height},{"minimum_height",cfg.value("minimum_height")},{"peer_caught_up_required",true},{"ibd_false_required",true},{"graceful_stop",stopped},{"last_status",status},{"status_history",statusHistory},{"scope","Actual wallet panel Create click -> bundled CoreRuntime -> public TLS/P2P sync; isolated data, no GPU or Robert machine"}};
 std::cout<<QJsonDocument(result).toJson(QJsonDocument::Compact).constData()<<'\n';return okay&&stopped?0:5;
}
