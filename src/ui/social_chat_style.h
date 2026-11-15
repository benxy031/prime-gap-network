#pragma once
#include <QString>
namespace pgn::ui {
inline QString SocialConversationStyle(){return QStringLiteral(R"QSS(
QWidget#pgn-social-chat-surface QWidget#pgn-social-conversation {background:#041319;}
QWidget#pgn-social-chat-surface QLabel#pgn-social-global-channel {font:600 18px "Segoe UI";color:#e4f2ef;}
QWidget#pgn-social-chat-surface QPushButton#pgn-social-toggle-members,
QWidget#pgn-social-chat-surface QPushButton#pgn-social-refresh-0 {background:#10343d;border:1px solid #365f68;border-radius:8px;padding:4px 10px;min-height:26px;}
QWidget#pgn-social-chat-surface QPushButton#pgn-social-toggle-members:hover,
QWidget#pgn-social-chat-surface QPushButton#pgn-social-refresh-0:hover {background:#1c4851;border-color:#6f979d;}
QWidget#pgn-social-chat-surface QPushButton#pgn-social-toggle-members:checked {background:#244c52;border-color:#638e90;}
QWidget#pgn-social-chat-surface QPushButton#pgn-social-toggle-members:focus,
QWidget#pgn-social-chat-surface QPushButton#pgn-social-refresh-0:focus {border:2px solid #8ab7af;padding:3px 9px;}
QWidget#pgn-social-chat-surface QListWidget#pgn-social-list-0 {background:#e5edef;border:0;border-radius:12px;padding:4px;}
QWidget#pgn-social-chat-surface QWidget#pgn-social-composer-0 {background:#f0f5f3;border:1px solid #a2bebd;border-radius:12px;}
QWidget#pgn-social-composer-0 QWidget {background:transparent;}
QWidget#pgn-social-composer-0 QPlainTextEdit#pgn-social-draft-body-0,
QWidget#pgn-social-composer-0 QTextBrowser#pgn-social-chat-preview {background:#f0f5f3;color:#1d3b42;border:1px solid transparent;border-radius:7px;padding:6px;font:14px "Segoe UI";}
QWidget#pgn-social-composer-0 QPlainTextEdit#pgn-social-draft-body-0:focus {border:1px solid #6b9d94;}
QWidget#pgn-social-composer-0 QPushButton[composeTool="true"],
QWidget#pgn-social-composer-0 QPushButton#pgn-social-preview-chat {background:transparent;color:#365861;border:1px solid transparent;border-radius:6px;padding:3px 5px;min-height:24px;font:12px "Segoe UI";}
QWidget#pgn-social-composer-0 QPushButton[composeTool="true"]:hover,
QWidget#pgn-social-composer-0 QPushButton#pgn-social-preview-chat:hover {background:#dce9e6;border-color:#afc8c2;}
QWidget#pgn-social-composer-0 QPushButton[composeTool="true"]:focus,
QWidget#pgn-social-composer-0 QPushButton#pgn-social-preview-chat:focus {border:2px solid #5c9489;padding:2px 4px;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-preview-chat:checked {background:#ccdfd8;border-color:#7da89b;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-save-0,
QWidget#pgn-social-composer-0 QPushButton#pgn-social-clear-0 {background:transparent;color:#496b72;border:1px solid transparent;border-radius:6px;padding:3px 5px;min-height:24px;font:11px "Segoe UI";}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-save-0:hover,
QWidget#pgn-social-composer-0 QPushButton#pgn-social-clear-0:hover {background:#dce9e6;border-color:#afc8c2;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-save-0:focus,
QWidget#pgn-social-composer-0 QPushButton#pgn-social-clear-0:focus {border:2px solid #5c9489;padding:2px 4px;}
QWidget#pgn-social-composer-0 QLabel {background:transparent;color:#496970;border:0;font:11px "Segoe UI";}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-send-0 {background:#237568;color:#f3ffff;border:1px solid #388d7e;border-radius:8px;min-width:84px;min-height:30px;padding:3px 10px;font:600 13px "Segoe UI";}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-send-0:hover {background:#2e8877;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-send-0:pressed {background:#19584c;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-send-0:focus {border:2px solid #7eafa0;padding:2px 9px;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-send-0:disabled {background:#d8e3e1;border-color:#b5cbc4;color:#627d7b;}
QWidget#pgn-social-chat-surface QListWidget#pgn-social-chat-channels {padding:2px;outline:0;}
QWidget#pgn-social-chat-surface QListWidget#pgn-social-chat-channels::item {padding:7px 9px;margin:2px 0;border-radius:7px;}
QWidget#pgn-social-chat-surface QListWidget#pgn-social-chat-channels::item:selected {background:#244e53;color:#e9f5f1;}
QWidget#pgn-social-chat-surface QListWidget#pgn-social-chat-channels::item:hover {background:#173c44;}
QWidget#pgn-social-chat-surface QListWidget#pgn-social-chat-members::item {padding:0;border:0;min-height:0;}
QWidget#pgn-social-composer-0 QLabel#pgn-social-send-reason-0,
QWidget#pgn-social-composer-0 QLabel#pgn-social-save-status-0,
QWidget#pgn-social-composer-0 QLabel#pgn-social-counter-0 {color:#496970;background:transparent;padding:0;border:0;}
QWidget#pgn-social-composer-0 QPushButton[composeTool="true"]:checked {background:#cfdfd8;border-color:#7eaa9c;}
QWidget#pgn-social-composer-0 QPushButton[composeTool="true"]:pressed {background:#bfd6cc;}
)QSS");}
}
