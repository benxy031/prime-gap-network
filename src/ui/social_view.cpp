#include "ui/social_view.h"
#include "social/social_numbers.h"
#include "social/social_leaderboard_cache.h"
#include <QThreadPool>
#include "ui/render/horizon_coin_navigation.h"
#include "ui/render/horizon_workspace_style.h"
#include "ui/social_light_style.h"
#include "ui/social_entry_delegate.h"
#include "ui/quadrigantium_rosette.h"
#include "ui/social_leaderboard_table.h"
#include "ui/social_icons.h"
#include "ui/social_chat_style.h"
#include "ui/social_workspace_refinement.h"
#include "ui/social_compose_edit.h"
#include "ui/social_compose_frame.h"
#include <QStyleOptionHeader>
#include "ui/social_access_dialog.h"
#include "ui/social_controls.h"
#include "ui/social_presentation.h"
#include "ui/social_chat_list.h"
#include <QInputMethodEvent>
#include "ui/social_forum_thread.h"
#include <QFrame>
#include <QMenu>
#include <QClipboard>
#include <QApplication>
#include <QMouseEvent>
#include <QUuid>
#include <memory>
#include <QComboBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QScopedValueRollback>
#include <QScrollBar>
#include <QShortcut>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QStyleOptionFocusRect>
#include <QTableWidget>
#include <QTabBar>
#include <QTabWidget>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocumentFragment>
#include <algorithm>
namespace pgn::ui {
namespace {
const char* names[]={"Chat","Publications","Forum","Miners"};
const char* nouns[]={"messages","publications","topics","miners"};
constexpr int LoadedLimit=500;
constexpr int DocumentTargetProperty=QTextFormat::UserProperty+31,DocumentAuthorProperty=QTextFormat::UserProperty+32;
QLabel* text(const QString& value,QWidget* parent=nullptr){
    auto* p=new QLabel(value,parent);p->setTextFormat(Qt::PlainText);p->setWordWrap(false);return p;
}
QVBoxLayout* vertical(QWidget* widget,int margin=0){
    auto* layout=new QVBoxLayout(widget);layout->setContentsMargins(margin,margin,margin,margin);layout->setSpacing(6);return layout;
}
QString stamp(const QDateTime& value){return value.toUTC().toString("yyyy-MM-dd HH:mm 'UTC'");}
QString safeTip(const QString& value){return "<qt>"+value.left(1000).toHtmlEscaped().replace("\n","<br/>")+"</qt>";}
QString metadata(const social::Entry& entry){
    QStringList result;if(!entry.author.isEmpty())result<<entry.author;
    if(entry.createdUtc.isValid())result<<stamp(entry.createdUtc);
    if(!entry.category.isEmpty())result<<entry.category;return result.join(" · ");
}
void document(QTextBrowser* browser,const social::Entry& entry,bool local=false){
    const auto scroll=browser->verticalScrollBar()->value();browser->clear();auto cursor=browser->textCursor();
    const auto base=browser->font();QTextCharFormat normal;normal.setFont(base);normal.setForeground(QColor("#161616"));normal.setProperty(DocumentTargetProperty,entry.id);normal.setProperty(DocumentAuthorProperty,entry.authorId);
    if(!entry.title.isEmpty()){auto title=normal;auto titleFont=base;titleFont.setPixelSize(20);titleFont.setWeight(QFont::DemiBold);title.setFont(titleFont);
        cursor.insertText(entry.title+"\n",title);}
    auto info=normal;info.setForeground(QColor("#525252"));
    cursor.insertText(local?"Local draft · not published\n":metadata(entry)+"\n",info);
    if(!entry.reference.isEmpty())cursor.insertText("Ticket / Gold: "+entry.reference+"\n",info);
    cursor.insertText("\n",normal);if(entry.bbcode){social::bbcode::Document body;body.setDefaultFont(base);social::bbcode::render(body,entry.body,16000);cursor.insertFragment(QTextDocumentFragment(&body));}else cursor.insertText(entry.body,normal);cursor.insertText("\n",normal);
    for(const auto& reply:entry.replies){auto replyFormat=normal;replyFormat.setProperty(DocumentTargetProperty,reply.id);replyFormat.setProperty(DocumentAuthorProperty,reply.authorId);auto author=replyFormat;author.setFontWeight(QFont::DemiBold);
        cursor.insertText("\n"+reply.author+" · "+stamp(reply.createdUtc)+"\n",author);cursor.insertText(reply.body+"\n",replyFormat);}
    browser->verticalScrollBar()->setValue(scroll);
}

}
SocialView::SocialView(QString path,social::Services services,QWidget* parent)
    :QWidget(parent),services_(std::move(services)),store_(std::move(path)){
    leaderboardWarmTimer_.setSingleShot(true);
    connect(&leaderboardWarmTimer_,&QTimer::timeout,this,&SocialView::warmLeaderboard);
    setObjectName("pgn-social-workspace");
    setStyleSheet(SocialLightStyle()+SocialChatContrastStyle()+SocialReadableStyle()+SocialConversationStyle()+SocialWorkspaceRefinement());
    auto* box=vertical(this);box->setSpacing(0);tabs_=new QTabWidget;tabs_->setProperty("socialSurface",true);tabs_->setObjectName("pgn-social-sections");
    for(int i=0;i<4;++i){
        auto& p=pages_[i];auto* page=new QWidget;if(i==0)page->setObjectName("pgn-social-chat-surface");if(i==3)page->setObjectName("pgn-social-leaderboard-surface");auto* layout=vertical(page,14);
        auto* tools=new QHBoxLayout;tools->setSpacing(8);
        if(i==0){channelTitle_=text("Conversation");channelTitle_->setProperty("heading",true);channelTitle_->setObjectName("pgn-social-global-channel");tools->addWidget(channelTitle_);
            channelDescription_=text({});channelDescription_->setProperty("caption",true);channelDescription_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);tools->addWidget(channelDescription_,1);}
        if(i==3){
            p.filter=new SocialComboBox;p.filter->setObjectName("pgn-social-miner-window");p.filter->setAccessibleName("Miner observation window");
            p.filter->addItem("Total");for(const auto window:{social::Window::Day,social::Window::Week,social::Window::Month,social::Window::Hour})p.filter->addItem(social::windowLabel(window));p.filter->hide();
            p.filter->setToolTip("Confirmed activity on the current chain. Fixed UTC calendar intervals; weeks start Monday. Canonical block-header timestamps, not physical mining time.");
            auto* periods=new QWidget;periods->setObjectName("pgn-social-periods");p.filter->setParent(periods);p.filter->hide();auto* periodBox=new QHBoxLayout(periods);periodBox->setContentsMargins(3,3,3,3);periodBox->setSpacing(2);
            auto* group=new QButtonGroup(periods);group->setExclusive(true);
            const QStringList periodIds{"total","today","week","month","hour"};
            for(int n=0;n<5;++n){auto* button=new QPushButton(n?social::windowLabel(static_cast<social::Window>(n)):QString("Total"));button->setObjectName("pgn-social-period-"+periodIds[n]);button->setAccessibleName(button->text()+" leaderboard period");button->setCheckable(true);button->setChecked(!n);button->setMinimumHeight(32);button->setVisible(n<2);group->addButton(button,n);periodBox->addWidget(button);connect(button,&QPushButton::clicked,this,[this,n]{pages_[3].filter->setCurrentIndex(n);});}
            connect(p.filter,&QComboBox::currentIndexChanged,group,[group](int n){if(group->button(n))group->button(n)->setChecked(true);});tools->addWidget(periods);
            p.windowInfo=text({});p.windowInfo->setObjectName("pgn-social-miner-interval");p.windowInfo->setProperty("caption",true);
        }
        p.search=new QLineEdit;p.search->setMaxLength(160);p.search->setPlaceholderText(i==0?"Find in loaded messages":"Filter loaded "+QString(nouns[i]));
        p.search->setAccessibleName(QString("Filter loaded %1").arg(nouns[i]));p.search->setObjectName(QString("pgn-social-search-%1").arg(i));tools->addWidget(p.search,i==0?0:1);
        if(i==0){p.search->setFixedWidth(184);p.search->setPlaceholderText("Search messages");p.search->addAction(SocialIcon(SocialGlyph::Search,QColor("#9bb9bf")),QLineEdit::LeadingPosition);
            auto* members=new QPushButton("Members");members->setObjectName("pgn-social-toggle-members");members->setCheckable(true);members->setChecked(true);members->setIcon(SocialIcon(SocialGlyph::Members,QColor("#bdd5d8")));members->setIconSize({18,18});tools->addWidget(members);
            connect(members,&QPushButton::toggled,this,[this](bool shown){membersPane_->setVisible(shown);});}
        p.refresh=new QPushButton("Refresh");p.refresh->setObjectName(QString("pgn-social-refresh-%1").arg(i));if(i==0){p.refresh->setIcon(SocialIcon(SocialGlyph::Refresh,QColor("#bdd5d8")));p.refresh->setIconSize({17,17});}tools->addWidget(p.refresh);
        connect(p.refresh,&QPushButton::clicked,this,[this,i]{read(i);});connect(p.search,&QLineEdit::textChanged,this,[this,i]{filter(i);});
        auto* browse=new QWidget;auto* browseBox=vertical(browse);
        if(i==3){browse->setObjectName("pgn-social-ranking-content");browseBox->setSpacing(0);
            auto* toolbar=new QWidget;toolbar->setObjectName("pgn-social-ranking-toolbar");auto* cap=vertical(toolbar,16);cap->setSpacing(12);
            auto* title=text("Leaderboard");title->setObjectName("pgn-social-ranking-title");auto* titleRow=new QHBoxLayout;titleRow->setSpacing(12);titleRow->addWidget(title);titleRow->addStretch();titleRow->addWidget(p.windowInfo);cap->addLayout(titleRow);
            p.search->setPlaceholderText("Search loaded miners");p.search->setMaximumWidth(330);p.search->setMinimumWidth(230);tools->setStretchFactor(p.search,0);tools->insertStretch(1,1);p.refresh->setMinimumHeight(32);
            p.search->addAction(SocialIcon(SocialGlyph::Search,QColor("#536f72")),QLineEdit::LeadingPosition);p.refresh->setIcon(SocialIcon(SocialGlyph::Refresh,QColor("#f1fbf7")));p.refresh->setIconSize({17,17});
            cap->addLayout(tools);browseBox->addWidget(toolbar);
        }else browseBox->addLayout(tools);
        p.dataSlot=i==3?new LeaderboardStack:new QStackedWidget;p.dataSlot->setObjectName(QString("pgn-social-data-slot-%1").arg(i));
        if(i==3){
            p.ranking=new LeaderboardTable;p.ranking->setObjectName("pgn-social-miners");
            auto* header=new LeaderboardHeader(p.ranking);p.ranking->setHorizontalHeader(header);
            p.ranking->setHorizontalHeaderLabels({"Miner","Validated positions","Tickets reported","Tickets validated","Audits validated","Last activity · block","Quadrigantium","Gold","Silver","Bronze","Rust","Largest recorded gap"});
            header->setSectionResizeMode(0,QHeaderView::Stretch);const int widths[]{0,158,116,116,112,152};for(int col=1;col<6;++col){header->setSectionResizeMode(col,QHeaderView::Fixed);header->resizeSection(col,widths[col]);}
            p.ranking->setAccessibleName("Leaderboard of authenticated network activity. Rank among loaded rows for the selected sort.");
            p.ranking->setStyleSheet("QTableWidget#pgn-social-miners{background:#f2f6f3;color:#203c43;border:0;border-radius:12px;padding:0;outline:0;}QTableWidget#pgn-social-miners::item{padding:0;border:0;}QTableWidget#pgn-social-miners QWidget{background:#f2f6f3;}QHeaderView{background:#e0ebe5;border:0;padding:0;}QScrollBar:vertical{background:#e1eaeb;width:8px;}QScrollBar::handle:vertical{background:#8ba6ab;min-height:24px;border-radius:4px;}");
            // Presentation only: the current authenticated snapshot supplies no
            // exhaustive per-operator tier totals. Never infer zero/medals.
            const MeritTier tiers[]{MeritTier::Gold,MeritTier::Silver,MeritTier::Bronze,MeritTier::NearMiss};
            for(int col=6;col<11;++col){auto* item=p.ranking->horizontalHeaderItem(col);item->setIcon(col==6?quadrigantiumRosetteIcon():gapRosetteIcon(tiers[col-7],false));item->setData(Qt::AccessibleTextRole,item->text());item->setToolTip(col==6?QString::fromUtf8("Quadrigantium — merit 40"):gapRosetteDescription(tiers[col-7],false));item->setData(Qt::AccessibleTextRole,item->toolTip());item->setData(Qt::AccessibleDescriptionRole,item->toolTip()+" · Recorded discoveries from validated reports; not coin payments. Q is independent of G/S/B/R.");header->setSectionResizeMode(col,QHeaderView::Fixed);header->resizeSection(col,44);item->setSizeHint(QSize(44,58));}
            for(int n=0;n<5;++n)header->moveSection(header->visualIndex(6+n),1+n);
            header->moveSection(header->visualIndex(11),7);
            p.ranking->horizontalHeaderItem(11)->setToolTip("Largest unique gap first disclosed in the selected UTC period by this miner, among currently qualified reports. Canonical publication header time; not physical discovery time or coin payments.");
            header->setSortIndicator(1,Qt::DescendingOrder);header->onSort=[this](int col){if(leaderboardAwardColumn(col))return;sortDescending_=sortColumn_==col?!sortDescending_:col!=0;sortColumn_=col;renderMiners();};
            p.ranking->verticalHeader()->hide();p.ranking->setEditTriggers(QAbstractItemView::NoEditTriggers);p.ranking->setSelectionBehavior(QAbstractItemView::SelectRows);
            p.ranking->setSelectionMode(QAbstractItemView::SingleSelection);p.ranking->setWordWrap(false);p.ranking->verticalHeader()->setMinimumSectionSize(36);p.ranking->verticalHeader()->setMaximumSectionSize(36);p.ranking->verticalHeader()->setDefaultSectionSize(36);
            p.dataSlot->addWidget(p.ranking);
             connect(p.filter,&QComboBox::currentIndexChanged,this,[this](int index){selectLeaderboardWindow(index);});
        }else{
        p.list=i==0?new SocialChatList:new QListWidget;p.list->setWordWrap(true);p.list->setMouseTracking(true);p.list->viewport()->setAttribute(Qt::WA_Hover,true);p.list->setObjectName(QString("pgn-social-list-%1").arg(i));p.list->setAccessibleName(i==0?"Global message chronology":i==1?"Publication feed":"Forum topics");
            p.list->setItemDelegate(new SocialEntryDelegate(i,p.list));p.list->setMouseTracking(true);
            if(i==0)p.list->installEventFilter(this);if(i==2){p.list->viewport()->installEventFilter(this);p.list->setCursor(Qt::PointingHandCursor);}
            p.list->setSelectionMode(QAbstractItemView::SingleSelection);p.dataSlot->addWidget(p.list);
            if(i>0){connect(p.list,&QListWidget::itemActivated,this,[this,i]{openRead(i);});connect(p.list,&QListWidget::itemClicked,this,[this,i]{openRead(i);});}
        }
        auto* emptyPane=new QWidget;emptyPane->setObjectName(QString("pgn-social-empty-pane-%1").arg(i));auto* emptyBox=vertical(emptyPane,12);p.empty=text({});p.empty->setWordWrap(true);p.empty->setAlignment(Qt::AlignLeft|Qt::AlignTop);
        p.empty->setObjectName(QString("pgn-social-empty-%1").arg(i));emptyBox->addWidget(p.empty);emptyBox->addStretch();p.dataSlot->addWidget(emptyPane);
        p.dataSlot->setCurrentIndex(1);
        if(i==2){auto* headers=new QHBoxLayout;headers->setContentsMargins(24,4,12,4);headers->setSpacing(0);headers->addWidget(text("Topic"),1);for(const auto& spec:QList<QPair<QString,int>>{{"Participants",140},{"Replies",88},{"Activity",140},{"State",100}}){auto* label=text(spec.first);label->setFixedWidth(spec.second);headers->addWidget(label);}browseBox->addLayout(headers);}
        browseBox->addWidget(p.dataSlot,1);
        auto* pager=new QHBoxLayout;p.count=text({});p.count->setObjectName(QString("pgn-social-loaded-%1").arg(i));pager->addWidget(p.count,1);
        if(i==0){auto* chat=static_cast<SocialChatList*>(p.list);auto* latest=new QPushButton("New messages · Jump to latest");latest->setObjectName("pgn-social-new-messages");latest->setAccessibleName("New messages. Jump to latest message");latest->hide();pager->addWidget(latest);chat->unseenChanged=[latest](bool unseen){latest->setVisible(unseen);};connect(latest,&QPushButton::clicked,chat,[this,chat]{chat->jumpToLatest();read(0);});}
        p.more=new QPushButton(i==0?"Older messages":i==1?"Older publications":i==2?"More topics":"Load more");p.more->setObjectName(QString("pgn-social-more-%1").arg(i));pager->addWidget(p.more);browseBox->addLayout(pager);
        connect(p.more,&QPushButton::clicked,this,[this,i]{read(i,true);});
        if(i==0){
            layout->setContentsMargins(0,0,0,0);auto* columns=new QHBoxLayout;columns->setSpacing(0);
            auto* rail=new QFrame;rail->setObjectName("pgn-social-channels-pane");rail->setFixedWidth(190);auto* railBox=vertical(rail,14);railBox->setSpacing(12);
            auto* heading=text("Network");heading->setProperty("heading",true);railBox->addWidget(heading);auto* caption=text("Text channels");caption->setProperty("caption",true);railBox->addWidget(caption);
            channels_=new QListWidget;channels_->setObjectName("pgn-social-chat-channels");channels_->setAccessibleName("Text channels from the authenticated service");railBox->addWidget(channels_,1);
            connect(channels_,&QListWidget::itemActivated,this,[this]{selectChannel();});connect(channels_,&QListWidget::itemClicked,this,[this]{selectChannel();});
            auto* access=new QPushButton("Roles && members");access->setObjectName("pgn-social-open-access");railBox->addWidget(access);connect(access,&QPushButton::clicked,this,[this]{openAccess();});columns->addWidget(rail);
            channels_->setContextMenuPolicy(Qt::CustomContextMenu);connect(channels_,&QListWidget::customContextMenuRequested,this,[this](QPoint pos){auto* item=channels_->itemAt(pos);if(!item)item=channels_->currentItem();if(!item)return;const QString id=item->data(Qt::UserRole).toString();auto* menu=new QMenu(this);menu->setProperty("socialSurface",true);auto* copy=menu->addAction("Copy channel ID");connect(copy,&QAction::triggered,this,[id]{QApplication::clipboard()->setText(id);});auto* permissions=menu->addAction("Channel permissions…");connect(permissions,&QAction::triggered,this,[this,id]{openAccess({},id);});connect(menu,&QMenu::aboutToHide,menu,&QObject::deleteLater);menu->popup(channels_->viewport()->mapToGlobal(pos));});
            auto* conversation=new QWidget;conversation->setObjectName("pgn-social-conversation");auto* cb=vertical(conversation,14);cb->setSpacing(12);cb->addWidget(browse,1);cb->addWidget(makeComposer(i));columns->addWidget(conversation,1);
            auto* side=new QFrame;side->setObjectName("pgn-social-members-pane");side->setFixedWidth(204);membersPane_=side;auto* mb=vertical(side,14);memberCount_=text("Participants unavailable");memberCount_->setProperty("caption",true);mb->addWidget(memberCount_);
            members_=new QListWidget;members_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);members_->setTextElideMode(Qt::ElideRight);members_->setObjectName("pgn-social-chat-members");members_->setItemDelegate(new SocialMemberDelegate(members_));members_->setAccessibleName("Authenticated channel participants and reported presence");mb->addWidget(members_,1);
            connect(members_,&QListWidget::itemActivated,this,[this]{if(members_->currentItem())openAccess(members_->currentItem()->data(Qt::UserRole).toString());});
            members_->setContextMenuPolicy(Qt::CustomContextMenu);connect(members_,&QListWidget::customContextMenuRequested,this,[this](QPoint pos){auto* item=members_->itemAt(pos);if(!item)item=members_->currentItem();if(!item||item->data(Qt::UserRole).toString().isEmpty())return;const auto id=item->data(Qt::UserRole).toString();auto* menu=new QMenu(this);menu->setProperty("socialSurface",true);auto* copy=menu->addAction("Copy full identity");connect(copy,&QAction::triggered,this,[id]{QApplication::clipboard()->setText(id);});auto* manage=menu->addAction("Roles & member actions…");connect(manage,&QAction::triggered,this,[this,id]{openAccess(id);});connect(menu,&QMenu::aboutToHide,menu,&QObject::deleteLater);menu->popup(members_->viewport()->mapToGlobal(pos.x()<0?QPoint(20,20):pos));});
            columns->addWidget(side);layout->addLayout(columns,1);
        }
        else if(i==3){layout->setContentsMargins(24,20,24,20);layout->addWidget(browse,1);
            page->setStyleSheet("QWidget#pgn-social-leaderboard-surface{background:#041319;}QWidget#pgn-social-ranking-content{background:transparent;}QLabel#pgn-social-ranking-title{font:600 24px 'Segoe UI';color:#e3f1ef;background:transparent;}QWidget#pgn-social-periods{background:#0b2c34;border:1px solid #244a53;border-radius:10px;}QWidget#pgn-social-periods QPushButton{font:600 12px 'Segoe UI';border:0;border-radius:7px;padding:0 20px;min-height:32px;color:#afc8ca;background:transparent;}QWidget#pgn-social-periods QPushButton:hover{background:#16434c;color:#eff8f7;}QWidget#pgn-social-periods QPushButton:checked{background:#dceae6;color:#214b44;}QWidget#pgn-social-periods QPushButton:pressed{background:#b9d8cd;color:#173e37;}QWidget#pgn-social-periods QPushButton:focus{border:2px solid #71aaa0;padding:0 18px;}QLineEdit#pgn-social-search-3{background:#12343c;border:1px solid #355e67;border-radius:8px;color:#e0eeed;min-height:24px;padding:5px 12px;}QLineEdit#pgn-social-search-3:focus{border:2px solid #74aaa2;padding:4px 11px;}QPushButton#pgn-social-refresh-3{background:#245e55;border:1px solid #3b7c70;border-radius:8px;color:#eff8f5;padding:0 20px;min-height:34px;font:600 12px 'Segoe UI';}QPushButton#pgn-social-refresh-3:hover{background:#2e7365;border-color:#609d8f;}QPushButton#pgn-social-refresh-3:pressed{background:#19493f;}QPushButton#pgn-social-refresh-3:focus{border:2px solid #97c5b8;padding:0 19px;}QPushButton#pgn-social-refresh-3:disabled{background:#17362f;color:#8ea9a3;border-color:#2a5149;}QLabel#pgn-social-loaded-3{font:12px 'Segoe UI';color:#8eabb2;}QLabel#pgn-social-miner-interval{font:12px 'Segoe UI';color:#a3bfc1;}");
            // This page has legacy local rules, so the scoped refinement must
            // also be applied here; a parent stylesheet cannot override them.
            page->setStyleSheet(page->styleSheet()+SocialWorkspaceRefinement());
        }
        else{
            auto* create=new QPushButton(i==1?"New publication":"New topic");create->setObjectName(QString("pgn-social-new-%1").arg(i));tools->addWidget(create);
            connect(create,&QPushButton::clicked,this,[this,i]{openComposer(i);});
            p.journey=new QStackedWidget;p.journey->setObjectName(QString("pgn-social-journey-%1").arg(i));
            if(i==2){auto* overview=new QWidget;auto* columns=new QHBoxLayout(overview);columns->setContentsMargins(0,0,0,0);columns->setSpacing(12);auto* categories=new QWidget;categories->setFixedWidth(164);auto* cb=vertical(categories);auto* heading=text("Categories");heading->setProperty("heading",true);cb->addWidget(heading);forumCategories_=new QListWidget;forumCategories_->setObjectName("pgn-social-forum-categories");forumCategories_->setAccessibleName("Forum categories");forumCategories_->addItems(social::categories());forumCategories_->setCurrentRow(0);forumCategories_->setStyleSheet("QListWidget{background:#09262d;color:#c9eee7;border:0;}QListWidget::item:selected{background:#16434a;color:#ffffff;}");cb->addWidget(forumCategories_,1);connect(forumCategories_,&QListWidget::currentRowChanged,this,[this]{filter(2);});columns->addWidget(categories);columns->addWidget(browse,1);p.journey->addWidget(overview);}else p.journey->addWidget(browse);
            auto* reading=new QWidget;auto* rb=vertical(reading);auto* readActions=new QHBoxLayout;
            auto* backButton=new QPushButton(i==1?"← Publications":"← Topics");backButton->setObjectName(QString("pgn-social-back-read-%1").arg(i));readActions->addWidget(backButton);readActions->addStretch();
            connect(backButton,&QPushButton::clicked,this,[this,i]{back(i);});
            rb->addLayout(readActions);
            if(i==2){p.threadTitle=text({});p.threadTitle->setProperty("heading",true);p.threadTitle->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);p.threadTitle->setObjectName("pgn-social-forum-title");rb->addWidget(p.threadTitle);p.thread=new ForumThread;rb->addWidget(p.thread,1);
                p.thread->onReply=[this](QString post){replyToPost(post);};p.thread->onQuote=[this](QString post,QString quote){replyToPost(post,quote);};p.thread->onMenu=[this](QString post,QPoint pos){contextMenu(2,pos,true,post);};
                p.thread->onReact=[this](QString post,QString reaction){reactToPost(post,reaction);};p.thread->reactionUnavailable=[this](QString post,QString reaction){const auto r=postRequest(post,social::AccessAction::ReactContent,reaction);return social::unavailable(access_?&*access_:nullptr,r,bool(services_.manage));};
                p.thread->setContextMenuPolicy(Qt::CustomContextMenu);connect(p.thread,&QListWidget::customContextMenuRequested,this,[this](QPoint pos){contextMenu(2,pos,true);});auto* footer=new QHBoxLayout;p.threadState=text({});p.threadState->setObjectName("pgn-social-forum-thread-state");footer->addWidget(p.threadState,1);p.moreReplies=new QPushButton("More replies");p.moreReplies->setObjectName("pgn-social-more-replies");footer->addWidget(p.moreReplies);connect(p.moreReplies,&QPushButton::clicked,this,[this]{readThread();});rb->addLayout(footer);
            }else{p.detail=new QTextBrowser;p.detail->setDocument(new social::bbcode::Document(p.detail));p.detail->document()->setDocumentMargin(24);p.detail->setOpenLinks(false);p.detail->setOpenExternalLinks(false);p.detail->setObjectName(QString("pgn-social-detail-%1").arg(i));rb->addWidget(p.detail,1);p.detail->setContextMenuPolicy(Qt::CustomContextMenu);connect(p.detail,&QTextBrowser::customContextMenuRequested,this,[this,i](QPoint pos){contextMenu(i,pos,true);});}
            if(i==2){auto* replyActions=new QHBoxLayout;p.reply=new QPushButton("Reply");p.reply->setObjectName("pgn-social-reply-topic");replyActions->addStretch();replyActions->addWidget(p.reply);rb->addLayout(replyActions);connect(p.reply,&QPushButton::clicked,this,[this]{openComposer(2,true);});}
            p.journey->addWidget(reading);p.journey->addWidget(makeComposer(i));
            if(i==1||i==2){auto* previewPane=new QWidget;auto* pv=vertical(previewPane);auto* bar=new QHBoxLayout;
                auto* returnDraft=new QPushButton("← Edit draft");returnDraft->setObjectName("pgn-social-back-preview");bar->addWidget(returnDraft);bar->addWidget(text("Preview · local draft"),1);pv->addLayout(bar);
                returnDraft->setObjectName(i==1?"pgn-social-back-preview":"pgn-social-forum-back-preview");p.preview=new QTextBrowser;p.preview->setDocument(new social::bbcode::Document(p.preview));p.preview->document()->setDocumentMargin(24);p.preview->setOpenLinks(false);p.preview->setOpenExternalLinks(false);p.preview->setObjectName(i==1?"pgn-social-publication-preview":"pgn-social-forum-preview");pv->addWidget(p.preview,1);
                connect(returnDraft,&QPushButton::clicked,this,[this,i]{pages_[i].journey->setCurrentIndex(2);pages_[i].draft->setFocus();});p.journey->addWidget(previewPane);}
            layout->addWidget(p.journey,1);
        }
        tabs_->addTab(page,names[i]);
        QWidget* target=i==3?static_cast<QWidget*>(p.ranking):static_cast<QWidget*>(p.list);target->setContextMenuPolicy(Qt::CustomContextMenu);connect(target,&QWidget::customContextMenuRequested,this,[this,i](QPoint pos){contextMenu(i,pos);});
        if(i==3){QWidget::setTabOrder(p.refresh,p.ranking->horizontalHeader());QWidget::setTabOrder(p.ranking->horizontalHeader(),p.ranking);}
    }
    auto* nav=HorizonCoinNavigation(tabs_,{{"01  CHAT","GLOBAL CHANNEL"},{"02  PUBLICATIONS","IDEAS + UPDATES"},{"03  FORUM","TOPICS + REPLIES"},{"04  MINERS","VERIFIED ACTIVITY"}},this);
    // Retain legacy pages/drafts, remove them from every user navigation route.
    tabs_->setTabVisible(1,false);tabs_->setTabVisible(2,false);tabs_->setTabText(3,"Leaderboard");
    for(const int legacy:{1,2}){tabs_->widget(legacy)->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Ignored);tabs_->widget(legacy)->setMinimumSize(0,0);}
    if(auto* group=nav->findChild<QButtonGroup*>()){
        group->button(1)->hide();group->button(2)->hide();
        group->button(3)->setText("02  LEADERBOARD\nVERIFIED ACTIVITY");
    }
    nav->setStyleSheet(nav->styleSheet()+"QPushButton{font:600 12px 'Segoe UI';}QPushButton:focus{border:2px solid #7eafb8;}");box->addWidget(nav);
    auto* context=new QWidget;context->setProperty("socialSurface",true);context->setProperty("socialChrome",true);context->setFixedHeight(30);auto* contextBox=new QHBoxLayout(context);contextBox->setContentsMargins(14,2,14,2);contextBox->setSpacing(12);
    account_=text({});account_->setObjectName("pgn-social-account");account_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);contextBox->addWidget(account_,1);
    account_->setFocusPolicy(Qt::StrongFocus);account_->setAccessibleDescription("Public Social identity; Shift+F10 opens identity and name-link actions.");account_->setContextMenuPolicy(Qt::CustomContextMenu);connect(account_,&QLabel::customContextMenuRequested,this,[this](QPoint pos){if(identity_.subject.isEmpty())return;auto* menu=new QMenu(this);auto* copy=menu->addAction("Copy full identity");connect(copy,&QAction::triggered,this,[this]{QApplication::clipboard()->setText(identity_.subject);});auto* link=menu->addAction("Link / withdraw public mining name...");link->setEnabled(bool(services_.publicNameLink));connect(link,&QAction::triggered,this,[this]{if(services_.publicNameLink)services_.publicNameLink(identity_,{});});connect(menu,&QMenu::aboutToHide,menu,&QObject::deleteLater);menu->popup(account_->mapToGlobal(pos));});
    network_=text({});network_->setObjectName("pgn-social-network");network_->setAlignment(Qt::AlignRight|Qt::AlignVCenter);contextBox->addWidget(network_,1);box->addWidget(context);
    status_=text({});status_->setProperty("socialSurface",true);status_->setProperty("socialChrome",true);status_->setObjectName("pgn-social-status");status_->setFixedHeight(22);status_->setContentsMargins(0,0,0,0);status_->setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);status_->setMinimumWidth(180);status_->setMaximumWidth(540);contextBox->insertWidget(1,status_);box->addWidget(tabs_,1);
    tabs_->setFocusPolicy(Qt::NoFocus);tabs_->tabBar()->setFocusPolicy(Qt::NoFocus);
    auto* periodClock=new QTimer(this);
    periodClock->setInterval(1000);
    connect(periodClock,&QTimer::timeout,this,[this]{refreshExpiredLeaderboardWindow();});
    periodClock->start();
    connect(tabs_,&QTabWidget::currentChanged,this,[this](int i){
        updateStatus();
        if(!pages_[i].loaded || (i==3 && pages_[i].leaderboardSaved))read(i);
    });
    for(int i=0;i<2;++i){const int page=i==0?0:3;auto* shortcut=new QShortcut(QKeySequence(QString("Ctrl+%1").arg(i+1)),this);shortcut->setContext(Qt::WidgetWithChildrenShortcut);connect(shortcut,&QShortcut::activated,this,[this,page]{selectSection(static_cast<social::Section>(page));});}
    auto* save=new QShortcut(QKeySequence::Save,this);save->setContext(Qt::WidgetWithChildrenShortcut);connect(save,&QShortcut::activated,this,[this]{saveDrafts();});
    auto* toggleMembers=new QShortcut(QKeySequence("Ctrl+U"),this);toggleMembers->setContext(Qt::WidgetWithChildrenShortcut);connect(toggleMembers,&QShortcut::activated,this,[this]{if(tabs_->currentIndex()==0)findChild<QPushButton*>("pgn-social-toggle-members")->toggle();});
    for(const auto sequence:{QKeySequence("Shift+F10"),QKeySequence(Qt::Key_Menu)}){auto* menu=new QShortcut(sequence,this);menu->setObjectName("pgn-social-keyboard-context");menu->setContext(Qt::WidgetWithChildrenShortcut);connect(menu,&QShortcut::activated,this,[this]{
        const auto* focused=QApplication::focusWidget();if(tabs_->currentIndex()==0&&(focused==channels_||focused==members_)){auto* list=focused==channels_?channels_:members_;QMetaObject::invokeMethod(list,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,QPoint(-1,-1)));return;}
        const int i=tabs_->currentIndex();QString post;if(i==2&&pages_[i].journey->currentIndex()==1)for(auto* widget=QApplication::focusWidget();widget&&widget!=this;widget=widget->parentWidget())if(widget->property("forumPostId").isValid()){post=widget->property("forumPostId").toString();break;}contextMenu(i,{-1,-1},i>0&&i<3&&pages_[i].journey->currentIndex()==1,post);});}
    for(const bool backwards:{false,true}){auto* jump=new QShortcut(QKeySequence(backwards?"Shift+F6":"F6"),this);jump->setContext(Qt::WidgetWithChildrenShortcut);connect(jump,&QShortcut::activated,this,[this,backwards]{
        if(tabs_->currentIndex()!=0)return;QVector<QWidget*> zones{channels_,pages_[0].list};if(membersPane_->isVisible())zones<<members_;zones<<pages_[0].draft;
        int current=-1;for(int i=0;i<zones.size();++i)if(zones[i]->hasFocus()||zones[i]->isAncestorOf(QApplication::focusWidget()))current=i;
        zones[(current+(backwards?zones.size()-1:1)+zones.size())%zones.size()]->setFocus();});}
    auto* escape=new QShortcut(QKeySequence(Qt::Key_Escape),this);escape->setContext(Qt::WidgetWithChildrenShortcut);connect(escape,&QShortcut::activated,this,[this]{back(tabs_->currentIndex());});
    autosave_.setSingleShot(true);autosave_.setInterval(500);connect(&autosave_,&QTimer::timeout,this,[this]{saveDrafts();});
    updateAccess();for(int i=0;i<3;++i)loadEditor(i);
}
QWidget* SocialView::makeComposer(int i){
    auto& p=pages_[i];auto* compose=new QWidget;compose->setObjectName(QString("pgn-social-composer-%1").arg(i));auto* box=vertical(compose);QHBoxLayout* chatTools=nullptr;if(i==0){box->setContentsMargins(12,7,12,6);box->setSpacing(4);}
    if(i>0){auto* bar=new QHBoxLayout;auto* button=new QPushButton(i==1?"← Publications":"← Topic");button->setObjectName(QString("pgn-social-back-compose-%1").arg(i));bar->addWidget(button);
        p.composeContext=text(i==1?"New publication":"New topic");p.composeContext->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);bar->addWidget(p.composeContext,1);box->addLayout(bar);connect(button,&QPushButton::clicked,this,[this,i]{back(i);});
        if(i==2){p.categoryRow=new QWidget;auto* row=new QHBoxLayout(p.categoryRow);row->setContentsMargins(0,0,0,0);row->addWidget(text("Category"));p.category=new SocialComboBox;p.category->addItems(social::categories());p.category->setObjectName("pgn-social-topic-category");row->addWidget(p.category,1);box->addWidget(p.categoryRow);}
        p.titleRow=new QWidget;auto* row=new QHBoxLayout(p.titleRow);row->setContentsMargins(0,0,0,0);row->addWidget(text("Title"));
        p.title=new QLineEdit;p.title->setMaxLength(160);p.title->setAccessibleName(i==1?"Publication title":"Topic title");p.title->setObjectName(QString("pgn-social-draft-title-%1").arg(i));row->addWidget(p.title,1);box->addWidget(p.titleRow);
    }
    p.draft=i==0?new SocialComposeEdit:new QPlainTextEdit;p.draft->setTabChangesFocus(true);p.draft->setMinimumHeight(60);p.draft->setObjectName(QString("pgn-social-draft-body-%1").arg(i));p.draft->setAccessibleName(QString("%1 draft text").arg(names[i]));
    p.draft->setPlaceholderText(i==0?"Write a message":i==1?"Write your publication…":"Write a topic or reply…");
    {auto palette=p.draft->palette();palette.setColor(QPalette::PlaceholderText,QColor("#526e78"));p.draft->setPalette(palette);}
    if(i==0){
        auto* toolbar=new QHBoxLayout;chatTools=toolbar;toolbar->setSpacing(3);
        for(const auto& tag:{"b","i","u","s"}){const QString name=tag;const QString label=name=="b"?"Bold":name=="i"?"Italic":name=="u"?"Underline":"Strikethrough";auto* button=new QPushButton(name.toUpper());button->setObjectName("pgn-social-format-"+name);button->setProperty("composeTool",true);button->setAccessibleName(label);button->setToolTip(label);button->setCheckable(true);button->setFixedSize(32,30);button->setText({});button->setIcon(SocialIcon(name=="b"?SocialGlyph::Bold:name=="i"?SocialGlyph::Italic:name=="u"?SocialGlyph::Underline:SocialGlyph::Strike,QColor("#385c5b")));button->setIconSize({18,18});auto font=button->font();font.setBold(name=="b");font.setItalic(name=="i");font.setUnderline(name=="u");font.setStrikeOut(name=="s");button->setFont(font);toolbar->addWidget(button);connect(button,&QPushButton::clicked,this,[this,name]{formatChat(name);});}
        const auto sync=[this]{const auto f=pages_[0].draft->currentCharFormat();for(const auto& tag:QStringList{"b","i","u","s"}){auto* button=findChild<QPushButton*>("pgn-social-format-"+tag);if(button)button->setChecked(tag=="b"?f.fontWeight()>=QFont::DemiBold:tag=="i"?f.fontItalic():tag=="u"?f.fontUnderline():f.fontStrikeOut());}};
        static_cast<SocialComposeEdit*>(p.draft)->formatChanged=sync;connect(p.draft,&QPlainTextEdit::cursorPositionChanged,this,sync);connect(p.draft,&QPlainTextEdit::selectionChanged,this,sync);connect(p.draft,&QPlainTextEdit::textChanged,this,sync);
        toolbar->addStretch();
        chatEditor_=new SocialComposeFrame(p.draft);box->addWidget(chatEditor_);
    }else{
        if(i==2){auto* toolbar=new QHBoxLayout;for(const auto& tag:{"b","i","u","s","quote","code","url"}){const QString t=tag;auto* button=new QPushButton(t=="url"?"Link":t=="quote"?"Quote":t=="code"?"Code":t.toUpper());button->setObjectName("pgn-social-forum-bbcode-"+t);button->setToolTip(social::bbcode::opening(t)+"…[/"+t+"]");toolbar->addWidget(button);connect(button,&QPushButton::clicked,this,[this,t]{formatDraft(2,t);});}toolbar->addStretch();box->addLayout(toolbar);}
        box->addWidget(p.draft,1);
    }
    if(i==1){auto* row=new QHBoxLayout;row->addWidget(text("Ticket / Gold reference (optional)"));p.reference=new QLineEdit;p.reference->setMaxLength(256);p.reference->setAccessibleName("Optional ticket or Gold reference");p.reference->setObjectName("pgn-social-publication-reference");row->addWidget(p.reference,1);box->addLayout(row);}
    auto* actions=new QHBoxLayout;actions->setSpacing(4);if(i==0)actions->addLayout(chatTools,1);p.saved=text({});p.saved->setObjectName(QString("pgn-social-save-status-%1").arg(i));p.saved->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);if(i)actions->addWidget(p.saved,1);
    p.counter=text({});p.counter->setObjectName(QString("pgn-social-counter-%1").arg(i));if(i==0)p.counter->setToolTip("Formatting counts toward message capacity.");actions->addWidget(p.counter);
    p.save=new QPushButton("Save draft");p.save->setObjectName(QString("pgn-social-save-%1").arg(i));actions->addWidget(p.save);
    auto* clear=new QPushButton("Clear");clear->setObjectName(QString("pgn-social-clear-%1").arg(i));actions->addWidget(clear);
    if(i==1||i==2){auto* preview=new QPushButton("Preview");preview->setObjectName(i==1?"pgn-social-preview-publication":"pgn-social-preview-forum");actions->addWidget(preview);connect(preview,&QPushButton::clicked,this,[this,i]{previewPublication(i);});}
    p.send=new QPushButton(i==0?"Send":i==1?"Publish":"Post topic");p.send->setObjectName(QString("pgn-social-send-%1").arg(i));p.send->setEnabled(false);actions->addWidget(p.send);box->addLayout(actions);
    if(i==0){p.save->setMinimumSize(84,30);clear->setMinimumSize(58,30);p.send->setMinimumSize(96,36);p.send->setToolTip("Enter sends · Shift+Enter starts a new line");p.draft->installEventFilter(this);p.send->setIcon(SocialIcon(SocialGlyph::Send,QColor("#f3ffff")));}
    p.reason=text("Sending unavailable · no authenticated send service connected.");p.reason->setObjectName(QString("pgn-social-send-reason-%1").arg(i));if(i==0){p.reason->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);p.saved->setFixedWidth(150);p.saved->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Preferred);auto* notices=new QHBoxLayout;notices->addWidget(p.reason,1);notices->addWidget(p.saved);box->addLayout(notices);}else box->addWidget(p.reason);
    if(i==0)connect(p.send,&QPushButton::clicked,this,[this]{sendChat();});
    connect(p.save,&QPushButton::clicked,this,[this]{saveDrafts();});connect(clear,&QPushButton::clicked,this,[this,i]{
        pages_[i].draft->clear();if(!pages_[i].replyMode){if(pages_[i].title)pages_[i].title->clear();if(pages_[i].reference)pages_[i].reference->clear();}saveDrafts();
    });
    connect(p.draft,&QPlainTextEdit::textChanged,this,[this,i]{draftChanged(i);});
    if(p.title)connect(p.title,&QLineEdit::textChanged,this,[this,i]{draftChanged(i);});
    if(p.reference)connect(p.reference,&QLineEdit::textChanged,this,[this,i]{draftChanged(i);});
    if(p.category)connect(p.category,&QComboBox::currentTextChanged,this,[this,i]{draftChanged(i);});
    return compose;
}
SocialView::~SocialView(){if(dirty_&&!loadFailed_&&identity_.valid())store_.save(identity_,drafts_);}
void SocialView::selectSection(social::Section section){const int i=static_cast<int>(section);if(i==0||i==3)tabs_->setCurrentIndex(i);}
void SocialView::loadEditor(int i){
    QScopedValueRollback<bool> guard(loadingDraft_,true);auto& p=pages_[i];const auto& d=drafts_.sections[i];
    const bool reply=i==2&&p.replyMode;const bool room=!reply||drafts_.replies.contains(replyId_)||drafts_.replies.size()<8;
    const bool chatRoom=i!=0||channel_.isEmpty()||drafts_.chats.contains(channel_)||drafts_.chats.size()<16;
    const bool enabled=identity_.valid()&&!loadFailed_&&room&&chatRoom&&(!reply||!replyId_.isEmpty());
    if(p.title){p.titleRow->setVisible(!reply);p.title->setText(d.title);p.title->setEnabled(enabled);}
    if(p.reference){p.reference->setText(d.reference);p.reference->setEnabled(enabled);}
    if(p.category){p.categoryRow->setVisible(!reply);p.category->setCurrentText(d.category);p.category->setEnabled(enabled);}
    const auto source=i==0&&!channel_.isEmpty()?drafts_.chats.value(channel_):reply?drafts_.replies.value(replyId_):d.body;if(i==0)static_cast<SocialComposeEdit*>(p.draft)->setWireSource(source);else p.draft->setPlainText(source);p.draft->setEnabled(enabled);p.save->setEnabled(enabled);
    if(i==0){chatEditor_->setCurrentIndex(0);for(auto* button:findChildren<QPushButton*>())if(button->objectName().startsWith("pgn-social-format-"))button->setEnabled(enabled);}
    p.send->setText(i==0?"Send":i==1?"Publish":reply?"Send reply":"Post topic");p.send->setEnabled(false);
    p.counter->setText(QString("%1 / %2").arg(i==0?static_cast<SocialComposeEdit*>(p.draft)->wireSource().size():p.draft->toPlainText().size()).arg(reply?4000:social::draftLimit(i)));
    p.saved->setText(loadFailed_?"Draft file unavailable":!identity_.valid()?"Select identity and network for drafts":!room?"Eight reply drafts already saved":reply&&replyId_.isEmpty()?"Select a topic before replying":"Local draft · autosave");
    p.saved->setToolTip(safeTip(loadError_));
    if(i==0)updateChatSend();
}
void SocialView::draftChanged(int i){
    if(loadingDraft_||!identity_.valid()||loadFailed_)return;auto& p=pages_[i];const bool reply=i==2&&p.replyMode;
    if(reply&&replyId_.isEmpty())return;const int limit=reply?4000:social::draftLimit(i);
    if(p.draft->toPlainText().size()>limit){QSignalBlocker guard(p.draft);auto value=p.draft->toPlainText().left(limit);if(!value.isEmpty()&&value.back().isHighSurrogate())value.chop(1);p.draft->setPlainText(value);}
    if(i==0&&!static_cast<SocialComposeEdit*>(p.draft)->wireError().isEmpty()){p.saved->setText("Not saved · message limit");updateChatSend();return;}
    if(i==0&&!channel_.isEmpty()){const auto value=static_cast<SocialComposeEdit*>(p.draft)->wireSource();if(value.isEmpty())drafts_.chats.remove(channel_);else if(drafts_.chats.contains(channel_)||drafts_.chats.size()<16)drafts_.chats[channel_]=value;}
    else if(reply){const auto value=p.draft->toPlainText();if(value.isEmpty()){drafts_.replies.remove(replyId_);drafts_.replyTargets.remove(replyId_);}else if(drafts_.replies.contains(replyId_)||drafts_.replies.size()<8){drafts_.replies.insert(replyId_,value);drafts_.replyTargets.insert(replyId_,replyPostId_.isEmpty()?replyId_:replyPostId_);}}
    else{auto& d=drafts_.sections[i];d.body=i==0?static_cast<SocialComposeEdit*>(p.draft)->wireSource():p.draft->toPlainText();if(p.title)d.title=p.title->text();if(p.reference)d.reference=p.reference->text();if(p.category)d.category=p.category->currentText();}
    dirty_=true;p.counter->setText(QString("%1 / %2").arg(i==0?static_cast<SocialComposeEdit*>(p.draft)->wireSource().size():p.draft->toPlainText().size()).arg(limit));p.saved->setText("Saving locally…");autosave_.start();
    if(i==0){if(!p.sending)p.sendNotice.clear();updateChatSend();}
}
bool SocialView::saveDrafts(){
    autosave_.stop();QString error=!identity_.valid()?"Select identity and network before saving.":loadFailed_?loadError_:store_.save(identity_,drafts_);
    for(int i=0;i<3;++i){pages_[i].saved->setText(error.isEmpty()?"Saved locally · "+QDateTime::currentDateTimeUtc().toString("HH:mm"):"Draft not saved");pages_[i].saved->setToolTip(safeTip(error));}
    if(error.isEmpty()){dirty_=false;pending_.remove(store_.path(identity_));}
    else if(dirty_&&identity_.valid()&&(pending_.contains(store_.path(identity_))||pending_.size()<64))pending_.insert(store_.path(identity_),drafts_);
    return error.isEmpty();
}
void SocialView::setLeaderboardCacheScope(QString directory, QString genesis){
    leaderboardCacheDirectory_=std::move(directory);
    leaderboardCacheGenesis_=std::move(genesis);
    restoreLeaderboard();
    updateAccess();
}

