#include "ui/social_view.h"
#include "ui/quadrigantium_rosette.h"
#include "ui/render/horizon_workspace_style.h"
#include "ui/render/horizon_primary_navigation.h"
#include "qt_offscreen_fixture.h"
#include <QApplication>
#include <cmath>
#include <QComboBox>
#include <QFile>
#include <QFontInfo>
#include <QJsonDocument>
#include <QHeaderView>
#include <QImage>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTextBrowser>
#include <QVBoxLayout>
#include <QStackedWidget>
#include <QScrollArea>
#include <iostream>
#include <stdexcept>
#include "social_v4_checks.h"
#include "social_chat_checks.h"
#include "social_leaderboard_utc_checks.h"
#include "social_public_names_checks.h"
#include "social_chat_scroll_checks.h"
#include "social/social_numbers.h"
#include <QClipboard>
using namespace pgn;
static int checks=0;
static void check(bool condition,const char* message){++checks;if(!condition)throw std::runtime_error(message);}
template<class T>static T* find(QWidget& view,const QString& name){auto* p=view.findChild<T*>(name);check(p,"Missing social control");return p;}
static social::Identity A(){return {QString(64,'a'),"SYNTHETIC-ACCOUNT-A"};}
static social::Identity B(){return {QString(64,'a'),"SYNTHETIC-ACCOUNT-B"};}
static social::Identity C(){return {QString(64,'b'),"SYNTHETIC-ACCOUNT-A"};}
static social::Snapshot snapshot(const social::ReadRequest& request){
    social::Snapshot s;s.identity=request.identity;s.section=request.section;s.window=request.window;
    s.authenticated=true;s.source="SYNTHETIC TEST FIXTURE — NOT NETWORK DATA";s.observedUtc=QDateTime::currentDateTimeUtc();return s;
}
static social::Entry entry(QString id,QString title,int age=0,QString category={}){
    return {id,"<b>Synthetic author</b>",title,"Synthetic text <script>literal only</script>",QDateTime::currentDateTimeUtc().addSecs(-age),category,{},{}};
}
static social::Provenance provenance(){return {A().genesis,"SYNTHETIC TEST FIXTURE — NOT NETWORK DATA",QDateTime::currentDateTimeUtc(),true};}
static QVector<social::Miner> miners(){
    QVector<social::Miner> values;
    for(const auto& id:QStringList{"missing","zero","large","adjacent"}){
        social::Miner m;m.id=id;m.name=id=="missing"?"Synthetic unavailable":id=="zero"?"Synthetic explicit zero":"Synthetic homonym";
        m.socialMemberId="canonical-"+id;
        if(id!="missing"){
            m.validatedSearchPositions={id=="zero"?QString("0"):id=="large"?QString("9007199254740993"):QString("9007199254740992"),provenance()};
            m.ticketsReported={QString("12"),provenance()};m.ticketsValidated={QString("9"),provenance()};
            m.validatedAudits={QString("7"),provenance()};m.lastActivityHeight={QString("16640"),provenance()};
        }
        values.append(m);
    }
    return values;
}
static void key(QWidget* view,int code){QKeyEvent e(QEvent::KeyPress,code,Qt::NoModifier);QApplication::sendEvent(view,&e);QKeyEvent up(QEvent::KeyRelease,code,Qt::NoModifier);QApplication::sendEvent(view,&up);}
static void fullyVisible(QWidget& root,QWidget* widget,const char* why){
    QApplication::processEvents();const QRect rect(widget->mapTo(&root,QPoint(0,0)),widget->size());
    check(widget->isVisible()&&!widget->visibleRegion().isEmpty()&&root.rect().contains(rect),why);
}
static social::AccessSnapshot reviewAccess(const social::Identity& identity){
    social::AccessSnapshot a;a.identity=identity;a.authenticated=true;a.community="synthetic-review-community";a.revision="synthetic-revision-1";a.source="SYNTHETIC REVIEW ONLY";a.observedUtc=QDateTime::currentDateTimeUtc();a.moderatorRoleId="preview-moderator";
    a.roles={{"preview-moderator","Moderators",10,{"VIEW_CHANNEL","SEND_MESSAGES","MANAGE_ROLES","MANAGE_MESSAGES","KICK_MEMBERS","BAN_MEMBERS"},false},{"preview-member","Members",0,{"VIEW_CHANNEL","SEND_MESSAGES"},false}};
    a.permissionCatalog={"VIEW_CHANNEL","SEND_MESSAGES","MANAGE_ROLES","MANAGE_MESSAGES","KICK_MEMBERS","BAN_MEMBERS"};a.grantablePermissions={"VIEW_CHANNEL","SEND_MESSAGES"};
    a.members={{"review-mod-a","Review mod A",{"preview-moderator"}},{"review-mod-b","Review mod B",{"preview-moderator"}},{"review-mod-c","Review mod C",{"preview-moderator"}},{"review-a","Review author A",{"preview-member"}},{"review-b","Review author B",{"preview-member"}},{"review-c","Review author C",{"preview-member"}}};
    a.session.identity=identity;a.session.state=social::AuthState::Verified;a.session.reference="SYNTHETIC-SESSION-NOT-A-TOKEN";a.session.verificationRef="SYNTHETIC-ASSERTION-NOT-CRYPTOGRAPHIC-PROOF";a.session.revocationRevision="preview-1";a.session.expiresUtc=QDateTime::currentDateTimeUtc().addSecs(300);
    a.session.devices={{"synthetic-device","Review device · synthetic",false}};a.session.device="synthetic-device";
    return a;
}

