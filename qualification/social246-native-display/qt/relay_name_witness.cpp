#include "current_display_consent.h"
#include "native_display_resolver.h"
#include "ui/social_name_link_dialog.h"
// Disposable integration harness only. Two actual Qt processes use the product
// CoreSocialService against isolated native Core wallets. Never a product RPC bridge.
#include "ui/social_view.h"
#include "ui/social_presentation.h"
#include <QLabel>
#include "ui/social_chat_list.h"
#include "social/social_core_service.h"
#include "ui/social_compose_edit.h"
#include <QMenu>
#include <QTableWidget>
#include <QAbstractEventDispatcher>
#include <QElapsedTimer>
#include "ui/social_entry_delegate.h"
#include <QLayout>
#include <QContextMenuEvent>
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
#include <QLineEdit>
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
    bool tracing{};QJsonArray scrollEvents;QElapsedTimer scrollClock;
    QJsonObject scrollState(){
      if(!view)return {};
      auto* list=static_cast<ui::SocialChatList*>(view->findChild<QListWidget*>("pgn-social-list-0"));if(!list)return {};auto* at=list->itemAt(QPoint(20,20));QJsonObject state{{"anchor",at?at->data(Qt::UserRole).toString():QString{}},{"anchor_top",at?list->visualItemRect(at).top():0},{"scroll",list->verticalScrollBar()->value()},{"maximum",list->verticalScrollBar()->maximum()},{"rows",list->count()},{"follow",list->followingLatest()}};
      for(int row=0;row<list->count();++row){auto* item=list->item(row);const auto rect=list->visualItemRect(item);if(rect.top()>=0){state.insert("complete_anchor",item->data(Qt::UserRole).toString());state.insert("complete_top",rect.top());break;}}return state;
    }
    void traceScroll(QString kind){if(!tracing)return;auto state=scrollState();state.insert("kind",kind);state.insert("ms",scrollClock.elapsed());if(scrollEvents.size()>=256)scrollEvents.removeFirst();scrollEvents.append(state);}
    bool eventFilter(QObject* object,QEvent* event)override{if(tracing&&(event->type()==QEvent::LayoutRequest||event->type()==QEvent::Resize))traceScroll("event-"+QString::number(int(event->type())));return QObject::eventFilter(object,event);}
    void watchScroll(){auto* list=view->findChild<QListWidget*>("pgn-social-list-0");list->installEventFilter(this);list->viewport()->installEventFilter(this);connect(list->verticalScrollBar(),&QScrollBar::valueChanged,this,[this]{traceScroll("value");});connect(list->verticalScrollBar(),&QScrollBar::rangeChanged,this,[this]{traceScroll("range");});connect(list->model(),&QAbstractItemModel::modelReset,this,[this]{traceScroll("model-reset");});}
    explicit Fixture(QJsonObject config):config_(std::move(config)){open();displayRefresh.setInterval(500);connect(&displayRefresh,&QTimer::timeout,this,[this]{if(view&&view->isVisible())view->refresh();});displayRefresh.start();}
    void rpc(QString method,QJsonArray params,bool wallet,std::function<void(QJsonValue,QString)> done){
        QFile cookie(config_["cookie_file"].toString());if(!cookie.open(QIODevice::ReadOnly)){done({},"Fixture cookie unavailable");return;}
        QNetworkRequest request(QUrl("http://127.0.0.1:"+QString::number(config_["rpc_port"].toInt())+(wallet?"/wallet/"+config_["wallet"].toString():QString{})));request.setTransferTimeout(5000);
        request.setHeader(QNetworkRequest::ContentTypeHeader,"application/json");request.setRawHeader("Authorization","Basic "+cookie.readAll().trimmed().toBase64());
        auto* reply=network_.post(request,QJsonDocument(QJsonObject{{"jsonrpc","2.0"},{"id","social45-owned-test"},{"method",method},{"params",params}}).toJson(QJsonDocument::Compact));
        connect(reply,&QNetworkReply::finished,this,[reply,done]{const auto raw=reply->readAll();const auto error=reply->error();reply->deleteLater();const auto o=QJsonDocument::fromJson(raw).object();
            if(raw.size()>1048576||error!=QNetworkReply::NoError||(!o["error"].isNull()&&!o["error"].isUndefined())){done({},"Isolated native RPC refused");return;}done(o["result"],{});});
    }

    QPointer<pgn::ui::SocialNameLinkDialog> nameDialog;
    QPointer<pgn::social::CurrentDisplayConsent> nameClient;
    social::Services effectiveServices;
    QPointer<social::NativeDisplayResolver> resolver;
    QTimer displayRefresh{this};
    int consentSignatures{};
    void ensureDisplay(){
      if(nameClient)return;
      const auto identity=service->identity();
      social::PublicDisplayRpc port=[this](QString method,QJsonArray args,auto done){rpc(method,args,false,done);};
      // Synthetic fixture wallet only. Same scoped public-packet/ownership
      // checks as product; this harness does NOT execute CoreRuntime lifecycle.
      social::NameLinkWalletSigner signer=[this](social::Identity identity,social::NameLinkSide side,QString script,QByteArray source,auto done){
        social::NameLinkPacket packet;auto error=social::decodeNameLinkPacket(source,identity.genesis,packet);
        const auto owner=side==social::NameLinkSide::Native?packet.nativeScript:side==social::NameLinkSide::Social?packet.socialScript:packet.certificate["authorizer_script"].toString();
        if(!error.isEmpty()||owner!=script){done({},"Synthetic wallet scope refused");return;}
        const auto challenge=side==social::NameLinkSide::Native?packet.nativeChallenge:side==social::NameLinkSide::Social?packet.socialChallenge:packet.transitionChallenge;
        rpc("getdescriptorinfo",{"raw("+script+")"},false,[this,script,challenge,done](QJsonValue value,QString error){
          const auto descriptor=value.toObject()["descriptor"].toString();if(!error.isEmpty()||descriptor.isEmpty()){done({},"Public descriptor unavailable");return;}
          rpc("deriveaddresses",{descriptor},false,[this,script,challenge,done](QJsonValue value,QString error){
            const auto addresses=value.toArray();if(!error.isEmpty()||addresses.size()!=1){done({},"Native address unavailable");return;}const auto address=addresses[0].toString();
            rpc("getaddressinfo",{address},true,[this,address,script,challenge,done](QJsonValue value,QString error){
              if(!error.isEmpty()||!value.toObject()["ismine"].toBool()||value.toObject()["scriptPubKey"].toString()!=script){done({},"Selected key is not owned by this synthetic wallet");return;}
              rpc("signmessage",{address,QString::fromUtf8(challenge)},true,[this,done](QJsonValue value,QString error){if(error.isEmpty())++consentSignatures;done(value.toString(),error);});
            });
          });
        });
      };
      resolver=new social::NativeDisplayResolver(port,identity,this);
      nameClient=new social::CurrentDisplayConsent(port,signer,identity,config_["state"].toString()+"/display-proofs",this);
      nameClient->changed=[this]{if(view)view->refresh();};
    }
    void openNameDialog(QString key={}){
      ensureDisplay();if(nameDialog)nameDialog->close();const auto identity=service->identity();
      auto consent=nameClient->services();const auto local=consent.submit;consent.submit=[this,local](social::NameLinkIntent intent,QByteArray source,auto done){resolver->submit(source,[local,intent,source,done](QJsonObject answer,QString error){if(!error.isEmpty()){done(answer,error);return;}local(intent,source,done);});};
      nameDialog=new pgn::ui::SocialNameLinkDialog(consent,{identity,identity.subject,key,"bind",QString(64,'0')},config_["state"].toString()+"/public-consent.json",view.get());
      nameDialog->show();
    }

    void closeUi(){
      tracing=false;displayRefresh.stop();
      if(service){service->changed={};service->stop();}
      if(nameClient)delete nameClient;nameClient=nullptr;
      if(resolver)delete resolver;resolver=nullptr;
      view.reset();service.reset();
      for(auto* reply:network_.findChildren<QNetworkReply*>()){reply->disconnect(this);reply->abort();reply->deleteLater();}
    }
    void open(){
        social::CoreSocialContext context{{config_["genesis"].toString(),config_["script"].toString()},config_["address"].toString(),config_["device"].toString(),"global","Isolated Native offchain relay qualification network"};
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
        social::CoreSocialRpc transport=[this](social::CoreSocialCall call,QJsonArray params,auto done){const QString method=call==social::CoreSocialCall::SubmitEvent?"submitsocialevent":call==social::CoreSocialCall::ReadEvents?"getsocialevents":call==social::CoreSocialCall::ReadAssociations?"getsocialassociations":"getsocialpeers";rpc(method,params,false,done);};
        service=std::make_unique<social::CoreSocialService>(context,crypto,transport,config_["state"].toString()+"/outbox.json");
        service->leaderboard=[this](social::ReadRequest request,auto done){QJsonArray args;if(!social::leaderboardArguments(request,args)){done({},"Invalid leaderboard request");return;}
            rpc("getpgnleaderboard",args,false,[request,done](QJsonValue value,QString error){social::Snapshot snapshot;if(error.isEmpty())error=social::decodeLeaderboard(request,value.toObject(),snapshot);done(std::move(snapshot),error);});};
        ensureDisplay();auto viewServices=service->services();
        const auto baseRead=viewServices.read;
        viewServices.read=[this,baseRead](social::ReadRequest request,auto done){baseRead(request,[this,done](social::Snapshot snapshot,QString error){
          if(!error.isEmpty()||!resolver){done(std::move(snapshot),error);return;}
          resolver->decorate(std::move(snapshot),done);
        });};
        effectiveServices=viewServices;viewServices.publicNameLink=[this](social::Identity,QStringList keys){openNameDialog(keys.value(0));};view=std::make_unique<ui::SocialView>(config_["state"].toString()+"/drafts.json",std::move(viewServices));view->setWindowTitle("PGN Social - isolated native QA "+config_["label"].toString());view->resize(1366,900);view->show();
        service->changed=[this]{if(view)view->refresh();};service->diagnostic=[](QString){};service->start();view->setIdentity(context.identity);watchScroll();
    }
    void command(QJsonObject command){
        const QJsonValue id=command.value("id");const auto kind=command["command"].toString();

        if(kind=="scroll_trace_start"){tracing=true;scrollEvents={};scrollClock.start();traceScroll("start");output({{"id",id},{"started",true}});return;}
        if(kind=="scroll_trace"){output({{"id",id},{"events",scrollEvents}});return;}
        if(kind=="scroll_fence"){
          // Observe drained GUI turns; do not force layout or sleep35ms.
          // https://doc.qt.io/qt-6/qabstracteventdispatcher.html#aboutToBlock
          const auto link=std::make_shared<QMetaObject::Connection>();const auto previous=std::make_shared<QJsonObject>();const auto count=std::make_shared<int>(0);
          *link=connect(QAbstractEventDispatcher::instance(),&QAbstractEventDispatcher::aboutToBlock,this,[this,id,link,previous,count]{const auto state=scrollState();traceScroll("idle");const bool stable=!previous->isEmpty()&&*previous==state;*previous=state;if(stable||++*count>=12){disconnect(*link);output({{"id",id},{"stable",stable},{"scroll_state",state}});}});return;
        }
        if(kind=="rendered"){
            auto* list=view->findChild<QListWidget*>("pgn-social-list-0");QJsonArray messages,members;
            for(int i=0;list&&i<list->count();++i){const auto* item=list->item(i);messages.append(QJsonObject{{"id",item->data(Qt::UserRole).toString()},{"author_id",item->data(ui::AuthorIdRole).toString()},{"author",item->data(ui::AuthorRole).toString()}});}
            auto* roster=view->findChild<QListWidget*>("pgn-social-chat-members");for(int i=0;roster&&i<roster->count();++i){const auto* item=roster->item(i);members.append(QJsonObject{{"id",item->data(Qt::UserRole).toString()},{"name",item->text()}});}output({{"id",id},{"messages",messages},{"members",members}});return;
        }
        if(kind=="name_from_leaderboard"){
            auto* table=view->findChild<QTableWidget*>("pgn-social-miners");int row=-1;if(table)for(int i=0;i<table->rowCount();++i)if(table->item(i,0)->data(Qt::UserRole).toString()==command["operator_id"].toString())row=i;
            if(row<0){output({{"id",id},{"clicked",false}});return;}table->setCurrentCell(row,0);table->scrollToItem(table->item(row,0));const auto point=table->visualItemRect(table->item(row,0)).center();QContextMenuEvent event(QContextMenuEvent::Mouse,point,table->viewport()->mapToGlobal(point));QApplication::sendEvent(table->viewport(),&event);
            auto* menu=view->findChild<QMenu*>("pgn-social-context-3");QAction* action=nullptr;if(menu)for(auto* entry:menu->actions())if(entry->text()=="Link this Native identity to my Social key...")action=entry;
            const bool enabled=action&&action->isEnabled();if(enabled)action->trigger();if(menu)menu->close();output({{"id",id},{"clicked",enabled}});return;
        }
        if(kind=="chat"){view->selectSection(social::Section::Chat);output({{"id",id},{"selected",true}});return;}
        if(kind=="name_open"){openNameDialog(command["native_script"].toString());output({{"id",id},{"opened",bool(nameDialog)}});return;}
        if(kind=="name_import"){if(!nameDialog)openNameDialog();nameDialog->importPublicPacket(command["packet"].toString().toUtf8());output({{"id",id},{"imported",true}});return;}
        if(kind=="name_state"){if(!nameDialog){output({{"id",id},{"error","Consent dialog absent"}});return;}output({{"id",id},{"phase",int(nameDialog->flow().phase())},{"packet",QString::fromUtf8(nameDialog->flow().packet())},{"reason",nameDialog->flow().reason()},{"signatures",consentSignatures},{"summary",nameDialog->findChild<QLabel*>("pgn-social-link-summary")->text()},{"status",nameDialog->findChild<QLabel*>("pgn-social-link-state")->text()}});return;}
        if(kind=="name_click"){
          auto* button=nameDialog?nameDialog->findChild<QPushButton*>("pgn-social-link-"+command["action"].toString()):nullptr;
          const bool enabled=button&&button->isEnabled();if(enabled)button->click();output({{"id",id},{"enabled",enabled},{"clicked",enabled}});return;
        }
        if(kind=="name_agree"){auto* check=nameDialog?nameDialog->findChild<QCheckBox*>("pgn-social-link-explicit-consent"):nullptr;if(check)check->setChecked(command["agreed"].toBool());output({{"id",id},{"agreed",check&&check->isChecked()}});return;}
        if(kind=="name_capture"){const bool ok=nameDialog&&nameDialog->grab().save(command["path"].toString());output({{"id",id},{"saved",ok}});return;}
        if(kind=="name_close"){if(nameDialog)nameDialog->close();output({{"id",id},{"closed",true}});return;}

        if(kind=="layout"){auto* composer=view->findChild<QWidget*>("pgn-social-composer-0");if(command["repair"].toBool()){composer->layout()->invalidate();composer->layout()->activate();}QJsonArray tree;for(int i=0;i<composer->layout()->count();++i){auto* item=composer->layout()->itemAt(i);QJsonObject entry{{"index",i},{"x",item->geometry().x()},{"width",item->geometry().width()},{"min_width",item->minimumSize().width()},{"widget",item->widget()?item->widget()->objectName():QString{}},{"nested",item->layout()!=nullptr}};if(item->layout()){QJsonArray children;for(int j=0;j<item->layout()->count();++j){auto* child=item->layout()->itemAt(j);children.append(QJsonObject{{"x",child->geometry().x()},{"width",child->geometry().width()},{"min_width",child->minimumSize().width()},{"max_width",child->maximumSize().width()},{"empty",child->isEmpty()},{"widget",child->widget()?child->widget()->objectName():QString{}}});}entry.insert("children",children);}tree.append(entry);}QJsonArray rows;for(const auto& name:{"pgn-social-composer-0","pgn-social-send-reason-0","pgn-social-save-status-0"}){auto* w=view->findChild<QWidget*>(name);const auto pos=w->mapTo(view.get(),QPoint{});const auto c=w->contentsRect();rows.append(QJsonObject{{"name",name},{"x",pos.x()},{"y",pos.y()},{"w",w->width()},{"h",w->height()},{"contents_x",c.x()},{"contents_w",c.width()},{"minw",w->minimumWidth()},{"minhintw",w->minimumSizeHint().width()},{"hintw",w->sizeHint().width()},{"maxw",w->maximumWidth()},{"policy",int(w->sizePolicy().horizontalPolicy())},{"parent",w->parentWidget()->objectName()}});}output({{"id",id},{"layout",rows},{"tree",tree}});return;}

        if(kind=="display_admit"){ensureDisplay();nameClient->verifyAndAdmit(command["packet"].toString().toUtf8(),[this,id](QString error){output({{"id",id},{"error",error}});});return;}
        if(kind=="display_restore"){ensureDisplay();nameClient->restore(command["script"].toString(),[this,id](QString error){output({{"id",id},{"error",error}});});return;}
        if(kind=="display_state"){ensureDisplay();const auto v=nameClient->store().head(command["script"].toString());output({{"id",id},{"state",v.state},{"head",v.head},{"label",v.label}});return;}
        if(kind=="snapshot"){
            effectiveServices.read({social::Section::Chat,service->identity()},[this,id](social::Snapshot snapshot,QString error){QJsonArray participants;for(const auto& member:snapshot.participants.value_or(QVector<social::Participant>{}))participants.append(QJsonObject{{"id",member.id},{"name",member.name},{"presence",member.presence}});QJsonArray entries;for(const auto& entry:snapshot.entries)entries.append(QJsonObject{{"id",entry.id},{"body",entry.body},{"author",entry.author},{"author_id",entry.authorId},{"state",entry.state}});
                auto* list=static_cast<ui::SocialChatList*>(view->findChild<QListWidget*>("pgn-social-list-0"));auto* anchor=list->itemAt(QPoint(20,20));output({{"id",id},{"error",error},{"messages",entries},{"participants",participants},{"body_font",QJsonObject::fromVariantMap(list->property("pgn_actual_body_font").toMap())},{"chrome_font",QJsonObject::fromVariantMap(ui::SocialActualFont(view->findChild<QLabel*>("pgn-social-account")->font()))},{"focused",view->focusWidget()?view->focusWidget()->objectName():QString{}},{"focused_global",QApplication::focusWidget()?QApplication::focusWidget()->objectName():QString{}},{"window_active",view->isActiveWindow()},{"saved_text",view->findChild<QLabel*>("pgn-social-save-status-0")->text()},{"visible_editor",view->findChild<QPlainTextEdit*>("pgn-social-draft-body-0")->toPlainText()},{"signed",signed_},{"verified",verified_},{"can_send",view->findChild<QPushButton*>("pgn-social-send-0")->isEnabled()},{"editor_height",view->findChild<QPlainTextEdit*>("pgn-social-draft-body-0")->height()},{"save_enabled",view->findChild<QPushButton*>("pgn-social-save-0")->isEnabled()},{"clear_enabled",view->findChild<QPushButton*>("pgn-social-clear-0")->isEnabled()},{"scroll",list->verticalScrollBar()->value()},{"maximum",list->verticalScrollBar()->maximum()},{"anchor",anchor?anchor->data(Qt::UserRole).toString():QString{}},{"anchor_top",anchor?list->visualItemRect(anchor).top():0},{"unseen",view->findChild<QPushButton*>("pgn-social-new-messages")->isVisible()}});});return;
        }
        if(kind=="moderate_menu"){
            auto* list=view->findChild<QListWidget*>("pgn-social-list-0");QListWidgetItem* target=nullptr;for(int i=0;i<list->count();++i)if(list->item(i)->data(Qt::UserRole).toString()==command["target"].toString())target=list->item(i);
            if(!target){output({{"id",id},{"error","Target absent from actual view"}});return;}list->setCurrentItem(target);list->scrollToItem(target);const auto pos=list->visualItemRect(target).center();QContextMenuEvent event(QContextMenuEvent::Mouse,pos,list->viewport()->mapToGlobal(pos));QApplication::sendEvent(list->viewport(),&event);
            QMenu* menu=nullptr;for(auto* candidate:view->findChildren<QMenu*>())if(candidate->isVisible())menu=candidate;QAction* action=nullptr;if(menu)for(auto* entry:menu->actions())if(entry->text()=="Remove message"||entry->text()=="Hide message")action=entry;
            const bool enabled=action&&action->isEnabled();const auto reason=action?action->toolTip():QString("Action not found");if(enabled&&command["trigger"].toBool())action->trigger();if(menu)menu->close();output({{"id",id},{"found",action!=nullptr},{"enabled",enabled},{"reason",reason}});return;
        }
        if(kind=="send"){auto* edit=view->findChild<QPlainTextEdit*>("pgn-social-draft-body-0");edit->setPlainText(command["text"].toString());const auto style=command["format"].toString();if(QStringList{"b","i","u","s"}.contains(style)){auto cursor=edit->textCursor();cursor.select(QTextCursor::Document);edit->setTextCursor(cursor);view->findChild<QPushButton*>("pgn-social-format-"+style)->click();}view->findChild<QPushButton*>("pgn-social-send-0")->setFocus();view->findChild<QPushButton*>("pgn-social-send-0")->click();output({{"id",id},{"clicked",true}});return;}
        if(kind=="capture"){
            view->setFixedSize(command["width"].toInt(1366),command["height"].toInt(994));QTimer::singleShot(100,view.get(),[this,id,command]{const auto ok=view->grab().save(command["path"].toString());output({{"id",id},{"saved",ok},{"width",view->width()},{"height",view->height()}});});return;
        }
        if(kind=="reopen"){closeUi();open();displayRefresh.start();output({{"id",id},{"reopened",true}});return;}
        if(kind=="focus_search"){view->findChild<QLineEdit*>("pgn-social-search-0")->setFocus();output({{"id",id},{"focused",true}});return;}
        if(kind=="draft"){view->findChild<QPlainTextEdit*>("pgn-social-draft-body-0")->setPlainText(command["text"].toString());view->saveDrafts();output({{"id",id},{"saved",true}});return;}
        if(kind=="draft_read"){output({{"id",id},{"text",view->findChild<QPlainTextEdit*>("pgn-social-draft-body-0")->toPlainText()}});return;}
        if(kind=="scroll_older"){auto* list=view->findChild<QListWidget*>("pgn-social-list-0");list->setFocus();auto* bar=list->verticalScrollBar();const int requested=bar->maximum()/3+7;bar->setSliderDown(true);bar->setSliderPosition(requested);bar->setSliderDown(false);output({{"id",id},{"scrolled",bar->value()==requested},{"requested",requested}});return;}
        if(kind=="jump_latest"){view->findChild<QPushButton*>("pgn-social-new-messages")->click();output({{"id",id},{"jumped",true}});return;}
        if(kind=="leaderboard"){view->selectSection(social::Section::Miners);output({{"id",id},{"selected",true}});return;}
        if(kind=="stop"){closeUi();output({{"id",id},{"stopped",true}});QApplication::quit();return;}
        output({{"id",id},{"error","Unknown harness command"}});
    }
};
int main(int argc,char** argv){QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);if(argc!=2)return 2;QFile file(QString::fromLocal8Bit(argv[1]));if(!file.open(QIODevice::ReadOnly))return 2;Fixture fixture(QJsonDocument::fromJson(file.readAll()).object());
    std::thread([&fixture]{std::string line;while(std::getline(std::cin,line)){const auto command=QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();QMetaObject::invokeMethod(&fixture,[&fixture,command]{fixture.command(command);},Qt::QueuedConnection);}}).detach();
    return app.exec();}