bool SocialView::restoreLeaderboard(){
    if(identity_.valid() && identity_.genesis!=leaderboardCacheGenesis_)return false;
    auto& page=pages_[3];
    const auto window=static_cast<social::Window>(page.filter->currentIndex());
    const auto saved=social::leaderboard_cache::load(leaderboardCacheDirectory_,leaderboardCacheGenesis_,window);
    if(!saved)return false;
    page.leaderboardSaved=true;
    present(3,*saved,false);
    return true;
}

void SocialView::setIdentity(social::Identity identity){
    if(identity==identity_)return;if(dirty_)saveDrafts();autosave_.stop();++sendEpoch_;identity_=std::move(identity);drafts_={};dirty_=false;loadError_.clear();loadFailed_=false;replyId_.clear();pages_[0].canSend=pages_[0].sending=false;pages_[0].sendNotice.clear();pages_[0].sendWhy.clear();
    static_cast<SocialChatList*>(pages_[0].list)->resetConversation();channel_.clear();channelData_.clear();channels_->clear();members_->clear();replyPostId_.clear();memberCount_->setText("Participants unavailable");channelTitle_->setText("Conversation");channelDescription_->clear();access_.reset();if(accessDialog_)delete accessDialog_;
    if(identity_.valid()){loadError_=store_.load(identity_,drafts_);loadFailed_=!loadError_.isEmpty();const auto key=store_.path(identity_);if(pending_.contains(key)){drafts_=pending_.value(key);dirty_=true;}else if(pending_.size()>=64){loadFailed_=true;loadError_="Recover pending drafts before editing another identity.";}}
    for(auto& p:pages_){++p.generation;++p.threadGeneration;p.threadBusy=false;if(p.thread)p.thread->clear();p.loaded=p.busy=p.replyMode=false;p.entries.clear();p.miners.clear();p.selectedId.clear();p.nextCursor.clear();p.chatHistoryGeneration.clear();p.observedUtc={};p.leaderboardPeriod.reset();if(p.windowInfo){p.windowInfo->clear();p.windowInfo->setToolTip({});}
        if(p.list){QSignalBlocker block(p.list);p.list->clear();}if(p.detail)p.detail->clear();if(p.ranking)p.ranking->setRowCount(0);if(p.journey)p.journey->setCurrentIndex(0);}
    leaderboardCache_={};
    leaderboardWaiters_={};
    leaderboardWarmAfter_={};
    leaderboardWindows_.clear();
    leaderboardWarmTimer_.stop();
    ++leaderboardIdentityGeneration_;
    leaderboardWarmInFlight_=false;
    leaderboardStartupRetries_=0;
    pages_[3].utcRefreshAttemptMs=0;static_cast<LeaderboardTable*>(pages_[3].ranking)->showLargestGap(false);
    auto& ranking=pages_[3];
    ranking.leaderboardSaved=false;
    for(const auto* id:{"week","month","hour"})if(auto* button=findChild<QPushButton*>("pgn-social-period-"+QString::fromLatin1(id)))button->hide();
    if(!restoreLeaderboard() && ranking.filter->currentIndex()>1){
        QSignalBlocker block(ranking.filter);
        ranking.filter->setCurrentIndex(0);
        restoreLeaderboard();
    }
    const QStringList periodIds{"total","today","week","month","hour"};
    if(auto* button=findChild<QPushButton*>("pgn-social-period-"+periodIds.value(ranking.filter->currentIndex())))button->setChecked(true);
    updateAccess();for(int i=0;i<3;++i)loadEditor(i);read(tabs_->currentIndex());
    if(ranking.leaderboardSaved && tabs_->currentIndex()!=3)read(3);
}
void SocialView::pageState(int i,QString state,QString message,QString detail){
    auto& p=pages_[i];p.state=std::move(state);p.statusText=std::move(message);if(detail.isEmpty())detail=p.statusText;
    if(tabs_->currentIndex()==i){status_->setText(p.statusText);status_->setToolTip(safeTip(detail));status_->setProperty("socialState",p.state);}
    setProperty(QString("pgn_social_state_%1").arg(i).toLatin1().constData(),p.state);
}
void SocialView::updateStatus(){
    const auto& p=pages_[tabs_->currentIndex()];status_->setText(p.statusText);status_->setToolTip(safeTip(p.statusText));status_->setProperty("socialState",p.state);
}
void SocialView::updateAccess(){
    const bool connected=identity_.valid()&&bool(services_.read);
    account_->setText(identity_.valid()?SocialDisplayName(identity_.subject):"Identity not selected");account_->setToolTip(safeTip(identity_.subject));
    network_->setText(identity_.valid()?"Selected PGN network":"Network not selected");network_->setToolTip(safeTip(identity_.genesis));
    for(int i=0;i<4;++i){auto& p=pages_[i];p.refresh->setEnabled(connected&&!p.busy);
        const int count=i==3?p.miners.size():p.entries.size();p.more->setEnabled(connected&&!p.busy&&!p.nextCursor.isEmpty()&&count<LoadedLimit);
        if(i==3 && p.leaderboardSaved)continue;
        p.count->setText(p.loaded?QString("%1 %2 loaded").arg(count).arg(nouns[i]):QString{});
        if(!connected){pageState(i,"offline","Offline · social service not connected");p.empty->setText("Connect an authenticated social service to load "+QString(nouns[i])+".");p.dataSlot->setCurrentIndex(1);}
        else if(!p.loaded){pageState(i,"ready","Ready to load "+QString(nouns[i]));p.empty->setText("No "+QString(nouns[i])+" loaded yet.");p.dataSlot->setCurrentIndex(1);}
    }
    updateStatus();
}
void SocialView::openRead(int i){
    auto& p=pages_[i];const int row=p.list->currentRow();if(row<0||row>=p.entries.size())return;p.selectedId=p.entries[row].id;++p.threadGeneration;p.threadBusy=false;showDetail(i);p.journey->setCurrentIndex(1);if(p.thread)p.thread->setFocus();else p.detail->setFocus();
}
void SocialView::showDetail(int i,bool preserve){
    auto& p=pages_[i];const auto it=std::find_if(p.entries.begin(),p.entries.end(),[&](const auto& item){return item.id==p.selectedId;});
    if(it==p.entries.end()){if(p.detail)p.detail->setPlainText("This item is unavailable in the current snapshot. Return to the list or refresh.");if(p.thread){p.thread->clear();p.threadTitle->setText("Topic unavailable in the current snapshot");p.moreReplies->setEnabled(false);}if(p.reply)p.reply->setEnabled(false);return;}
    if(p.thread){p.threadTitle->setText(it->category+" / "+it->title);p.thread->canDraft=identity_.valid()&&!loadFailed_;p.thread->setTopic(*it,preserve);
        p.threadState->setText(QString("%1 replies loaded").arg(it->replies.size())+(it->replyCount?" · "+*it->replyCount+" total":" · total unavailable"));
        p.moreReplies->setEnabled(!p.threadBusy&&bool(services_.readThread)&&!it->nextReplyCursor.isEmpty()&&it->replies.size()<200);
        p.moreReplies->setToolTip(!it->nextReplyCursor.isEmpty()&&!services_.readThread?"No authenticated thread paging service connected.":it->replies.size()>=200?"Loaded reply limit reached.":QString{});
    }else{const int scroll=preserve?p.detail->verticalScrollBar()->value():0;document(p.detail,*it);p.detail->verticalScrollBar()->setValue(scroll);}if(p.reply)p.reply->setEnabled(true);
}
void SocialView::openComposer(int i,bool reply){
    auto& p=pages_[i];p.replyMode=reply;
    if(reply){const auto it=std::find_if(p.entries.begin(),p.entries.end(),[&](const auto& item){return item.id==p.selectedId;});if(it==p.entries.end())return;replyId_=it->id;replyPostId_=drafts_.replyTargets.value(replyId_,replyId_);p.composeContext->setText("Reply to: "+it->title);p.composeContext->setToolTip(safeTip(it->title+"\nPost: "+replyPostId_));}
    else if(p.composeContext)p.composeContext->setText(i==1?"New publication":"New topic");
    if(i==2)if(auto* button=findChild<QPushButton*>("pgn-social-back-compose-2"))button->setText(reply?"← Topic":"← Topics");
    loadEditor(i);p.journey->setCurrentIndex(2);if(p.draft->isEnabled())p.draft->setFocus();
}
void SocialView::back(int i){
    if(i==0){pages_[0].draft->setFocus();return;}
    if(i<1||i>2)return;auto& p=pages_[i];if(p.journey->currentIndex()==3){p.journey->setCurrentIndex(2);p.draft->setFocus();return;}
    if(p.journey->currentIndex()==2&&i==2&&p.replyMode&&!p.selectedId.isEmpty()){p.journey->setCurrentIndex(1);p.thread->setFocus();}
    else{p.journey->setCurrentIndex(0);p.list->setFocus();}
}
void SocialView::previewPublication(int i){
    auto& p=pages_[i];const auto& d=drafts_.sections[i];social::Entry entry;entry.title=p.replyMode?p.composeContext->text():d.title;entry.body=i==2&&p.replyMode?drafts_.replies.value(replyId_):d.body;entry.reference=d.reference;entry.bbcode=i==2;
    document(p.preview,entry,true);p.journey->setCurrentIndex(3);p.preview->setFocus();
}
social::AccessRequest SocialView::postRequest(QString post,social::AccessAction action,QString reaction)const{
    social::AccessRequest r;r.identity=identity_;r.community=access_?access_->community:QString{};r.revision=access_?access_->revision:QString{};r.sessionReference=access_?access_->session.reference:QString{};r.scope=social::AccessScope::Forum;r.action=action;r.reaction=std::move(reaction);
    const auto& page=pages_[2];for(const auto& topic:page.entries)if(topic.id==page.selectedId){if(post==topic.id)r.target=post;else for(const auto& reply:topic.replies)if(reply.id==post){r.target=post;r.parent=topic.id;}}
    return r;
}
void SocialView::reactToPost(QString post,QString reaction){auto r=postRequest(post,social::AccessAction::ReactContent,reaction);if(!r.target.isEmpty())manageContext(std::move(r));}
void SocialView::replyToPost(QString post,QString quote){
    auto& p=pages_[2];QString author;bool found=false;for(const auto& topic:p.entries)if(topic.id==p.selectedId){if(topic.id==post){author=topic.author;found=true;}else for(const auto& reply:topic.replies)if(reply.id==post){author=reply.author;found=true;}}
    if(!found)return;openComposer(2,true);if(!p.draft->isEnabled())return;replyPostId_=post;p.composeContext->setText("Reply to "+author);p.composeContext->setToolTip(safeTip("Topic: "+p.selectedId+"\nPost: "+post));
    if(drafts_.replies.contains(replyId_)){drafts_.replyTargets.insert(replyId_,post);dirty_=true;autosave_.start();}
    if(!quote.isEmpty()){const QString insertion="[quote]"+quote+"[/quote]\n";if(insertion.size()+p.draft->toPlainText().size()+(p.draft->toPlainText().isEmpty()?0:1)>4000){p.saved->setText("Quote exceeds reply limit · select a shorter excerpt");return;}
        auto cursor=p.draft->textCursor();cursor.movePosition(QTextCursor::End);cursor.insertText((p.draft->toPlainText().isEmpty()?QString{}:"\n")+insertion);p.draft->setTextCursor(cursor);}
    p.draft->setFocus();
}
void SocialView::readThread(){
    auto& p=pages_[2];auto it=std::find_if(p.entries.begin(),p.entries.end(),[&](const auto& e){return e.id==p.selectedId;});
    if(it==p.entries.end()||p.threadBusy||!services_.readThread||it->nextReplyCursor.isEmpty()||it->replies.size()>=200)return;
    const social::ThreadRequest request{identity_,it->id,it->nextReplyCursor,qMin(50,200-int(it->replies.size()))};const auto generation=++p.threadGeneration;p.threadBusy=true;p.moreReplies->setEnabled(false);p.threadState->setText("Loading replies…");const QPointer<SocialView> weak(this);
    services_.readThread(request,[weak,request,generation](social::ThreadSnapshot data,QString error){if(!weak||weak->identity_!=request.identity)return;auto& p=weak->pages_[2];if(!p.threadBusy||p.threadGeneration!=generation||p.selectedId!=request.topic)return;p.threadBusy=false;
        auto topic=std::find_if(p.entries.begin(),p.entries.end(),[&](const auto& e){return e.id==request.topic;});if(topic==p.entries.end())return;
        if(error.isEmpty())error=social::validateThread(data,request);
        if(error.isEmpty()){QSet<QString> ids{topic->id};for(const auto& q:topic->replies)ids.insert(q.id);for(const auto& q:data.replies)if(ids.contains(q.id))error="Reply page overlaps loaded posts.";if(topic->replies.size()+data.replies.size()>200)error="Loaded reply limit exceeded.";}
        if(!error.isEmpty()){p.threadState->setText("Unable to load replies · previous posts retained");p.threadState->setToolTip(safeTip(error));p.moreReplies->setEnabled(true);return;}
        topic->replies+=data.replies;topic->nextReplyCursor=data.nextCursor;weak->showDetail(2,true);
    });
}