static social::Snapshot previewData(const social::ReadRequest& request){
    auto s=snapshot(request);s.source="Review fixtures";s.access=reviewAccess(request.identity);
    if(request.section==social::Section::Chat){
        s.channel=request.channel.isEmpty()?"preview-general":request.channel;
        s.channels=QVector<social::Channel>{{"preview-general","general","Network discussion"},{"preview-mining","mining","Mining discussions"},{"preview-research","research","Research notes"},{"preview-support","support","Support questions"}};
        s.participants=QVector<social::Participant>{{"review-mod-a","Review mod A",{},{"preview-moderator"}},{"review-mod-b","Review mod B",{},{"preview-moderator"}},{"review-mod-c","Review mod C",{},{"preview-moderator"}},{"review-a","Review author A",{},{"preview-member"}},{"review-b","Review author B",{},{"preview-member"}},{"review-c","Review author C",{},{"preview-member"}}};
        const QStringList messages{
            "Welcome. This channel keeps the discussion tied to the selected network.",
            "Can we include the exact observation window when we discuss a miner's activity?",
            "Yes. The source and observation time matter as much as the displayed count.",
            "I am preparing a short note about reading an audit receipt.\nIt separates the reported observation from a verified result.",
            "Please include the ticket reference in this channel so the discussion keeps its context.",
            "The draft is saved locally. I will review the wording before sharing it.",
            "A missing metric should remain [b]unavailable[/b]. It should not be presented as zero.",
            "[quote]A missing metric should remain unavailable.[/quote]That distinction matters when a snapshot is partial.",
            "The research topic now contains the method and the questions still open.",
            "Thanks. Let's keep the exact references with the next network snapshot."
        };
        for(int i=0;i<messages.size();++i){auto e=entry(QString("chat-%1").arg(i),{},(messages.size()-i)*150);e.author=i%2?"Review author B":"Review author A";e.body=messages[i];s.entries.append(e);}
    }else if(request.section==social::Section::Miners){
        s.miners=miners();
    }
    for(auto& e:s.entries){e.authorId=e.author.endsWith(" B")?"review-b":"review-a";e.bbcode=true;}

    return s;
}
int main(int argc,char** argv){
 try{
    if(!test::prepareOffscreen())return 2;QApplication app(argc,argv);app.setFont(QFont("Segoe UI",10));
    QTemporaryDir temp;check(temp.isValid(),"No temporary draft directory");
    test::v4::run(app,temp.path()+"/v4-contract");
    test::chat::run(temp.path()+"/send-contract/drafts.json");
    test::utc::run(app,temp.path()+"/utc-contract/drafts.json");
    test::names::run(app,temp.path()+"/names-contract/outbox.json");
    social::DraftStore store(temp.path()+"/store/drafts.json");social::Drafts d,loaded;
    d.sections[0].body=QString::fromUtf8("Unicode café 🌌");check(store.save(A(),d).isEmpty()&&store.load(A(),loaded).isEmpty()&&loaded.sections[0].body==d.sections[0].body,"Unicode round-trip failed");
    check(store.path(A())!=store.path(B())&&store.path(A())!=store.path(C()),"Draft identity/genesis collision");
    check(store.load(B(),loaded).isEmpty()&&loaded.sections[0].body.isEmpty(),"Cross-identity draft leaked");
    d.sections[0].body=QString(4001,'x');check(!store.save(A(),d).isEmpty()&&store.load(A(),loaded).isEmpty()&&loaded.sections[0].body.startsWith("Unicode"),"Invalid save replaced complete draft");
    social::Drafts escaped;for(int i=0;i<3;++i)escaped.sections[i].body=QString(social::draftLimit(i),QChar(1));
    for(int i=0;i<8;++i)escaped.replies.insert(QString("topic-%1").arg(i),QString(4000,QChar(1)));
    social::DraftStore escapedStore(temp.path()+"/escaped/drafts.json");
    check(escapedStore.save(A(),escaped).isEmpty()&&escapedStore.load(A(),loaded).isEmpty()&&loaded.replies.size()==8,"Escaped JSON exceeds its own reload limit");
    escaped.replies.insert("ninth","no");check(!escapedStore.save(A(),escaped).isEmpty(),"Reply draft limit missing");
    {
        ui::SocialView local(temp.path()+"/local/drafts.json");local.resize(1280,677);local.show();app.processEvents();
        check(local.findChildren<QScrollArea*>().isEmpty(),"A whole-page scroll area remains");
        auto* tabs=find<QTabWidget>(local,"pgn-social-sections");
        check(tabs->count()==4&&tabs->isTabVisible(0)&&tabs->isTabVisible(3)&&!tabs->isTabVisible(1)&&!tabs->isTabVisible(2),"Only Chat/Leaderboard may be visible; preserve dormant pages");
        check(tabs->tabText(0)=="Chat"&&tabs->tabText(3)=="Leaderboard","Incorrect active subtab names");
        for(const auto hidden:{social::Section::Publications,social::Section::Forum}){local.selectSection(hidden);check(tabs->currentIndex()==0,"selectSection reopened a retired page");}
        for(auto* label:local.findChildren<QLabel*>())check(!label->text().contains("People, ideas")&&!label->text().contains("SOCIAL / COMMUNITY"),"Hero or marketing header remains");
        check(!find<QPlainTextEdit>(local,"pgn-social-draft-body-0")->isEnabled(),"Unscoped draft allowed");
        local.setIdentity(A());auto* body=find<QPlainTextEdit>(local,"pgn-social-draft-body-0");body->setPlainText("Draft A");
        local.setIdentity(B());check(body->toPlainText().isEmpty(),"Identity switch leaked draft");body->setPlainText("Draft B");
        local.setIdentity(C());check(body->toPlainText().isEmpty(),"Genesis switch leaked draft");body->setPlainText("Draft C");
        local.setIdentity(A());check(body->toPlainText()=="Draft A","Scoped draft not restored");
        body->setPlainText(QString(3999,'x')+QString::fromUtf8("🌌"));check(body->toPlainText().size()==3999,"Limit split Unicode surrogate");
        body->setPlainText("Close flush");auto* nav=local.findChild<QFrame*>("horizonNavigation");check(nav&&nav->height()==58,"Navigation diverges from Horizon height");
        const auto buttons=nav->findChildren<QPushButton*>();buttons[0]->setFocus();key(buttons[0],Qt::Key_Right);check(tabs->currentIndex()==3,"Arrow navigation did not skip retired sections");
        buttons[0]->setFocus();key(buttons[0],Qt::Key_Space);key(buttons[0],Qt::Key_Tab);check(QApplication::focusWidget()!=buttons[0],"Hidden tab proxy traps focus");
        body->setFocus();key(body,Qt::Key_Tab);check(QApplication::focusWidget()!=body,"Composer traps Tab");
        fullyVisible(local,find<QPushButton>(local,"pgn-social-send-0"),"Chat Send cut at 1280x720");
        fullyVisible(local,find<QPushButton>(local,"pgn-social-save-0"),"Chat Save cut at 1280x720");
        fullyVisible(local,find<QLabel>(local,"pgn-social-send-reason-0"),"Disabled Send reason cut");
        check(!find<QPushButton>(local,"pgn-social-send-0")->isEnabled(),"Unconnected send enabled");
    }
    social::DraftStore localStore(temp.path()+"/local/drafts.json");check(localStore.load(A(),loaded).isEmpty()&&loaded.sections[0].body=="Close flush","Close lost pending autosave");
    social::DraftStore brokenStore(temp.path()+"/broken/drafts.json");QDir().mkpath(QFileInfo(brokenStore.path(A())).absolutePath());QFile bad(brokenStore.path(A()));
    check(bad.open(QIODevice::WriteOnly),"No corrupt fixture");bad.write("{broken");bad.close();
    {ui::SocialView local(temp.path()+"/broken/drafts.json");local.setIdentity(A());check(!local.saveDrafts(),"Corrupt draft file silently overwritten");}
    check(bad.open(QIODevice::ReadOnly)&&bad.readAll()=="{broken","Corrupt file not retained");bad.close();

    struct Pending{social::ReadRequest request;social::Services::Reply reply;};QVector<Pending> pending;
    social::Services services{[&](auto request,auto reply){pending.append({request,std::move(reply)});}};
    auto* view=new ui::SocialView(temp.path()+"/remote/drafts.json",services);view->resize(1280,677);view->show();app.processEvents();
    check(pending.isEmpty(),"Network read inferred without identity");view->setIdentity(A());view->refresh();check(pending.size()==1,"Overlapping reads admitted");
    auto invalid=snapshot(pending.last().request);invalid.authenticated=false;invalid.entries={entry("x",{})};pending.last().reply(invalid,{});
    check(find<QListWidget>(*view,"pgn-social-list-0")->count()==0&&view->property("pgn_social_state_0")=="error","Unauthenticated content accepted");
    view->refresh();const auto old=pending.last();view->setIdentity(C());old.reply(snapshot(old.request),{});
    check(find<QListWidget>(*view,"pgn-social-list-0")->count()==0,"Late wrong-genesis response accepted");
    auto* focus=find<QPlainTextEdit>(*view,"pgn-social-draft-body-0");focus->setFocus();app.processEvents();
    auto chat=snapshot(pending.last().request);chat.entries={entry("new",{},10),entry("old",{},90)};pending.last().reply(chat,{});
    check(QApplication::focusWidget()==focus,"Arriving content stole composer focus");
    auto* messages=find<QListWidget>(*view,"pgn-social-list-0");check(messages->count()==2&&messages->item(0)->text().contains("<script>literal only</script>"),"Chat is not safe plain text");
    check(!view->findChild<QTextBrowser*>("pgn-social-detail-0"),"Chat has a duplicate reading panel");
    view->setIdentity(A());pending.last().reply(snapshot(pending.last().request),{});
    auto validChat=snapshot(pending.last().request);validChat.entries={entry("kept",{},10)};
    view->refresh();pending.last().reply(validChat,{});view->refresh();auto duplicate=validChat;duplicate.entries.append(validChat.entries[0]);pending.last().reply(duplicate,{});
    check(messages->count()==1&&view->property("pgn_social_state_0")=="error","Malformed refresh erased valid chat");
    const int beforeHidden=pending.size();
    for(const auto hidden:{social::Section::Publications,social::Section::Forum})view->selectSection(hidden);
    check(pending.size()==beforeHidden&&find<QTabWidget>(*view,"pgn-social-sections")->currentIndex()==0,"Hidden section started a read");

    view->selectSection(social::Section::Miners);auto data=snapshot(pending.last().request);data.miners=miners();data.miners[2].validatedSearchPositions.provenance.source="<img src='not-loaded'>";pending.last().reply(data,{});
    auto* table=find<QTableWidget>(*view,"pgn-social-miners");
    const QStringList columns{"Miner","Validated positions","Tickets reported","Tickets validated","Audits validated","Last activity · block"};
    check(table->columnCount()==11,"Six verified metrics plus five explicit unavailable tier columns expected");for(int i=0;i<columns.size();++i)check(table->horizontalHeaderItem(i)->text()==columns[i],"Wrong leaderboard columns");
    {const ui::MeritTier tiers[]{ui::MeritTier::Gold,ui::MeritTier::Silver,ui::MeritTier::Bronze,ui::MeritTier::NearMiss};
     for(int col=6;col<11;++col){const auto* h=table->horizontalHeaderItem(col);check(!h->icon().isNull()&&!h->data(Qt::AccessibleTextRole).toString().isEmpty()&&!h->toolTip().isEmpty(),"Tier glyph lacks icon/accessibility");for(int row=0;row<table->rowCount();++row)check(table->item(row,col)->text()==QString::fromUtf8("—"),"Tier counter fabricated from absent data");}
     for(int col=7;col<11;++col)check(table->horizontalHeaderItem(col)->icon().pixmap(24,28).toImage()==ui::gapRosetteIcon(tiers[col-7],false).pixmap(24,28).toImage(),"Existing Mining artwork changed");
     check(table->horizontalHeaderItem(6)->toolTip()==QString::fromUtf8("Quadrigantium · merit 40"),"Q lost its approved meaning");}
    check(!table->showGrid(),"Leaderboard still draws a vertical grid");
    for(int row=0;row<table->rowCount();++row)check(table->rowHeight(row)==36,"Leaderboard rows exceed the36px density contract");
    for(int n=0;n<5;++n)check(table->horizontalHeader()->logicalIndex(n+1)==6+n&&table->columnWidth(6+n)==44,"Awards are not44px immediately after Miner");
    auto* totalButton=find<QPushButton>(*view,"pgn-social-period-total");auto* todayButton=find<QPushButton>(*view,"pgn-social-period-today");
    check(totalButton->isChecked()&&!todayButton->isChecked(),"Initial segmented Total state lost");
    check(table->item(0,0)->data(Qt::UserRole)!=table->item(1,0)->data(Qt::UserRole),"Homonymous display names collapsed canonical identities");
    const auto rowId=[&](int row){return table->item(row,0)->data(Qt::UserRole).toString();};
    check(rowId(0)=="large"&&rowId(1)=="adjacent"&&rowId(2)=="zero"&&rowId(3)=="missing","Exact volume sort failed above 2^53 or missing value sorted as zero");
    check(table->item(0,1)->text()==social::leaderboardNumber("9007199254740993")&&table->item(0,1)->text()==table->item(1,1)->text(),"Scientific display fixture does not exercise equal rounded text");
    check(table->item(2,1)->text()=="0"&&table->item(3,1)->text()==QString::fromUtf8("—"),"Absent/explicit zero distinction failed");
    check((table->item(0,1)->textAlignment()&Qt::AlignRight)&&table->item(0,1)->data(Qt::AccessibleDescriptionRole).toString().contains(A().genesis)&&table->item(0,1)->toolTip().contains("9007199254740993"),"Numeric alignment or exact accessible provenance missing");
    check(table->item(0,1)->toolTip().contains("&lt;img")&&!table->item(0,1)->toolTip().contains("<img"),"External tooltip interpreted as rich content");
    check(find<QLabel>(*view,"pgn-social-loaded-3")->text().contains("Sorted by Validated positions (descending)"),"Default volume sort is not explicit");
    auto* sort=find<QHeaderView>(*view,"pgn-social-miner-sort-header");auto* refreshMiners=find<QPushButton>(*view,"pgn-social-refresh-3");refreshMiners->setFocus();key(refreshMiners,Qt::Key_Tab);app.processEvents();check(sort->hasFocus(),"Sort header is not reachable with Tab");key(sort,Qt::Key_Home);key(sort,Qt::Key_Right);key(sort,Qt::Key_Return);
    check(find<QLabel>(*view,"pgn-social-loaded-3")->text().contains("Sorted by Validated positions (ascending)"),"Changed sort order is not explicit");
    check(rowId(0)=="zero"&&rowId(1)=="adjacent"&&rowId(2)=="large"&&rowId(3)=="missing","Keyboard exact-volume sort failed or placed unknown first");
    // Equal counters are ordered by canonical operator identity, never homonym.
    key(sort,Qt::Key_Right);key(sort,Qt::Key_Return);check(rowId(0)=="adjacent"&&rowId(1)=="large"&&rowId(2)=="zero","Counter ties are not ordered by canonical identity");
    for(int col=1;col<6;++col){check(table->item(3,col)->text()==QString::fromUtf8("—"),"Missing metric became a fabricated zero");}
    for(const auto field:{&social::Miner::validatedSearchPositions,&social::Miner::ticketsReported,&social::Miner::ticketsValidated,&social::Miner::validatedAudits,&social::Miner::lastActivityHeight}){
        auto badMetric=data;(badMetric.miners[2].*field).provenance.genesis=C().genesis;check(!social::validate(badMetric,pending.last().request).isEmpty(),"Wrong-genesis value accepted");
        badMetric=data;(badMetric.miners[2].*field).provenance.authenticated=false;check(!social::validate(badMetric,pending.last().request).isEmpty(),"Unauthenticated metric accepted");
    }
    auto* window=find<QComboBox>(*view,"pgn-social-miner-window");check(window->count()==2&&window->itemText(1)==social::windowLabel(social::Window::Day)&&window->currentText()=="Total"&&pending.last().request.window==social::Window::AllTime,"Leaderboard observation windows or default changed");
    view->refresh();const auto oldPage=pending.last();view->setIdentity(B());oldPage.reply(data,{});check(table->rowCount()==0,"Late old-identity leaderboard page accepted");
    view->setIdentity(A());auto current=snapshot(pending.last().request);current.miners=miners();pending.last().reply(current,{});
    view->refresh();pending.last().reply({},"Source unavailable");check(table->rowCount()==4&&view->property("pgn_social_state_3")=="error","Error erased known snapshot");
    view->refresh();pending.last().reply(snapshot(pending.last().request),{});check(table->rowCount()==0&&view->property("pgn_social_state_3")=="empty","Empty response confused with offline/error");
    {for(const auto& name:{"pgn-social-toggle-members","pgn-social-refresh-0","pgn-social-send-0"})check(!find<QPushButton>(*view,name)->icon().isNull(),"Chat action lacks its native vector icon");}
    view->setIdentity({});check(table->rowCount()==0&&messages->count()==0&&view->property("pgn_social_state_3")=="offline","Signout did not clear data/state");
    view->setIdentity(A());const auto late=pending.last();delete view;late.reply(snapshot(late.request),{});
    {
        ui::SocialView pages(temp.path()+"/page-refusals/drafts.json",services);pages.setIdentity(A());pages.selectSection(social::Section::Miners);
        auto first=snapshot(pending.last().request);first.miners={miners()[1]};first.nextCursor="page-two";pending.last().reply(first,{});
        auto* table=find<QTableWidget>(pages,"pgn-social-miners");auto* more=find<QPushButton>(pages,"pgn-social-more-3");
        more->click();const auto overlap=pending.last();auto bad=snapshot(overlap.request);bad.miners=first.miners;bad.nextCursor="page-three";overlap.reply(bad,{});
        check(table->rowCount()==1&&pages.property("pgn_social_state_3")=="error","Overlapping page duplicated an operator");
        more->click();const auto loop=pending.last();bad=snapshot(loop.request);bad.nextCursor=loop.request.cursor;loop.reply(bad,{});
        check(table->rowCount()==1&&pages.property("pgn_social_state_3")=="error","Repeated page cursor accepted");
        more->click();const auto next=pending.last();auto good=snapshot(next.request);good.miners={miners()[2]};next.reply(good,{});
        check(table->rowCount()==2,"Valid retry did not append one distinct operator");
        next.reply(first,{});check(table->rowCount()==2,"Duplicate page callback changed loaded rows");
    }
    {
        ui::SocialView paged(temp.path()+"/paged/drafts.json",services);paged.setIdentity(A());paged.selectSection(social::Section::Miners);
        int count=0;
        for(int page=0;page<6;++page){const auto req=pending.last().request;const int amount=page==0?95:page==5?5:100;
            check(req.limit==(page==5?5:100),"Remaining capacity not carried into bounded request");
            auto response=snapshot(req);for(int n=0;n<amount;++n){auto m=miners()[1];m.id=QString("page-miner-%1").arg(count++);response.miners.append(m);}response.nextCursor=QString("cursor-%1").arg(page+1);pending.last().reply(response,{});
            check(find<QTableWidget>(paged,"pgn-social-miners")->rowCount()==count,"Bounded page append failed");
            if(page<5){const int before=pending.size();find<QPushButton>(paged,"pgn-social-more-3")->click();find<QPushButton>(paged,"pgn-social-more-3")->click();check(pending.size()==before+1,"More overlaps reads");}}
        const int before=pending.size();check(count==500&&!find<QPushButton>(paged,"pgn-social-more-3")->isEnabled(),"500-row buffer cap missing");
        find<QPushButton>(paged,"pgn-social-more-3")->click();check(pending.size()==before,"Read exceeded buffer cap");
    }
    {
        QVector<Pending> calls;social::Services service;service.read=[&](auto request,auto done){calls.append({request,done});};
        ui::SocialView ranking(temp.path()+"/design/drafts.json",service);ranking.resize(1366,900);ranking.show();ranking.setIdentity(A());ranking.selectSection(social::Section::Miners);app.processEvents();
        auto response=snapshot(calls.last().request);response.miners=miners();response.miners[0].socialMemberId=A().subject;calls.last().reply(response,{});app.processEvents();
        auto* grid=find<QTableWidget>(ranking,"pgn-social-miners");auto* total=find<QPushButton>(ranking,"pgn-social-period-total");auto* today=find<QPushButton>(ranking,"pgn-social-period-today");
        check(grid->height()<=58+4*36+4,"Small leaderboard leaves a giant pale empty sheet");
        for(int size=1;size<=4;++size){auto small=response;small.miners.resize(size);ranking.refresh();calls.last().reply(small,{});app.processEvents();check(grid->height()<=qMax(100,58+size*36+4),"One-to-four-row card stretched beyond its data");}
        for(int row=0;row<grid->rowCount();++row)check(grid->item(row,0)->data(Qt::UserRole+180).toBool()==(grid->item(row,0)->data(Qt::UserRole).toString()=="missing"),"Self-row highlight inferred from name or another identity");
        today->setFocus();key(today,Qt::Key_Space);app.processEvents();check(today->isChecked()&&!total->isChecked()&&calls.last().request.window==social::Window::Day,"Keyboard Today button bypassed real UTC query");
        const auto stale=calls.last();total->click();app.processEvents();check(total->isChecked()&&!today->isChecked()&&calls.last().request.window==social::Window::AllTime,"Total button bypassed real period reset");
        stale.reply(response,{});check(grid->rowCount()==0,"Old period callback repopulated new period");
        response=snapshot(calls.last().request);response.miners=miners();calls.last().reply(response,{});app.processEvents();
        auto* search=find<QLineEdit>(ranking,"pgn-social-search-3");search->setText("homonym");app.processEvents();int shown=0;for(int row=0;row<grid->rowCount();++row)shown+=!grid->isRowHidden(row);check(shown==2,"Loaded search collapsed homonyms or stopped filtering");
        search->clear();ranking.resize(1920,1080);app.processEvents();check(grid->width()>=ranking.width()-60&&grid->horizontalScrollBar()->maximum()==0,"Large ranking does not fill its workspace or hid metrics");
    }
    if(argc==3&&QString::fromLocal8Bit(argv[1])=="--render-access"){
        const QString output=QString::fromLocal8Bit(argv[2]);check(QDir().mkpath(output),"No access capture directory");
        QWidget frame;frame.setStyleSheet("QWidget{background:#041319;color:#c9eee7;}");auto* layout=new QVBoxLayout(&frame);layout->setContentsMargins(16,16,16,16);
        auto* banner=new QLabel("REVISION PREVIEW · synthetic roles / session · no management or identity service");banner->setStyleSheet("font:12px 'Segoe UI';color:#e6bd64;");layout->addWidget(banner);layout->addStretch();
        auto identity=A();identity.subject="Review author A";auto* access=new ui::SocialAccessDialog(identity,reviewAccess(identity),{},"preview-general",{},&frame);access->setWindowFlags(Qt::Widget);access->setFixedSize(820,530);layout->addWidget(access,0,Qt::AlignHCenter);layout->addStretch();frame.resize(1366,950);frame.show();access->show();app.processEvents();
        auto* pages=access->findChild<QTabWidget*>();check(pages&&pages->count()==4,"Access sections missing");const QStringList names{"roles","members","channel-permissions","account-devices"};
        for(int i=0;i<4;++i){pages->setCurrentIndex(i);app.processEvents();fullyVisible(frame,access,"Access panel clipped");QImage image(frame.size(),QImage::Format_ARGB32);image.fill(Qt::transparent);frame.render(&image);check(image.save(output+"/access-"+names[i]+".png"),"Access capture not saved");}
    }

    if(argc==3&&QString::fromLocal8Bit(argv[1])=="--render"){
        const QString output=QString::fromLocal8Bit(argv[2]);check(QDir().mkpath(output),"No capture output directory");
        {QFont font("Segoe UI");font.setPixelSize(13);const QFontInfo actual(font);QJsonArray widths;const QFontMetrics metrics(font);for(int n=0;n<10;++n)widths.append(metrics.horizontalAdvance(QString::number(n)));for(int n=1;n<10;++n)check(widths[n]==widths[0],"Effective Segoe UI numeric glyphs are not tabular");QFile file(output+"/fonts.json");check(file.open(QIODevice::WriteOnly),"Font receipt unavailable");file.write(QJsonDocument(QJsonObject{{"requested_family","Segoe UI"},{"actual_family",actual.family()},{"style",actual.styleName()},{"pixel_size",actual.pixelSize()},{"digit_advances",widths}}).toJson());}
        QWidget shell;shell.setStyleSheet("QWidget{background:#010b10;color:#c9eee7;}"+HorizonWorkspaceControlStyle());
        auto* layout=new QVBoxLayout(&shell);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
        auto* top=new QFrame;top->setObjectName("horizonCoinNavigation");top->setStyleSheet(HorizonPrimaryNavigationStyle());top->setFixedHeight(43);auto* bar=new QHBoxLayout(top);bar->setContentsMargins(14,5,14,5);
        int primary=0;for(const auto& label:{"PGN\nPRIME GAP SEARCH","WALLET\nPGN CORE CLIENT","SOCIAL\nCOMMUNITY SPACE"}){auto* button=new QPushButton(label);button->setCheckable(true);button->setChecked(QString(label).startsWith("SOCIAL"));button->setFixedSize(QList<int>{260,210,210}[primary++],33);bar->addWidget(button);}
        auto* indicator=new QLabel("REVISION PREVIEW · synthetic fixtures · no network");indicator->setStyleSheet("font:11px 'Segoe UI';color:#e6bd64;");bar->addWidget(indicator,1);layout->addWidget(top);
        social::Services fixtures{[](auto request,auto done){auto review=previewData(request);if(request.section==social::Section::Miners){review.miners.clear();const int requested=qEnvironmentVariableIntValue("PGN_SOCIAL_REVIEW_MINERS");const int amount=requested>=1&&requested<=18?requested:18;for(int n=0;n<amount;++n){auto m=miners()[1];m.id=QString("preview-operator-%1").arg(n,4,10,QChar('0'));m.socialMemberId="preview-canonical-"+QString::number(n);m.name=n<2?"Review homonym":QString("Review miner %1").arg(n+1,2,10,QChar('0'));m.validatedSearchPositions.value=QString::number(9007199254740993ULL-100000000000000ULL*n);m.ticketsReported.value=QString::number(1900-n*71);m.ticketsValidated.value=QString::number(1800-n*67);m.validatedAudits.value=QString::number(700-n*29);m.lastActivityHeight.value=QString::number(17000-n*4);if(n==16)m.validatedAudits.value.reset();review.miners.append(m);}}done(review,{});}};auto* socialView=new ui::SocialView(temp.path()+"/review/drafts.json",fixtures);layout->addWidget(socialView,1);
        auto identity=A();identity.subject="Review author A";socialView->setIdentity(identity);
        for(const QSize size:{QSize(1366,990),QSize(1920,1080)}){
            shell.resize(size);shell.show();app.processEvents();check(shell.size()==size,"Page forces oversized shell");
            for(const int i:{0,3}){
                socialView->selectSection(static_cast<social::Section>(i));app.processEvents();
                if(i==0){find<QPlainTextEdit>(*socialView,"pgn-social-draft-body-0")->setPlainText("I will keep the exact observation window in the draft before sharing it.");find<QPushButton>(*socialView,"pgn-social-save-0")->click();}
                if(i==3){auto* table=find<QTableWidget>(*socialView,"pgn-social-miners");fullyVisible(shell,find<QPushButton>(*socialView,"pgn-social-period-today"),"Today action clipped");fullyVisible(shell,find<QPushButton>(*socialView,"pgn-social-refresh-3"),"Refresh clipped");check(table->horizontalScrollBar()->maximum()==0,"Leaderboard essential columns require horizontal scrolling");check(table->width()>=shell.width()-60,"Leaderboard imposes empty lateral margins");}
                if(i==3)if(auto* cap=socialView->findChild<QWidget*>("pgn-social-ranking-toolbar")){
                    auto* title=find<QLabel>(*socialView,"pgn-social-ranking-title");
                    auto luminance=[](QColor color){auto linear=[](double v){return v<=0.04045?v/12.92:std::pow((v+0.055)/1.055,2.4);};return 0.2126*linear(color.redF())+0.7152*linear(color.greenF())+0.0722*linear(color.blueF());};
                    const auto fg=luminance(title->palette().color(QPalette::WindowText)),bg=luminance(cap->palette().color(QPalette::Window));
                    check((qMax(fg,bg)+0.05)/(qMin(fg,bg)+0.05)>=4.5,"Integrated leaderboard title inherits illegible contrast");
                }
                if(i==0){fullyVisible(shell,find<QPushButton>(*socialView,"pgn-social-send-0"),"Chat Send clipped");fullyVisible(shell,find<QPushButton>(*socialView,"pgn-social-save-0"),"Chat Save clipped");fullyVisible(shell,find<QLabel>(*socialView,"pgn-social-send-reason-0"),"Chat reason clipped");fullyVisible(shell,find<QLabel>(*socialView,"pgn-social-save-status-0"),"Chat autosave status clipped");check(find<QLabel>(*socialView,"pgn-social-save-status-0")->visibleRegion().contains(find<QLabel>(*socialView,"pgn-social-save-status-0")->rect()),"Autosave status clipped by composer parent");}
                fullyVisible(shell,find<QLabel>(*socialView,"pgn-social-status"),"Stable status line clipped");app.processEvents();
                if(i==0){auto* composer=find<QWidget>(*socialView,"pgn-social-composer-0");for(const auto& name:{"pgn-social-send-reason-0","pgn-social-save-status-0"}){auto* label=find<QLabel>(*socialView,name);check(composer->rect().contains(QRect(label->mapTo(composer,QPoint{}),label->size())),"Composer status overflows parent after layout");}auto* saved=find<QLabel>(*socialView,"pgn-social-save-status-0");check(saved->fontMetrics().horizontalAdvance(saved->text())<=saved->contentsRect().width(),"Saved status text clipped");}
                QImage image(shell.size(),QImage::Format_ARGB32);image.fill(Qt::transparent);shell.render(&image);check(image.save(output+QString("/social-%1-%2.png").arg(size.width()).arg(i)),"Capture not saved");
            }
        }
    }
    test::scroll::run(temp.path()+"/scroll/drafts.json",argc==3&&QString::fromLocal8Bit(argv[1])=="--render-scroll"?QString::fromLocal8Bit(argv[2]):QString{});
    std::cout<<"PASS: V4 mixed surfaces and contrast; safe BBCode; Chat/Leaderboard and scoped drafts; service-authoritative roles/refusal; canonical identity menus and public names; exact pagination/UTC/volume; send retry/restart; Posted vs Delivered; post-layout Chat scroll anchors/follow/unseen/channel isolation\n";
    std::cout<<"CHECKS "<<(checks+test::v4::checks+test::chat::checks+test::utc::checks+test::names::checks+test::scroll::checks)<<"\n";
    return 0;
 }catch(const std::exception& error){std::cerr<<"FAIL: "<<error.what()<<'\n';return 1;}
}
