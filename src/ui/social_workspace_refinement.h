#pragma once
#include <QString>
namespace pgn::ui {
// Native composition/interaction only. No figures, permissions or presence.
inline QString SocialWorkspaceRefinement(){return QStringLiteral(R"QSS(
QWidget#pgn-social-ranking-toolbar {background:#eef4f1;border:1px solid #d6e3dc;border-bottom:0;border-top-left-radius:12px;border-top-right-radius:12px;}
QWidget#pgn-social-ranking-toolbar QLabel {background:transparent;color:#4d6962;border:0;font:12px "Segoe UI";}
QWidget#pgn-social-ranking-toolbar QLabel#pgn-social-ranking-title {font:600 22px "Segoe UI";color:#183e35;}
QWidget#pgn-social-ranking-toolbar QWidget#pgn-social-periods {background:#dfeae4;border:1px solid #b9cec1;border-radius:9px;}
QWidget#pgn-social-ranking-toolbar QWidget#pgn-social-periods QPushButton {background:transparent;color:#365e52;border:1px solid transparent;border-radius:6px;padding:2px 18px;min-height:28px;font:600 12px "Segoe UI";}
QWidget#pgn-social-ranking-toolbar QWidget#pgn-social-periods QPushButton:hover {background:#d0e3d7;}
QWidget#pgn-social-ranking-toolbar QWidget#pgn-social-periods QPushButton:checked {background:#fffefd;color:#204e3d;border-color:#a8c2b1;}
QWidget#pgn-social-ranking-toolbar QWidget#pgn-social-periods QPushButton:focus {border:2px solid #3d8065;padding:1px 17px;}
QWidget#pgn-social-ranking-toolbar QLineEdit {background:#f8faf8;color:#254d42;border:1px solid #b3cbbd;border-radius:8px;padding:7px 10px;min-height:20px;font:13px "Segoe UI";selection-background-color:#bfd9ca;}
QWidget#pgn-social-ranking-toolbar QLineEdit:focus {border:2px solid #49876b;padding:6px 9px;}
QWidget#pgn-social-ranking-toolbar QPushButton#pgn-social-refresh-3 {background:#2d7259;color:#f6fff8;border:1px solid #3c8569;border-radius:8px;padding:5px 14px;min-height:26px;font:600 13px "Segoe UI";}
QWidget#pgn-social-ranking-toolbar QPushButton#pgn-social-refresh-3:hover {background:#378667;}
QWidget#pgn-social-ranking-toolbar QPushButton#pgn-social-refresh-3:pressed {background:#235b48;}
QWidget#pgn-social-ranking-toolbar QPushButton#pgn-social-refresh-3:focus {border:2px solid #6fa98c;padding:4px 13px;}
QWidget#pgn-social-ranking-toolbar QPushButton#pgn-social-refresh-3:disabled {background:#d8e4dd;border-color:#bdcfc4;color:#657d70;}
QWidget#pgn-social-chat-surface QPushButton#pgn-social-toggle-members,
QWidget#pgn-social-chat-surface QPushButton#pgn-social-refresh-0 {font:600 13px "Segoe UI";min-height:28px;padding:4px 12px;}
QWidget#pgn-social-chat-surface QListWidget#pgn-social-list-0 {padding:8px 4px;}
QWidget#pgn-social-composer-0 {background:#f0f6f2;border:1px solid #a7c4b7;border-radius:13px;}
QWidget#pgn-social-composer-0 QPlainTextEdit#pgn-social-draft-body-0 {background:#f0f6f2;color:#203e36;border-radius:8px;padding:6px;min-height:0;font:14px "Segoe UI";selection-background-color:#bfdacd;}
QWidget#pgn-social-composer-0 QPlainTextEdit#pgn-social-draft-body-0:focus {border:1px solid #82aa96;}
QWidget#pgn-social-composer-0 QPushButton[composeTool="true"] {background:#e4eee7;color:#385c4a;border:1px solid transparent;border-radius:7px;padding:0;min-height:28px;}
QWidget#pgn-social-composer-0 QPushButton[composeTool="true"]:hover {background:#d2e4d7;border-color:#b0c9b7;}
QWidget#pgn-social-composer-0 QPushButton[composeTool="true"]:checked {background:#bed9c9;border-color:#76a289;}
QWidget#pgn-social-composer-0 QPushButton[composeTool="true"]:focus {border:2px solid #4e8970;padding:0;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-send-0 {background:#2d765b;border-color:#478c6d;border-radius:9px;min-height:30px;min-width:88px;padding:4px 12px;font:600 14px "Segoe UI";}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-send-0:hover {background:#368b68;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-send-0:pressed {background:#215a46;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-send-0:focus {border:2px solid #72a78b;padding:3px 11px;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-send-0:disabled {background:#d9e5dd;border-color:#b8cfc0;color:#698173;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-save-0,
QWidget#pgn-social-composer-0 QPushButton#pgn-social-clear-0 {background:#e1ebe5;color:#365b49;border:1px solid #a8c2b2;border-radius:7px;padding:3px 8px;min-height:24px;font:600 12px "Segoe UI";}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-save-0:hover,
QWidget#pgn-social-composer-0 QPushButton#pgn-social-clear-0:hover {background:#d1e2d8;border-color:#86aa96;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-save-0:pressed,
QWidget#pgn-social-composer-0 QPushButton#pgn-social-clear-0:pressed {background:#bfd6c9;}
QWidget#pgn-social-composer-0 QPushButton#pgn-social-save-0:focus,
QWidget#pgn-social-composer-0 QPushButton#pgn-social-clear-0:focus {border:2px solid #3d8065;padding:2px 7px;}
)QSS");}
}