void SocialView::renderList(int i){
    auto& p=pages_[i];QString selected;const int row=p.list->currentRow();if(row>=0&&row<p.list->count())selected=p.list->item(row)->data(Qt::UserRole).toString();
    auto* chat=i==0?static_cast<SocialChatList*>(p.list):nullptr;if(chat)chat->beginUpdate();
    const int scroll=p.list->verticalScrollBar()->value();QSignalBlocker block(p.list);p.list->clear();
    int entryIndex=0;for(const auto& e:p.entries){const auto display=SocialDisplayName(e.authorId,e.author);QString value=i==0?display+" · "+stamp(e.createdUtc)+"\n"+e.body:i==1?e.title+"\n"+metadata(e)+"\n"+e.body.left(210):e.title+"\n"+metadata(e);
        auto* item=new QListWidgetItem(value);item->setData(Qt::UserRole,e.id);item->setData(AuthorRole,display);item->setData(PinnedRole,e.pinned);item->setData(TimeRole,i==0?e.createdUtc.toUTC().toString("HH:mm"):stamp(e.createdUtc));item->setData(BodyRole,e.body);item->setData(TitleRole,e.title);item->setData(CategoryRole,e.category);item->setData(AuthorIdRole,e.authorId);item->setData(RepliesRole,e.replyCount.value_or("unavailable"));item->setData(ActivityRole,e.lastActivity.isValid()?e.lastActivity.toUTC().toString("MM-dd HH:mm 'UTC'"):"unavailable");item->setData(StateRole,e.state.isEmpty()?"unavailable":e.state);
        if(i==0){const auto* previous=entryIndex?&p.entries[entryIndex-1]:nullptr;const bool day=!previous||previous->createdUtc.toUTC().date()!=e.createdUtc.toUTC().date();const bool start=e.pinned||day||previous->authorId!=e.authorId||previous->createdUtc.secsTo(e.createdUtc)>300;item->setData(MessageDayRole,day);item->setData(MessageGroupRole,start);item->setData(MessageDateRole,e.createdUtc);item->setToolTip(safeTip(display+"\n"+e.authorId+"\n"+e.createdUtc.toUTC().toString(Qt::ISODate)));}++entryIndex;
        item->setData(ActivityTimeRole,e.lastActivity);item->setData(LastContributorRole,e.lastContributor);QStringList people,peopleInfo;if(e.contributors)for(const auto& person:*e.contributors){people<<person.name;peopleInfo<<person.name+" · "+person.id;}item->setData(ContributorsRole,people);
        if(i==2){item->setText(value+"\nReplies: "+e.replyCount.value_or("unavailable")+" · Activity: "+item->data(ActivityRole).toString()+" · State: "+item->data(StateRole).toString());item->setToolTip(safeTip(item->text()+"\nRecent contributors: "+peopleInfo.join(", ")+"\nLast contributor: "+e.lastContributor));item->setData(Qt::AccessibleDescriptionRole,"Enter reads the topic. Menu or Shift+F10 opens its actions.");}
        p.list->addItem(item);if(e.id==selected)p.list->setCurrentItem(item);}
    if(!chat)p.list->verticalScrollBar()->setValue(scroll);filter(i);if(chat)chat->endUpdate();
}
void SocialView::filter(int i){
    auto& p=pages_[i];const auto query=p.search->text();int visible=0;
    if(i==3){for(int row=0;row<p.ranking->rowCount();++row){bool match=false;for(int col=0;col<4;++col)match|=p.ranking->item(row,col)->text().contains(query,Qt::CaseInsensitive);p.ranking->setRowHidden(row,!match);if(match)++visible;}p.ranking->updateGeometry();p.dataSlot->updateGeometry();}
    else for(int row=0;row<p.entries.size();++row){const auto& e=p.entries[row];const bool match=(i!=2||e.category==forumCategories_->currentItem()->text())&&(e.title+" "+e.author+" "+e.body+" "+e.reference).contains(query,Qt::CaseInsensitive);p.list->item(row)->setHidden(!match);if(match)++visible;}
    const int total=i==3?p.miners.size():p.entries.size();
    p.dataSlot->setCurrentIndex(visible?0:1);
    if(!visible&&total)p.empty->setText(i==2?"No topics match this category or filter.":"No matches in loaded "+QString(nouns[i])+". Change the filter.");
    else if(!visible&&p.loaded)p.empty->setText("No "+QString(nouns[i])+" in this snapshot.");
    p.count->setText(p.loaded?QString("%1 of %2 %3 loaded").arg(visible).arg(total).arg(nouns[i]):QString{});
    if(i==3&&p.loaded)p.count->setText(p.count->text()+" · Sorted by "+p.ranking->horizontalHeaderItem(sortColumn_)->text()+(sortDescending_?" (descending)":" (ascending)"));
    p.more->setEnabled(identity_.valid()&&bool(services_.read)&&!p.busy&&!p.nextCursor.isEmpty()&&total<LoadedLimit);
}
namespace {
constexpr qint64 LeaderboardFreshSeconds = 30;
bool leaderboardPeriodCurrent(const social::Snapshot& snapshot,qint64 now){
    if(snapshot.window==social::Window::AllTime)return true;
    if(!snapshot.leaderboardPeriod||!snapshot.leaderboardPeriod->utcStart)return false;
    const auto start=*snapshot.leaderboardPeriod->utcStart;
    const auto end=social::leaderboardWindowEnd(snapshot.window,start);
    return end&&now>=start&&now<*end;
}
bool leaderboardFresh(const social::Snapshot& snapshot,qint64 now){
    const auto observed=snapshot.observedUtc.toSecsSinceEpoch();
    return snapshot.observedUtc.isValid()&&now>=observed&&now-observed<=LeaderboardFreshSeconds&&
           leaderboardPeriodCurrent(snapshot,now);
}
}

