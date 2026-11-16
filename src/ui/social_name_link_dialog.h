#pragma once
#include "social/social_link_flow.h"
#include "social/social_link_packet_store.h"
#include <QDialog>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QTimer>
namespace pgn::ui {
// A functional consent window using existing V2 only. Neither the displayed
// label nor a local recovered file can grant a name, role or human identity.
class SocialNameLinkDialog final:public QDialog {
 social::NameLinkFlow flow_;social::NameLinkPacketStore store_;
 QLabel *summary_,*state_;QLineEdit* native_;QPlainTextEdit* packet_;
 QCheckBox* consent_;QComboBox *withdrawalSigner_,*action_;QPushButton *prepare_,*nativeSign_,*socialSign_,*submit_,*retry_,*save_,*load_;
 QByteArray saved_;QString event_,storageError_;bool rendering_{};
 void persist(){const auto source=flow_.packet();if(source.isEmpty()||source==saved_)return;storageError_=store_.save(source);if(storageError_.isEmpty())saved_=source;}
 void render(){
  rendering_=true;const auto& p=flow_.prepared();const auto phase=flow_.phase();const bool busy=phase==social::NameLinkPhase::Preparing||phase==social::NameLinkPhase::Signing||phase==social::NameLinkPhase::Submitting;
  action_->setEnabled(!busy);action_->setCurrentIndex(flow_.intent().action=="withdraw"?1:0);
  if(!p.packet.id.isEmpty()&&event_!=p.packet.id){event_=p.packet.id;consent_->setChecked(false);}
  const auto s=flow_.intent().socialScript,k=flow_.intent().nativeScript;
  withdrawalSigner_->setVisible(flow_.intent().action=="withdraw");withdrawalSigner_->setEnabled(!busy&&!flow_.packet().isEmpty()&&phase!=social::NameLinkPhase::Pending&&phase!=social::NameLinkPhase::Included);
  withdrawalSigner_->setCurrentIndex(p.packet.certificate.value("authorizer_script").toString()==k?1:0);
  auto name=p.displayName.isEmpty()?QString("Public Native name is not yet verified."):QString("Native public name: %1").arg(p.displayName);
  summary_->setText((flow_.intent().action=="withdraw"?QString("Withdraw this public name association.\n"):QString{})+name+"\nSocial "+s.left(8)+"…"+s.right(6)+"  /  Native "+k.left(8)+"…"+k.right(6)+"\nDisplay association only. No roles, mining statistics or proof of a common person.");
  summary_->setToolTip("Social S: "+s+"\nNative K: "+k+"\nOperator O: "+p.packet.operatorId+"\nNative anchor: "+p.anchor);
  QString status;
  switch(phase){
   case social::NameLinkPhase::Idle:status="Choose the Native signing key, then prepare the public consent.";break;
   case social::NameLinkPhase::Preparing:status="Checking canonical Native identity and consent…";break;
   case social::NameLinkPhase::NeedsNativeConsent:status="Native consent needed. Sign here if this wallet owns K; otherwise export the packet to its owner.";break;
   case social::NameLinkPhase::ReadyForSocialConsent:status=flow_.intent().action=="withdraw"?"Participant withdrawal consent needed.":"Native consent verified. The Social key owner can now consent.";break;
   case social::NameLinkPhase::NativeConsentReady:status="Both key consents are verified. Apply the display link; capable PGN peers receive it automatically.";break;
   case social::NameLinkPhase::Signing:status="Waiting for this wallet's explicit signature. No automatic unlock.";break;
   case social::NameLinkPhase::Submitting:status="Submitting the exact signed public packet…";break;
   case social::NameLinkPhase::Pending:status="Pending local proof admission. Keep this signed packet; no block or role is required.";break;
   case social::NameLinkPhase::VerifiedLocal:status="Verified for current display locally. Capable PGN peers receive the signed link. No role or historical identity is granted.";break;
   case social::NameLinkPhase::Included:status="Included in canonical Native history. Chat names refresh from the verified association.";break;
   case social::NameLinkPhase::Blocked:status="Consent refused: "+flow_.reason();break;
   case social::NameLinkPhase::Uncertain:status="Outcome uncertain: "+flow_.reason()+" Keep the public packet and check again.";break;
  }
  if(!storageError_.isEmpty())status+="\n"+storageError_;state_->setText(status);
  packet_->setPlainText(QString::fromUtf8(flow_.packet()));native_->setEnabled(!busy&&phase!=social::NameLinkPhase::Pending&&phase!=social::NameLinkPhase::Included);
  prepare_->setEnabled(!busy&&flow_.packet().isEmpty()&&phase!=social::NameLinkPhase::Pending&&phase!=social::NameLinkPhase::Included);load_->setEnabled(!busy);save_->setEnabled(!flow_.packet().isEmpty());
  const bool agreed=consent_->isChecked();consent_->setEnabled(!busy&&!p.packet.source.isEmpty()&&phase!=social::NameLinkPhase::Included&&phase!=social::NameLinkPhase::Pending);
  nativeSign_->setEnabled(agreed&&!busy&&flow_.intent().action=="bind"&&phase==social::NameLinkPhase::NeedsNativeConsent);
  socialSign_->setText(flow_.intent().action=="withdraw"?"Sign withdrawal":"Sign Social consent");
  socialSign_->setEnabled(agreed&&!busy&&phase==social::NameLinkPhase::ReadyForSocialConsent&&(flow_.intent().action=="withdraw"||flow_.intent().session.subject==s));
  submit_->setEnabled(agreed&&!busy&&phase==social::NameLinkPhase::NativeConsentReady);
  retry_->setEnabled(!busy&&!flow_.packet().isEmpty()&&phase!=social::NameLinkPhase::Included);
  rendering_=false;
 }
public:
 SocialNameLinkDialog(social::NameLinkServices services,social::NameLinkIntent intent,QString recoveryFile,QWidget* parent=nullptr):
  QDialog(parent),flow_(std::move(services)),store_(std::move(recoveryFile),intent.session){
  setObjectName("pgn-social-name-link");setProperty("socialSurface",true);setWindowTitle("Link public mining name");resize(720,700);setMinimumSize(640,480);{QFont font("Segoe UI");font.setPixelSize(14);setFont(font);};
  setStyleSheet("#pgn-social-name-link{background:#09262d;color:#e1eeee}#pgn-social-name-link QLabel,#pgn-social-name-link QCheckBox{color:#e1eeee}#pgn-social-name-link QLineEdit,#pgn-social-name-link QPlainTextEdit{background:#eaf0ef;color:#1a333b;border:1px solid #8aa6a9;border-radius:8px;padding:8px}#pgn-social-name-link QPushButton{background:#164550;color:#eef8f6;border:1px solid #3a6a72;border-radius:8px;padding:6px 12px;min-height:24px}#pgn-social-name-link QPushButton:hover{background:#205763}#pgn-social-name-link QPushButton:pressed{background:#103640}#pgn-social-name-link QPushButton:focus{border:2px solid #8ad1bf}#pgn-social-name-link QPushButton:disabled{background:#20383e;color:#94a9ab;border-color:#38535a}#pgn-social-name-link QPushButton[primary=true]{background:#237b68;border-color:#4fa48f}");
  auto* box=new QVBoxLayout(this);box->setContentsMargins(20,20,20,20);box->setSpacing(8);
  auto* title=new QLabel("Link a verified public name");title->setTextFormat(Qt::PlainText);auto font=title->font();font.setPixelSize(20);font.setBold(true);title->setFont(font);box->addWidget(title);
  summary_=new QLabel;summary_->setObjectName("pgn-social-link-summary");summary_->setTextFormat(Qt::PlainText);summary_->setWordWrap(true);box->addWidget(summary_);
  action_=new QComboBox;action_->setObjectName("pgn-social-link-action");action_->setAccessibleName("Public name association action");action_->addItems({"Use a verified Native public name","Withdraw the current name association"});box->addWidget(action_);
  auto* keyLabel=new QLabel("Native signing script K");keyLabel->setTextFormat(Qt::PlainText);box->addWidget(keyLabel);
  native_=new QLineEdit(intent.nativeScript);native_->setObjectName("pgn-social-link-native-key");native_->setMaxLength(44);native_->setPlaceholderText("Public P2WPKH script, 0014…");native_->setAccessibleName("Native public signing script");keyLabel->setBuddy(native_);box->addWidget(native_);
  prepare_=new QPushButton("Prepare consent");prepare_->setObjectName("pgn-social-link-prepare");box->addWidget(prepare_,0,Qt::AlignLeft);
  state_=new QLabel;state_->setObjectName("pgn-social-link-state");state_->setTextFormat(Qt::PlainText);state_->setWordWrap(true);box->addWidget(state_);
  consent_=new QCheckBox("I consent to the exact public keys and display association shown above.");consent_->setObjectName("pgn-social-link-explicit-consent");box->addWidget(consent_);
  withdrawalSigner_=new QComboBox;withdrawalSigner_->setObjectName("pgn-social-link-withdrawal-key");withdrawalSigner_->setAccessibleName("Withdrawal consent key");withdrawalSigner_->addItems({"Withdraw using the Social key S","Withdraw using the Native key K (S is not required)"});box->addWidget(withdrawalSigner_);
  auto* sign=new QHBoxLayout;nativeSign_=new QPushButton("Sign Native consent");nativeSign_->setObjectName("pgn-social-link-sign-native");socialSign_=new QPushButton("Sign Social consent");socialSign_->setObjectName("pgn-social-link-sign-social");submit_=new QPushButton("Use verified name");submit_->setObjectName("pgn-social-link-submit");submit_->setProperty("primary",true);sign->addWidget(nativeSign_);sign->addWidget(socialSign_);sign->addWidget(submit_);box->addLayout(sign);
  auto* packetLabel=new QLabel("Public consent packet · transferable between the two key-owning devices");packetLabel->setWordWrap(true);packetLabel->setTextFormat(Qt::PlainText);box->addWidget(packetLabel);
  packet_=new QPlainTextEdit;packet_->setReadOnly(true);packet_->setObjectName("pgn-social-link-public-packet");packet_->setMaximumHeight(72);packet_->setAccessibleName("Canonical public consent packet");box->addWidget(packet_);
  auto* actions=new QHBoxLayout;load_=new QPushButton("Import packet…");load_->setObjectName("pgn-social-link-import");save_=new QPushButton("Export packet…");save_->setObjectName("pgn-social-link-export");retry_=new QPushButton("Recheck name");retry_->setObjectName("pgn-social-link-check");auto* close=new QPushButton("Close");close->setObjectName("pgn-social-link-close");actions->addWidget(load_);actions->addWidget(save_);actions->addWidget(retry_);actions->addStretch();actions->addWidget(close);box->addLayout(actions);
  connect(action_,&QComboBox::activated,this,[this,currentHead=intent.previousHead](int choice){if(rendering_)return;auto next=flow_.intent();next.action=choice?"withdraw":"bind";next.previousHead=currentHead;consent_->setChecked(false);flow_.setIntent(next);});
  flow_.changed=[this]{persist();render();};flow_.setIntent(intent);
  connect(native_,&QLineEdit::textEdited,this,[this](QString key){if(rendering_)return;auto intent=flow_.intent();intent.nativeScript=key;flow_.setIntent(intent);});
  connect(prepare_,&QPushButton::clicked,this,[this]{flow_.prepare();});connect(consent_,&QCheckBox::toggled,this,[this]{if(!rendering_)render();});
  connect(withdrawalSigner_,&QComboBox::activated,this,[this](int choice){consent_->setChecked(false);flow_.chooseWithdrawalSigner(choice?flow_.intent().nativeScript:flow_.intent().socialScript);});
  connect(nativeSign_,&QPushButton::clicked,this,[this]{flow_.sign(social::NameLinkSide::Native,consent_->isChecked());});
  connect(socialSign_,&QPushButton::clicked,this,[this]{flow_.sign(flow_.intent().action=="withdraw"?social::NameLinkSide::Transition:social::NameLinkSide::Social,consent_->isChecked());});
  connect(submit_,&QPushButton::clicked,this,[this]{flow_.submit(consent_->isChecked());});connect(retry_,&QPushButton::clicked,this,[this]{flow_.refresh();});connect(close,&QPushButton::clicked,this,&QDialog::close);
  connect(load_,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getOpenFileName(this,"Import public consent packet",{},"Public consent (*.json);;All files (*)");if(path.isEmpty())return;QFile file(path);if(!file.open(QIODevice::ReadOnly)||file.size()>24576){state_->setText("Public packet is unreadable or exceeds 24 KiB. Existing packet preserved.");return;}importPublicPacket(file.readAll());});
  connect(save_,&QPushButton::clicked,this,[this]{const auto path=QFileDialog::getSaveFileName(this,"Export public consent packet","social-consent.json","Public consent (*.json)");if(path.isEmpty())return;QSaveFile file(path);file.setDirectWriteFallback(false);const auto source=flow_.packet();if(!file.open(QIODevice::WriteOnly)||file.write(source)!=source.size()||!file.commit())state_->setText("Atomic public-packet export did not commit. Existing file is preserved.");});
  auto* timer=new QTimer(this);timer->setInterval(2000);connect(timer,&QTimer::timeout,this,[this]{if(isVisible()&&flow_.phase()==social::NameLinkPhase::Pending)flow_.refresh();});timer->start();
 }
 social::NameLinkFlow& flow(){return flow_;}
 void importPublicPacket(const QByteArray& source){
  social::NameLinkPacket packet;const auto invalid=social::decodeNameLinkPacket(source,flow_.intent().session.genesis,packet);if(!invalid.isEmpty()){state_->setText(invalid);return;}
  auto intent=flow_.intent();intent.socialScript=packet.socialScript;intent.nativeScript=packet.nativeScript;intent.action=packet.action;intent.previousHead=packet.previous;native_->setText(intent.nativeScript);flow_.setIntent(intent);flow_.importPacket(source);
 }
 void restorePublicPacket(){QByteArray packet;const auto error=store_.load(packet);if(!error.isEmpty())state_->setText(error);else if(!packet.isEmpty()){social::NameLinkPacket p;const auto invalid=social::decodeNameLinkPacket(packet,flow_.intent().session.genesis,p);if(!invalid.isEmpty())state_->setText(invalid);else if(p.id==flow_.intent().previousHead)state_->setText("The saved consent is already the current association. Choose an action to prepare another transition; no signature was requested.");else if(p.previous!=flow_.intent().previousHead)state_->setText("An earlier public packet is preserved. Its head differs from the current Native association; it is not applied automatically.");else importPublicPacket(packet);}}
};
}
