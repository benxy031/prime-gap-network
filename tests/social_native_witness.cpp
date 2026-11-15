// Disposable integration harness only. Two actual Qt processes use the product
// CoreSocialService against isolated native Core wallets. Never a product RPC bridge.
#include "ui/social_view.h"
#include "ui/social_chat_list.h"
#include "social/social_core_service.h"
#include "social/social_leaderboard.h"
#include "social/social_signing_contract.h"
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QLabel>
#include <QTimer>
#include <QScrollBar>
#include <QKeyEvent>
#include <iostream>
#include <thread>
using namespace pgn;
static void output(QJsonObject o){std::cout<<QJsonDocument(o).toJson(QJsonDocument::Compact).constData()<<std::endl;}
class Fixture final:public QObject {
    QNetworkAccessManager network_{this};QJsonObject config_;int signed_{},verified_{};
public:
    std::unique_ptr<social::CoreSocialService> service;std::unique_ptr<ui::SocialView> view;
    explicit Fixture(QJsonObject config):config_(std::move(config)){open();}
    void rpc(QString method,QJsonArray params,bool wallet,std::function<void(QJsonValue,QString)> done){
        QFile cookie(config_["cookie_file"].toString());if(!cookie.open(QIODevice::ReadOnly)){done({},"Fixture cookie unavailable");return;}
        QNetworkRequest request(QUrl("http://127.0.0.1:"+QString::number(config_["rpc_port"].toInt())+(wallet?"/wallet/"+config_["wallet"].toString():QString{})));request.setTransferTimeout(5000);
        request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");request.setRawHeader("Authorization","Basic "+cookie.readAll().trimmed().toBase64());
        auto* reply=network_.post(request,QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"id","social45-owned-test"},{"method",method},{"params",params}}).toJson(QJsonDocument::Compact));
        connect(reply,&QNetworkReply::finished,this,[reply,done]{const auto raw=reply->readAll();const auto error=reply->error();reply->deleteLater();const auto o=QJsonDocument::fromJson(raw).object();
            if(raw.size()>1048576||error!=QNetworkReply::NoError||(!o["error"].isNull()&&!o["error"].isUndefined())){done({},"Isolated native RPC refused");return;}done(o["result"],{});});
    }
    void open(){
        social::CoreSocialContext context{{config_["genesis"].toString(),config_["script"].toString()},config_["address"].toString(),config_["device"].toString(),"global","Isolated Core20 test network"};
        social::CoreSocialCrypto crypto;
        crypto.sign=[this,context](QByteArray source,auto done){
            if(!social::signingRequestError({context.identity.genesis,context.identity.subject,context.address},source).isEmpty()){done({},"Invalid signing scope");return;}
            rpc("getaddressinfo",{context.address},true,[this,context,source,done](QJsonValue value,QString error){const auto owner=value.toObject();if(!error.isEmpty()||!owner["ismine"].toBool()||owner["scriptPubKey"].toString()!=context.identity.subject){done({},"Fixture owner mismatch");return;}
                rpc("signmessage",{context.address,QString::fromUtf8(source)},true,[this,done](QJsonValue signature,QString error){if(error.isEmpty())++signed_;done(signature.toString(),error);});});
        };
        crypto.verify=[this,context](QString script,QByteArray source,QString signature,auto done){
            const auto body=QJsonDocument::fromJson(source.mid(source.indexOf('\n')+1)).object();const auto address=body["signing_address"].toString();
            if(!social::signingRequestError({context.identity.genesis,script,address},source,QDateTime::currentMSecsSinceEpoch(),social::EnvelopeUse::StoredVerification).isEmpty()){done(false,"Invalid stored verification scope");return;}
            rpc("getdescriptorinfo",{"raw("+script+")"},false,[this,address,source,signature,done](QJsonValue value,QString error){const auto descriptor=value.toObject()["descriptor"].toString();if(!error.isEmpty()||descriptor.isEmpty()){done(false,"Descriptor unavailable");return;}
                rpc("deriveaddresses",{descriptor},false,[this,address,source,signature,done](QJsonValue value,QString error){const auto addresses=value.toArray();if(!error.isEmpty()||addresses.size()!=1||addresses[0].toString()!=address){done(false,"Native script/address mismatch");return;}
                    rpc("verifymessage",{address,signature,QString::fromUtf8(source)},false,[this,done](QJsonValue valid,QString error){if(error.isEmpty()&&valid.toBool())++verified_;done(error.isEmpty()&&valid.toBool(),error);});});});
        };
        social::CoreSocialRpc transport=[this](social::CoreSocialCall call,QJsonArray params,auto done){const QString method=call==social::CoreSocialCall::SubmitEvent?"submitsocialevent":call==social::CoreSocialCall::ReadEvents?"getsocialevents":"getsocialpeers";rpc(method,params,false,done);};
        service=std::make_unique<social::CoreSocialService>(context,std::move(crypto),std::move(transport),config_["state"].toString()+"/outbox.json");
        service->leaderboard=[this](social::ReadRequest request,auto done){QJsonArray args;if(!social::leaderboardArguments(request,args)){done({},"Invalid leaderboard request");return;}
            rpc("getpgnleaderboard",args,false,[request,done](QJsonValue value,QString error){social::Snapshot snapshot;if(error.isEmpty())error=social::decodeLeaderboard(request,value.toObject(),snapshot);done(std::move(snapshot),error);});};
        view=std::make_unique<ui::SocialView>(config_["state"].toString()+"/drafts.json",service->services());view->setWindowTitle("PGN Social · isolated native QA "+config_["label"].toString());view->resize(1366,994);view->show();
        service->changed=[this]{view->refresh();};service->diagnostic=[](QString){};service->start();view->setIdentity(context.identity);
    }
    void command(QJsonObject command){
        const QJsonValue id=command.value("id");const auto kind=command["command"].toString();
        if(kind=="snapshot"){
            service->services().read({social::Section::Chat,service->identity()},[this,id](social::Snapshot snapshot,QString error){QJsonArray entries;for(const auto& entry:snapshot.entries)entries.append(QJsonObject{{"id",entry.id},{"body",entry.body},{"author",entry.author},{"author_id",entry.authorId}});
                auto* list=static_cast<ui::SocialChatList*>(view->findChild<QListWidget*>("pgn-social-list-0"));auto* anchor=list->itemAt(QPoint(20,20));output({{"id",id},{"error",error},{"messages",entries},{"signed",signed_},{"verified",verified_},{"can_send",view->findChild<QPushButton*>("pgn-social-send-0")->isEnabled()},{"scroll",list->verticalScrollBar()->value()},{"maximum",list->verticalScrollBar()->maximum()},{"anchor",anchor?anchor->data(Qt::UserRole).toString():QString{}},{"anchor_top",anchor?list->visualItemRect(anchor).top():0},{"unseen",view->findChild<QPushButton*>("pgn-social-new-messages")->isVisible()}});});return;
        }
        if(kind=="send"){auto* edit=view->findChild<QPlainTextEdit*>("pgn-social-draft-body-0");edit->setPlainText(command["text"].toString());view->findChild<QPushButton*>("pgn-social-send-0")->click();output({{"id",id},{"clicked",true}});return;}
        if(kind=="capture"){
            view->setFixedSize(command["width"].toInt(1366),command["height"].toInt(994));QTimer::singleShot(100,view.get(),[this,id,command]{const auto ok=view->grab().save(command["path"].toString());output({{"id",id},{"saved",ok},{"width",view->width()},{"height",view->height()}});});return;
        }
        if(kind=="reopen"){view.reset();service.reset();open();output({{"id",id},{"reopened",true}});return;}
        if(kind=="draft"){view->findChild<QPlainTextEdit*>("pgn-social-draft-body-0")->setPlainText(command["text"].toString());view->saveDrafts();output({{"id",id},{"saved",true}});return;}
        if(kind=="draft_read"){output({{"id",id},{"text",view->findChild<QPlainTextEdit*>("pgn-social-draft-body-0")->toPlainText()}});return;}
        if(kind=="scroll_older"){auto* list=view->findChild<QListWidget*>("pgn-social-list-0");list->setFocus();QKeyEvent press(QEvent::KeyPress,Qt::Key_Home,Qt::NoModifier);QApplication::sendEvent(list,&press);QKeyEvent release(QEvent::KeyRelease,Qt::Key_Home,Qt::NoModifier);QApplication::sendEvent(list,&release);output({{"id",id},{"scrolled",true}});return;}
        if(kind=="jump_latest"){view->findChild<QPushButton*>("pgn-social-new-messages")->click();output({{"id",id},{"jumped",true}});return;}
        if(kind=="leaderboard"){view->selectSection(social::Section::Miners);output({{"id",id},{"selected",true}});return;}
        if(kind=="stop"){view.reset();service.reset();output({{"id",id},{"stopped",true}});QApplication::quit();return;}
        output({{"id",id},{"error","Unknown harness command"}});
    }
};
int main(int argc,char** argv){QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);if(argc!=2)return 2;QFile file(QString::fromLocal8Bit(argv[1]));if(!file.open(QIODevice::ReadOnly))return 2;Fixture fixture(QJsonDocument::fromJson(file.readAll()).object());
    std::thread([&fixture]{std::string line;while(std::getline(std::cin,line)){const auto command=QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();QMetaObject::invokeMethod(&fixture,[&fixture,command]{fixture.command(command);},Qt::QueuedConnection);}}).detach();
    return app.exec();}