void SocialView::selectLeaderboardWindow(int index,bool restoreSaved){
    if(index<0||static_cast<size_t>(index)>=leaderboardCache_.size())return;
    auto& page=pages_[3];
    leaderboardStartupRetries_=0;
    ++page.generation;
    page.busy=page.loaded=page.leaderboardSaved=false;
    page.nextCursor.clear();
    page.miners.clear();
    page.leaderboardPeriod.reset();
    page.windowInfo->clear();
    page.windowInfo->setToolTip({});
    const auto window=static_cast<social::Window>(index);
    const auto& cached=leaderboardCache_.at(index);
    const auto now=QDateTime::currentSecsSinceEpoch();
    if(cached&&leaderboardPeriodCurrent(*cached,now)){
        present(3,*cached,false);
        if(leaderboardFresh(*cached,now)){
            updateAccess();
            if(!leaderboardWarmTimer_.isActive())leaderboardWarmTimer_.start(150);
            return;
        }
    }else if(!restoreSaved||!restoreLeaderboard()){
        if(window!=social::Window::AllTime)page.windowInfo->setText("UTC interval loading…");
        renderMiners();
        updateAccess();
    }
    updateAccess();
    read(3);
}

void SocialView::fetchLeaderboard(const social::ReadRequest& request,social::Services::Reply reply,bool retryExpired){
    const auto index=static_cast<int>(request.window);
    auto& waiting=leaderboardWaiters_.at(index);
    const bool active=!waiting.isEmpty();
    waiting.append(std::move(reply));
    if(active)return;
    const auto identityGeneration=leaderboardIdentityGeneration_;
    const auto requestGeneration=++leaderboardRequestGeneration_.at(index);
    const QPointer<SocialView> weak(this);
    services_.read(request,[weak,request,index,identityGeneration,requestGeneration,retryExpired](social::Snapshot snapshot,QString error){
        if(!weak||weak->leaderboardIdentityGeneration_!=identityGeneration||weak->identity_!=request.identity||
           weak->leaderboardRequestGeneration_.at(index)!=requestGeneration)return;
        auto replies=std::move(weak->leaderboardWaiters_.at(index));
        weak->leaderboardWaiters_.at(index).clear();
        if(replies.isEmpty())return;
        if(error.isEmpty())error=social::validate(snapshot,request);
        if(error.isEmpty()&&!leaderboardPeriodCurrent(snapshot,QDateTime::currentSecsSinceEpoch())){
            if(retryExpired){
                // Every caller waiting for this first page must receive the same replacement.
                weak->fetchLeaderboard(request,[weak,identityGeneration,replies=std::move(replies)]
                    (social::Snapshot refreshed,QString refreshError) mutable {
                    for(auto& completion:replies){
                        if(!weak||weak->leaderboardIdentityGeneration_!=identityGeneration)return;
                        completion(refreshed,refreshError);
                    }
                },false);
                return;
            }
            error="The leaderboard period changed while loading. Refresh to try again.";
        }
        if(error.isEmpty()){
            auto& cache=weak->leaderboardCache_.at(index);
            cache=snapshot;
            cache->access.reset();
            if(!weak->leaderboardCacheDirectory_.isEmpty() && request.identity.genesis==weak->leaderboardCacheGenesis_){
                QThreadPool::globalInstance()->start([directory=weak->leaderboardCacheDirectory_,
                    genesis=weak->leaderboardCacheGenesis_,saved=*cache]{
                    social::leaderboard_cache::save(directory,genesis,saved);
                });
            }
            if(weak->pages_[3].filter->currentIndex()==index){
                if(!snapshot.leaderboardWindows.isEmpty())weak->leaderboardWindows_=snapshot.leaderboardWindows;
                else if(weak->leaderboardWindows_.isEmpty())
                    weak->leaderboardWindows_={social::Window::AllTime,social::Window::Day};
            }
        }
        for(auto& completion:replies){
            if(!weak||weak->leaderboardIdentityGeneration_!=identityGeneration)return;
            completion(snapshot,error);
        }
        if(weak&&weak->leaderboardIdentityGeneration_==identityGeneration&&
           !weak->leaderboardWarmTimer_.isActive())weak->leaderboardWarmTimer_.start(150);
    });
}

void SocialView::warmLeaderboard(){
    if(!isVisible()||tabs_->currentIndex()!=3||pages_[3].busy||leaderboardWarmInFlight_||
       !services_.read||!identity_.valid())return;
    const auto now=QDateTime::currentSecsSinceEpoch();
    const auto active=static_cast<social::Window>(pages_[3].filter->currentIndex());
    for(const auto window:{social::Window::Day,social::Window::Hour,social::Window::Week,
                          social::Window::Month,social::Window::AllTime}){
        const auto index=static_cast<int>(window);
        const auto& cached=leaderboardCache_.at(index);
        if(window==active||!leaderboardWindows_.contains(window)||
           !leaderboardWaiters_.at(index).isEmpty()||now<leaderboardWarmAfter_.at(index)||
           (cached&&leaderboardFresh(*cached,now)))continue;
        leaderboardWarmAfter_.at(index)=now+60;
        leaderboardWarmInFlight_=true;
        const QPointer<SocialView> weak(this);
        const auto identityGeneration=leaderboardIdentityGeneration_;
        fetchLeaderboard({social::Section::Miners,identity_,window,{},100,{}},
            [weak,identityGeneration](social::Snapshot,QString){
                if(weak&&weak->leaderboardIdentityGeneration_==identityGeneration)
                    weak->leaderboardWarmInFlight_=false;
            });
        return;
    }
}

void SocialView::refresh(){read(tabs_->currentIndex());}
void SocialView::refreshExpiredLeaderboardWindow(qint64 now){
    auto& p=pages_[3];if(!isVisible()||tabs_->currentIndex()!=3||!p.loaded||!p.leaderboardPeriod||!p.leaderboardPeriod->utcStart)return;
    const auto index=p.filter->currentIndex();
    const auto end=social::leaderboardWindowEnd(static_cast<social::Window>(index),*p.leaderboardPeriod->utcStart);
    if(!end||now/1000<*end||now-p.utcRefreshAttemptMs<60000)return;
    p.utcRefreshAttemptMs=now;
    leaderboardCache_.at(index).reset();
    selectLeaderboardWindow(index,false);
}
void SocialView::read(int i,bool more){
    auto& p=pages_[i];const int total=i==3?p.miners.size():p.entries.size();
    if(!identity_.valid()||!services_.read||p.busy||(more&&(p.nextCursor.isEmpty()||total>=LoadedLimit)))return;
    p.busy=true;const auto generation=++p.generation;p.refresh->setEnabled(false);p.more->setEnabled(false);
    if(i==3 && p.leaderboardSaved)pageState(i,"saved","Saved ranking · updating…", "Last verified "+stamp(p.observedUtc));
    else pageState(i,"loading",more?"Loading more "+QString(nouns[i])+"…":i==3&&p.loaded?"Updating ranking…":"Loading "+QString(nouns[i])+"…");
    const social::ReadRequest request{static_cast<social::Section>(i),identity_,i==3?static_cast<social::Window>(p.filter->currentIndex()):social::Window::AllTime,more?p.nextCursor:QString{},more?qMin(100,LoadedLimit-total):100,i==0?channel_:QString{}};
    const QPointer<SocialView> weak(this);
    const auto receive=[weak,i,generation,request,more](social::Snapshot data,QString error){
        if(!weak)return;auto& p=weak->pages_[i];if(p.generation!=generation||!p.busy||weak->identity_!=request.identity)return;
        p.busy=false;p.refresh->setEnabled(true);if(error.isEmpty()&&(i!=3||more))error=social::validate(data,request);
        if(error.isEmpty()&&i==0&&!data.chatHistoryGeneration.isEmpty()){
            if(more&&p.chatHistoryGeneration!=data.chatHistoryGeneration)error="Chat history source changed; refresh before loading older messages.";
            if(p.chatHistoryGeneration==data.chatHistoryGeneration){QMap<QString,const social::Entry*> prior;for(const auto& entry:p.entries)prior.insert(entry.id,&entry);
                for(const auto& entry:data.entries)if(prior.contains(entry.id)){const auto& old=*prior[entry.id];if(old.authorId!=entry.authorId||old.body!=entry.body||old.createdUtc!=entry.createdUtc||old.bbcode!=entry.bbcode){error="Chat history contains a conflicting immutable message ID.";break;}}
            }
        }
         if(error.isEmpty()&&more){QSet<QString> ids;if(i==3){if(p.leaderboardPeriod!=data.leaderboardPeriod)error="Leaderboard anchor or UTC interval changed; refresh the ranking.";for(const auto& item:p.miners)ids.insert(item.id);for(const auto& item:data.miners)if(ids.contains(item.id))error="Page overlaps loaded miner identities.";}
            else{for(const auto& item:p.entries)ids.insert(item.id);for(const auto& item:data.entries)if(ids.contains(item.id))error="Page overlaps loaded records.";}
            if((i==3?p.miners.size()+data.miners.size():p.entries.size()+data.entries.size())>LoadedLimit)error="Loaded data limit reached. Refine the filter or refresh.";}
        if(!error.isEmpty()){
            if(i==0){p.canSend=false;p.sendWhy=error;weak->access_.reset();weak->updateChatSend();}
            if(i==3 && p.leaderboardSaved)weak->pageState(i,"saved","Saved ranking · refresh unavailable", "Last verified "+stamp(p.observedUtc)+". "+error);
            else weak->pageState(i,"error","Unable to load "+QString(nouns[i])+" · "+error.left(130)+(p.loaded?" · previous snapshot retained":" · retry available"),error);
            if(!p.loaded){p.empty->setText("Unable to load "+QString(nouns[i])+". Use Refresh to retry.");p.dataSlot->setCurrentIndex(1);}
            if(i==3&&!more&&(!p.loaded||p.leaderboardSaved)&&weak->leaderboardStartupRetries_<2&&
               (error.startsWith("Core RPC read failed (1):")||error.startsWith("Core RPC read failed (2):")||
                error.startsWith("Core RPC read failed (4):")||error.startsWith("Core RPC read failed (7):"))){
                const auto delay=weak->leaderboardStartupRetries_++?8000:2500;
                if(!p.loaded)p.empty->setText("Waiting for Core. The ranking will retry automatically.");
                QTimer::singleShot(delay,weak,[weak,generation]{
                    if(weak&&weak->isVisible()&&weak->tabs_->currentIndex()==3&&
                       weak->pages_[3].generation==generation)weak->read(3);
                });
            }
            weak->filter(i);return;}
        if(i==3&&!more)weak->leaderboardStartupRetries_=0;
        p.leaderboardSaved=false;
        weak->present(i,data,more);
    };
    if(i==3&&!more)fetchLeaderboard(request,receive);
    else services_.read(request,receive);
}
void SocialView::present(int i,const social::Snapshot& data,bool append){
    if(data.access){const auto error=social::validateAccess(*data.access,identity_);
        if(accessDialog_)if(auto* dialog=dynamic_cast<SocialAccessDialog*>(accessDialog_.data()))dialog->observeAccess(*data.access);
        if(error.isEmpty())access_=data.access;else access_.reset();}
    if(i==0){pages_[0].canSend=data.chatSend&&data.chatSend->allowed;pages_[0].sendWhy=data.chatSend?data.chatSend->reason:QString{};if(!append)chatDirectory(data);updateChatSend();}
    auto& p=pages_[i];const bool continuous=i==0&&!data.chatHistoryGeneration.isEmpty()&&p.chatHistoryGeneration==data.chatHistoryGeneration;
    const auto previousCursor=p.nextCursor;const bool historyChanged=i==0&&p.loaded&&!data.chatHistoryGeneration.isEmpty()&&p.chatHistoryGeneration!=data.chatHistoryGeneration;
    if(historyChanged){static_cast<SocialChatList*>(p.list)->resetConversation();p.entries.clear();}if(i==0)p.chatHistoryGeneration=data.chatHistoryGeneration;
    p.loaded=true;p.observedUtc=data.observedUtc;p.nextCursor=data.nextCursor;
     if(i==3){p.leaderboardPeriod=data.leaderboardPeriod;
         const auto window=static_cast<social::Window>(p.filter->currentIndex());
         const auto supported=data.leaderboardWindows.isEmpty()?QVector<social::Window>{social::Window::AllTime,social::Window::Day}:data.leaderboardWindows;
         const QStringList periodIds{"total","today","week","month","hour"};for(int n=0;n<5;++n)if(auto* button=findChild<QPushButton*>("pgn-social-period-"+periodIds[n]))button->setVisible(supported.contains(static_cast<social::Window>(n)));
         static_cast<LeaderboardTable*>(p.ranking)->showLargestGap(data.largestGapSupported);
         if(data.leaderboardPeriod&&data.leaderboardPeriod->utcStart){const auto start=QDateTime::fromSecsSinceEpoch(*data.leaderboardPeriod->utcStart,Qt::UTC);const auto end=QDateTime::fromSecsSinceEpoch(social::leaderboardWindowEnd(window,*data.leaderboardPeriod->utcStart).value(),Qt::UTC);const auto format=window==social::Window::Hour?QString("yyyy-MM-dd HH:mm"):QString("yyyy-MM-dd");p.windowInfo->setText(start.toString(format)+" → "+end.toString(format)+" · UTC");p.windowInfo->setToolTip("Fixed interval ["+stamp(start)+", "+stamp(end)+"). Canonical block-header time, not physical execution time. Reports and largest gap: disclosure; audits: first PASSED; validated volume: current qualification episode. Anchor: "+data.leaderboardPeriod->anchor);}
         else{p.windowInfo->clear();p.windowInfo->setToolTip({});}
         if(append)p.miners+=data.miners;else p.miners=data.miners;renderMiners();}
    else{if(i==0&&p.loaded&&!append&&!p.entries.isEmpty()){
            QSet<QString> old;for(const auto& entry:p.entries)old.insert(entry.id);const auto& tail=p.entries.last();bool fresh=false,own=false;
            for(const auto& entry:data.entries)if(!old.contains(entry.id)){fresh=true;own|=entry.authorId==identity_.subject&&(entry.createdUtc>tail.createdUtc||(entry.createdUtc==tail.createdUtc&&entry.id>tail.id));}
            if(fresh)static_cast<SocialChatList*>(p.list)->newMessages(own);
        }if(continuous&&!append){
            auto merged=p.entries;QSet<QString> ids;for(const auto& entry:merged)ids.insert(entry.id);for(const auto& entry:data.entries)if(!ids.contains(entry.id))merged.append(entry);
            auto* chat=static_cast<SocialChatList*>(p.list);
            if(merged.size()<=LoadedLimit||chat->followingLatest()){const bool older=!p.entries.isEmpty()&&(data.entries.isEmpty()||p.entries.first().createdUtc<data.entries.first().createdUtc||(p.entries.first().createdUtc==data.entries.first().createdUtc&&p.entries.first().id<data.entries.first().id));p.entries=std::move(merged);if(older)p.nextCursor=previousCursor;}
            else p.nextCursor=previousCursor; // Keep the reader's bounded window; Jump latest requests the current page.
            if(data.participants){QMap<QString,QString> labels;for(const auto& member:*data.participants)labels.insert(member.id,member.name);for(auto& entry:p.entries)entry.author=labels.value(entry.authorId,entry.authorId);}
        }else if(append)p.entries+=data.entries;else p.entries=data.entries;
        std::stable_sort(p.entries.begin(),p.entries.end(),[i](const auto& a,const auto& b){const auto x=i==2&&a.lastActivity.isValid()?a.lastActivity:a.createdUtc,y=i==2&&b.lastActivity.isValid()?b.lastActivity:b.createdUtc;return x==y?a.id<b.id:i==0?x<y:x>y;});
        if(i==0&&p.entries.size()>LoadedLimit){p.entries.remove(0,p.entries.size()-LoadedLimit);p.nextCursor=p.entries.first().id;}
        if(i==0){const QSet<QString> removed(data.removedContent.begin(),data.removedContent.end()),pinned(data.pinnedContent.begin(),data.pinnedContent.end());p.entries.erase(std::remove_if(p.entries.begin(),p.entries.end(),[&](const auto& e){return removed.contains(e.id);}),p.entries.end());for(auto& e:p.entries)e.pinned=pinned.contains(e.id);}
        renderList(i);if((p.detail||p.thread)&&!p.selectedId.isEmpty()){if(i==2&&!append){++p.threadGeneration;p.threadBusy=false;}showDetail(i,true);}}
    p.refresh->setEnabled(identity_.valid()&&bool(services_.read));
    const int total=i==3?p.miners.size():p.entries.size();p.more->setEnabled(!p.nextCursor.isEmpty()&&total<LoadedLimit);
    if(i==0&&data.connection){const auto& state=*data.connection;network_->setText(state.networkLabel);network_->setToolTip(safeTip(identity_.genesis));pageState(i,state.connectedPeers?"loaded":"ready",state.connectedPeers?QString("Connected · %1 peer%2").arg(state.connectedPeers).arg(state.connectedPeers==1?"":"s"):"Waiting for a peer connection");}
    else if(i==3 && p.leaderboardSaved)pageState(i,"saved","Saved ranking · "+stamp(data.observedUtc));
    else pageState(i,total?"loaded":"empty",total?"Updated "+data.observedUtc.toUTC().toString("HH:mm")+" UTC":"Empty snapshot · "+data.observedUtc.toUTC().toString("HH:mm")+" UTC");
}
void SocialView::renderMiners(){
    auto& p=pages_[3];auto values=p.miners;const auto decimal=[](const QString& a,const QString& b){return a.size()!=b.size()?(a.size()<b.size()?-1:1):QString::compare(a,b);};
    const auto counter=[](const social::Miner& m,int col)->const social::Counter&{switch(col){case 1:return m.validatedSearchPositions;case 2:return m.ticketsReported;case 3:return m.ticketsValidated;case 4:return m.validatedAudits;case 11:return m.largestRecordedGap;default:return m.lastActivityHeight;}};
    std::stable_sort(values.begin(),values.end(),[this,&decimal,&counter](const auto& a,const auto& b){int compare=0;
        if(sortColumn_==0)compare=QString::compare(a.name,b.name,Qt::CaseInsensitive);
        else{const auto& x=counter(a,sortColumn_).value;const auto& y=counter(b,sortColumn_).value;if(bool(x)!=bool(y))return bool(x);if(x)compare=decimal(*x,*y);}
        return compare?(sortDescending_?compare>0:compare<0):a.id<b.id;});
    p.ranking->horizontalHeader()->setSortIndicator(sortColumn_,sortDescending_?Qt::DescendingOrder:Qt::AscendingOrder);
    p.ranking->setRowCount(values.size());
    for(int row=0;row<values.size();++row){const auto& m=values[row];
        for(int metric=0;metric<7;++metric){const int col=metric==6?11:metric;const auto raw=col?counter(m,col).value.value_or(QString::fromUtf8("—")):m.name;auto* item=new QTableWidgetItem(col?social::leaderboardNumber(raw):raw);if(!col){item->setData(Qt::UserRole,m.id);item->setData(AuthorIdRole,m.socialMemberId);}if(col)item->setTextAlignment(Qt::AlignRight|Qt::AlignVCenter);item->setData(LeaderboardOwnRole,!m.socialMemberId.isEmpty()&&m.socialMemberId==identity_.subject);item->setData(LeaderboardRankRole,row+1);
            QString info;if(!col)info="Miner identity: "+m.id+"\nGenesis: "+(p.leaderboardSaved?leaderboardCacheGenesis_:identity_.genesis);
            else{const auto& source=counter(m,col).provenance;
                const bool present=bool(counter(m,col).value);
                info=present?"Exact value: "+raw+"\n"+source.source+"\nObserved: "+stamp(source.observedUtc)+"\nGenesis: "+source.genesis:col==11?"Unavailable: "+m.largestGapReason:"Unavailable: no authenticated value supplied. No zero inferred.";}
            item->setToolTip(safeTip(info));item->setData(Qt::AccessibleDescriptionRole,info);p.ranking->setItem(row,col,item);}
        for(int col=6;col<11;++col){const auto& counted=m.rosettes[col-6];const auto raw=counted.value.value_or(QString::fromUtf8("—"));auto* item=new QTableWidgetItem(raw);item->setTextAlignment(Qt::AlignCenter);item->setData(LeaderboardOwnRole,!m.socialMemberId.isEmpty()&&m.socialMemberId==identity_.subject);
          const QString scope="Recorded discoveries from validated reports; not coin payments. Unique disclosed gaps per operator; only disclosed records (V1 best list limited to100/report). Q is independent of G/S/B/R.";
          const QString info=counted.value?("Exact recorded count: "+raw+"\n"+scope+"\n"+counted.provenance.source+"\nGenesis: "+counted.provenance.genesis):("Unavailable: "+m.rosetteReasons[col-6]+"\n"+scope);
          item->setToolTip(safeTip(info));item->setData(Qt::AccessibleDescriptionRole,info);p.ranking->setItem(row,col,item);}}
    filter(3);p.ranking->updateGeometry();p.dataSlot->updateGeometry();
}
void SocialView::formatChat(QString tag){auto* editor=static_cast<SocialComposeEdit*>(pages_[0].draft);if(editor->isEnabled())editor->toggle(tag);}
void SocialView::formatDraft(int i,QString tag){
    if(i==0){formatChat(std::move(tag));return;}
    auto* draft=pages_[i].draft;if(!draft->isEnabled())return;
    auto cursor=draft->textCursor();const auto selected=cursor.selectedText().replace(QChar(0x2029),'\n');
    const QString open=social::bbcode::opening(tag),close="[/"+tag+"]";
    const int limit=i==2&&pages_[2].replyMode?4000:social::draftLimit(i);if(draft->toPlainText().size()+open.size()+close.size()>limit){pages_[i].saved->setText("Formatting exceeds the draft source limit");return;}
    const int start=cursor.selectionStart();cursor.beginEditBlock();cursor.insertText(open+selected+close);cursor.endEditBlock();
    cursor.setPosition(start+open.size());if(!selected.isEmpty())cursor.setPosition(start+open.size()+selected.size(),QTextCursor::KeepAnchor);draft->setTextCursor(cursor);
    draft->setFocus();
}

void SocialView::chatDirectory(const social::Snapshot& data){
    QString selfName;if(data.participants)for(const auto& person:*data.participants)if(person.id==identity_.subject){selfName=person.name;break;}
    account_->setText(SocialDisplayName(identity_.subject,selfName));
    channelTitle_->setProperty("channelId",data.channel);channelTitle_->setText("Conversation");channelDescription_->clear();
    QSignalBlocker guard(channels_);channels_->clear();channelData_=data.channels.value_or(QVector<social::Channel>{});
    for(const auto& channel:channelData_){auto* item=new QListWidgetItem("#  "+channel.name,channels_);item->setData(Qt::UserRole,channel.id);item->setToolTip(safeTip(channel.description));
        if(channel.id==data.channel){channels_->setCurrentItem(item);channelTitle_->setText("# "+channel.name);channelDescription_->setText(channel.description);}}
    channels_->setToolTip(data.channels?"Only channels supplied by the authenticated service.":"Channel directory unavailable.");
    members_->clear();memberCount_->setText(data.participants?QString("Members · %1").arg(data.participants->size()):"Members unavailable");
    if(!data.participants)return;
    auto participants=*data.participants;
    auto group=[this](const social::Participant& person){QString name="Members";int top=-1;if(access_)for(const auto& role:access_->roles)if(person.roleIds.contains(role.id)&&role.position>top){top=role.position;name=role.name;}return QPair<int,QString>{top,name};};
    std::stable_sort(participants.begin(),participants.end(),[&](const auto& a,const auto& b){const auto x=group(a),y=group(b);return x.first!=y.first?x.first>y.first:QString::compare(a.name,b.name,Qt::CaseInsensitive)<0;});
    QString last;for(const auto& person:participants){const auto role=group(person).second;if(role!=last){if(role!="Members"){auto* heading=new QListWidgetItem(role,members_);heading->setFlags(Qt::NoItemFlags);auto font=heading->font();font.setPixelSize(12);font.setWeight(QFont::DemiBold);heading->setFont(font);}last=role;}
        const auto name=SocialDisplayName(person.id,person.name);auto* item=new QListWidgetItem(name+(person.presence.isEmpty()?QString{}:"\n"+person.presence),members_);item->setData(Qt::UserRole,person.id);item->setToolTip(safeTip(name+"\n"+person.id+(person.presence.isEmpty()?QString{}:"\n"+person.presence)));
        QPixmap avatar(32,32);avatar.fill(Qt::transparent);QPainter painter(&avatar);painter.setRenderHint(QPainter::Antialiasing);painter.setPen(Qt::NoPen);painter.setBrush(SocialAvatarColor(person.id));painter.drawEllipse(avatar.rect());painter.setPen(QColor("#193f48"));QFont font("Segoe UI");font.setPixelSize(12);font.setWeight(QFont::DemiBold);painter.setFont(font);painter.drawText(avatar.rect(),Qt::AlignCenter,SocialAvatarInitials(person.id,name));item->setIcon(QIcon(avatar));}
    members_->setIconSize({32,32});
}
void SocialView::updateChatSend(){
    auto& p=pages_[0];if(!p.send)return;const auto source=static_cast<SocialComposeEdit*>(p.draft)->wireSource();const QString channel=channel_.isEmpty()?channelTitle_->property("channelId").toString():channel_;
    const bool connected=bool(services_.sendChat)&&p.canSend&&identity_.valid()&&!channel.isEmpty()&&!loadFailed_;
    const auto error=static_cast<SocialComposeEdit*>(p.draft)->wireError();p.send->setEnabled(connected&&!p.sending&&!source.trimmed().isEmpty()&&error.isEmpty());
    QString reason="Sending unavailable · no authenticated send service connected.";
    if(services_.sendChat){reason=!p.sendWhy.isEmpty()?p.sendWhy:"Sending unavailable · authenticated availability required.";
        if(connected){reason=p.sendNotice.isEmpty()?"Enter to send · Shift+Enter for a new line":p.sendNotice;
            if(p.sendNotice.isEmpty()&&drafts_.chatOperations.contains(channel)&&drafts_.chatOperations[channel].sourceHash==QString::fromLatin1(QCryptographicHash::hash(source.toUtf8(),QCryptographicHash::Sha256).toHex()))reason="Pending send recovered · Send retries the same operation.";}}
    if(p.sending)reason="Publishing signed message…";
    if(!error.isEmpty())reason=error;
    p.reason->setText(reason.left(256));p.reason->setToolTip(safeTip(reason));
}
void SocialView::sendChat(){
    auto& p=pages_[0];if(!p.send->isEnabled()||!services_.sendChat)return;
    const QString source=static_cast<SocialComposeEdit*>(p.draft)->wireSource(),channel=channel_.isEmpty()?channelTitle_->property("channelId").toString():channel_;
    const QString hash=QString::fromLatin1(QCryptographicHash::hash(source.toUtf8(),QCryptographicHash::Sha256).toHex());
    if(!drafts_.chatOperations.contains(channel)&&drafts_.chatOperations.size()>=16){p.sendNotice="Pending Chat operation limit reached.";updateChatSend();return;}
    auto& operation=drafts_.chatOperations[channel];if(operation.sourceHash!=hash||operation.requestId.isEmpty())operation={QUuid::createUuid().toString(QUuid::WithoutBraces),hash};
    dirty_=true;if(!saveDrafts()){p.sendNotice="Send paused · pending operation could not be saved.";updateChatSend();return;}
    const social::SendRequest request{identity_,channel,operation.requestId,source};const auto epoch=sendEpoch_;if(window()->focusWidget()==p.send)p.draft->setFocus(Qt::OtherFocusReason);p.sending=true;p.sendNotice.clear();updateChatSend();
    const QPointer<SocialView> weak(this);const auto completed=std::make_shared<bool>(false);
    services_.sendChat(request,[weak,request,epoch,completed](social::SendResult result){
        if(!weak||*completed||weak->sendEpoch_!=epoch||weak->identity_!=request.identity)return;*completed=true;auto& page=weak->pages_[0];page.sending=false;
        if(result.identity!=request.identity||result.channel!=request.channel||result.requestId!=request.requestId){page.canSend=false;page.sendWhy="Send response scope mismatch · draft retained.";weak->updateChatSend();return;}
        if((result.outcome==social::SendOutcome::Accepted||result.outcome==social::SendOutcome::Posted)&&!result.messageId.isEmpty()&&result.messageId.size()<=256){
            if(weak->drafts_.chatOperations.value(request.channel).requestId==request.requestId)weak->drafts_.chatOperations.remove(request.channel);
            const bool unchanged=static_cast<SocialComposeEdit*>(page.draft)->wireSource()==request.bbcode;if(unchanged)page.draft->clear();weak->dirty_=true;weak->saveDrafts();page.sendNotice=unchanged?(result.reason.isEmpty()?"Sent · confirmation received":result.reason.left(180)):"Previous message published · current draft kept.";
            if(page.send->hasFocus()||page.draft->hasFocus())page.draft->setFocus();static_cast<SocialChatList*>(page.list)->followPublished();++page.generation;page.busy=false;weak->read(0);
        }else{page.sendNotice=result.outcome==social::SendOutcome::Rejected?"Send refused · "+result.reason.left(150):"Publication not confirmed · draft kept. Retry safely. "+result.reason.left(100);
            weak->pageState(0,"error",page.sendNotice);if(result.reason.contains("401")){page.canSend=false;page.sendWhy=page.sendNotice;}}
        weak->updateChatSend();
    });
}
void SocialView::selectChannel(){
    auto* item=channels_->currentItem();if(!item)return;const QString chosen=item->data(Qt::UserRole).toString();if(chosen.isEmpty()||chosen==channel_)return;
    if(dirty_)saveDrafts();++sendEpoch_;static_cast<SocialChatList*>(pages_[0].list)->resetConversation();channel_=chosen;if(accessDialog_)delete accessDialog_;auto& p=pages_[0];p.canSend=p.sending=false;p.sendNotice.clear();++p.generation;p.loaded=p.busy=false;p.entries.clear();p.list->clear();p.nextCursor.clear();members_->clear();memberCount_->setText("Participants loading…");
    p.chatHistoryGeneration.clear();channelTitle_->setText(item->text());loadEditor(0);read(0);
}
void SocialView::openAccess(QString member,QString channel){
    if(accessDialog_){accessDialog_->raise();accessDialog_->activateWindow();return;}
    auto* dialog=new SocialAccessDialog(identity_,access_,services_,channel.isEmpty()?channelTitle_->property("channelId").toString():channel,member,this);dialog->setAttribute(Qt::WA_DeleteOnClose);accessDialog_=dialog;dialog->show();
}
void SocialView::contextMenu(int i,const QPoint& pos,bool reading,QString post){
    auto& p=pages_[i];QString content,id,member,parent;bool replyTarget=false;QWidget* target=p.list?static_cast<QWidget*>(p.list):static_cast<QWidget*>(p.ranking);QPoint point=pos;
    if(i==3){auto* item=p.ranking->itemAt(pos);if(!item)item=p.ranking->currentItem();if(!item)return;const int row=item->row();content=p.ranking->item(row,0)->text();id=p.ranking->item(row,0)->data(Qt::UserRole).toString();member=p.ranking->item(row,0)->data(AuthorIdRole).toString();}
    else if(i==2&&reading){target=p.thread;id=p.selectedId;const auto selected=post.isEmpty()?p.thread->selectedPost():post;for(const auto& entry:p.entries)if(entry.id==id){content=entry.body;member=entry.authorId;if(!selected.isEmpty()&&selected!=id)for(const auto& reply:entry.replies)if(reply.id==selected){parent=id;id=reply.id;content=reply.body;member=reply.authorId;replyTarget=true;}}}
    else{auto* item=reading?p.list->currentItem():p.list->itemAt(pos);if(!item)item=p.list->currentItem();if(!item)return;id=item->data(Qt::UserRole).toString();content=item->data(BodyRole).toString();member=item->data(AuthorIdRole).toString();if(reading)target=p.detail;}
    auto* menu=new QMenu(this);menu->setObjectName("pgn-social-context-"+QString::number(i));menu->setProperty("socialSurface",true);
    auto* copy=menu->addAction(i==3?"Copy miner name":"Copy source text");connect(copy,&QAction::triggered,this,[content]{QApplication::clipboard()->setText(content);});
    auto* copyId=menu->addAction(i==3?"Copy public miner ID":"Copy item ID");connect(copyId,&QAction::triggered,this,[id]{QApplication::clipboard()->setText(id);});
    if(i==3){QStringList keys;for(const auto& miner:p.miners)if(miner.id==id&&miner.publicIdentity){keys=miner.publicIdentity->signingScripts;break;}auto* link=menu->addAction("Link this Native identity to my Social key...");link->setEnabled(bool(services_.publicNameLink)&&!keys.isEmpty());connect(link,&QAction::triggered,this,[this,keys]{if(services_.publicNameLink)services_.publicNameLink(identity_,keys);});}
    if(!member.isEmpty()){menu->addSeparator();auto* memberAction=menu->addAction("Roles & member actions…");connect(memberAction,&QAction::triggered,this,[this,member]{openAccess(member);});}
    QVector<QPair<QPointer<QAction>,std::shared_ptr<social::AccessRequest>>> focusedActions;
    if(i<3){menu->addSeparator();for(const auto action:{social::AccessAction::RemoveContent,social::AccessAction::PinContent}){
        social::AccessRequest r;r.identity=identity_;r.community=access_?access_->community:QString{};r.revision=access_?access_->revision:QString{};r.sessionReference=access_?access_->session.reference:QString{};r.channel=i==0?channelTitle_->property("channelId").toString():QString{};r.action=action;r.target=id;
        r.parent=parent;r.scope=static_cast<social::AccessScope>(i+1);const QString noun=i==0?"message":i==1?"publication":replyTarget?"reply":"topic";auto* entry=menu->addAction((action==social::AccessAction::RemoveContent?"Remove ":"Pin ")+noun);const auto error=social::unavailable(access_?&*access_:nullptr,r,bool(services_.manage));entry->setEnabled(error.isEmpty());entry->setToolTip(safeTip(error));const auto current=std::make_shared<social::AccessRequest>(r);focusedActions.append({entry,current});connect(entry,&QAction::triggered,this,[this,current]{manageContext(*current);});}
        if(!services_.manage){auto* reason=menu->addAction("Moderation unavailable · service not connected");reason->setEnabled(false);}
    }
    if(i==0&&services_.readAccess){const QPointer<SocialView> weak(this);const QPointer<QMenu> alive(menu);const auto identity=identity_;const auto channel=channelTitle_->property("channelId").toString();
        for(const auto& entry:focusedActions){entry.first->setEnabled(false);entry.first->setToolTip("Checking current permissions…");}
        services_.readAccess({identity,channel,{},{},{id}},[weak,alive,identity,focusedActions](social::AccessSnapshot snapshot,QString error){if(!weak||!alive||weak->identity_!=identity)return;if(error.isEmpty())error=social::validateAccess(snapshot,identity);if(error.isEmpty())weak->access_=snapshot;
            for(const auto& entry:focusedActions)if(entry.first){auto& r=*entry.second;if(error.isEmpty()){r.community=snapshot.community;r.revision=snapshot.revision;r.sessionReference=snapshot.session.reference;}const auto denied=error.isEmpty()?social::unavailable(&snapshot,r,bool(weak->services_.manage)):error;entry.first->setEnabled(denied.isEmpty());entry.first->setToolTip(safeTip(denied));}
        });
    }
    menu->setToolTipsVisible(true);connect(menu,&QMenu::aboutToHide,menu,&QObject::deleteLater);
    if(point.x()<0||point.y()<0)point={24,24};menu->popup(target->mapToGlobal(point));
}
void SocialView::manageContext(social::AccessRequest r){
    const auto error=social::unavailable(access_?&*access_:nullptr,r,bool(services_.manage));if(!error.isEmpty()){pageState(tabs_->currentIndex(),"denied","Denied · "+error);return;}
    r.requestId=QUuid::createUuid().toString(QUuid::WithoutBraces);const QPointer<SocialView> weak(this);const int section=tabs_->currentIndex();const auto generation=pages_[section].generation,threadGeneration=pages_[section].threadGeneration;
    const auto completed=std::make_shared<bool>(false);services_.manage(r,[weak,r,section,generation,threadGeneration,completed](social::AccessResult result){if(!weak||*completed||weak->identity_!=r.identity||weak->pages_[section].generation!=generation||(section==2&&weak->pages_[section].threadGeneration!=threadGeneration))return;*completed=true;
        if(!social::matchingResult(result,r)){weak->pageState(section,"denied","Denied · response scope mismatch");return;}
        if(result.outcome==social::AccessOutcome::Accepted){weak->access_.reset();weak->pageState(section,"ready","Accepted by service");weak->refresh();}
        else weak->pageState(section,"denied","Denied · "+result.reason.left(256));});
}

bool SocialView::eventFilter(QObject* object,QEvent* event){
    if(object==pages_[0].draft&&event->type()==QEvent::InputMethod)chatPreedit_=!static_cast<QInputMethodEvent*>(event)->preeditString().isEmpty();
    if(object==pages_[0].draft&&event->type()==QEvent::KeyPress){auto* key=static_cast<QKeyEvent*>(event);if((key->key()==Qt::Key_Return||key->key()==Qt::Key_Enter)&&!chatPreedit_&&!(key->modifiers()&(Qt::ShiftModifier|Qt::ControlModifier|Qt::AltModifier|Qt::MetaModifier))){if(pages_[0].send->isEnabled())sendChat();return true;}}
    if(pages_[2].list&&object==pages_[2].list->viewport()&&event->type()==QEvent::MouseButtonRelease){auto* mouse=static_cast<QMouseEvent*>(event);if(mouse->button()==Qt::LeftButton&&mouse->position().x()>=pages_[2].list->viewport()->width()-34){const auto pos=mouse->position().toPoint();if(auto* item=pages_[2].list->itemAt(pos)){pages_[2].list->setCurrentItem(item);contextMenu(2,pos);return true;}}}
    return QWidget::eventFilter(object,event);
}
}
