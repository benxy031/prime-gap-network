#pragma once
#include "social/social_bbcode.h"
#include <QPlainTextEdit>
#include <QShortcut>
#include <QSignalBlocker>
#include <QTextBlock>
#include <QTextDocumentFragment>
#include <QDataStream>
#include <QMimeData>
#include <QKeyEvent>
#include <QInputMethodEvent>
#include <functional>

namespace pgn::ui {
// Native visible character formatting; the existing signed wire/draft format
// remains BBCode. No HTML import, resources, protocol extension or preview.
class SocialComposeEdit:public QPlainTextEdit {
    QString original_;QByteArray originalFingerprint_;
    QByteArray fingerprint()const{QByteArray bytes;QDataStream stream(&bytes,QIODevice::WriteOnly);for(auto b=document()->begin();b.isValid();b=b.next()){stream<<b.text()<<b.blockFormat();for(auto i=b.begin();!i.atEnd();++i){const auto f=i.fragment();if(f.isValid())stream<<f.text()<<f.charFormat();}}return bytes;}
    template<class F>void guarded(F action){const auto before=wireSource();const auto revision=document()->revision();bool changed=false;{QSignalBlocker block(this);auto cursor=textCursor();cursor.beginEditBlock();action();cursor.endEditBlock();const auto error=wireError();if(!error.isEmpty()&&(before.size()<=social::bbcode::MaxSource||wireSource().size()>=before.size())){if(document()->revision()!=revision){if(document()->isUndoAvailable()){undo();document()->clearUndoRedoStacks(QTextDocument::RedoStack);}else setWireSource(before);}setProperty("composeLimit",error);}else setProperty("composeLimit",QString{});changed=document()->revision()!=revision||!error.isEmpty();}if(changed)Q_EMIT textChanged();}
    static QString literal(QString text){
        // Existing opaque code syntax is the only escape in the v1 renderer.
        // Escape each opening bracket separately, including literal [/code].
        // This changes no wire grammar and older clients retain the same text.
        return text.replace('[',"[code][[/code]");
    }
public:
    std::function<void()> formatChanged;
    explicit SocialComposeEdit(QWidget* parent=nullptr):QPlainTextEdit(parent){
        document()->setResourceProvider([](const QUrl&)->QVariant{return {};});
        for(const auto& binding:QList<QPair<QKeySequence,QString>>{{QKeySequence::Bold,"b"},{QKeySequence::Italic,"i"},{QKeySequence::Underline,"u"},{QKeySequence("Ctrl+Shift+X"),"s"}}){auto* shortcut=new QShortcut(binding.first,this);shortcut->setContext(Qt::WidgetShortcut);connect(shortcut,&QShortcut::activated,this,[this,tag=binding.second]{toggle(tag);});}
    }
    void setWireSource(const QString& source){
        const QSignalBlocker block(this);social::bbcode::Document parsed;parsed.setDefaultFont(font());social::bbcode::render(parsed,source);
        clear();setCurrentCharFormat(QTextCharFormat{});QTextCursor cursor(document());cursor.insertFragment(QTextDocumentFragment(&parsed));cursor.setPosition(0);setTextCursor(cursor);original_=source;originalFingerprint_=fingerprint();document()->setModified(false);if(formatChanged)formatChanged();
    }
    QString wireSource()const{
        if(fingerprint()==originalFingerprint_)return original_;
        QString result;bool first=true;
        for(auto block=document()->begin();block.isValid();block=block.next()){
            if(!first)result+='\n';first=false;
            QStringList active;
            for(auto i=block.begin();!i.atEnd();++i){const auto fragment=i.fragment();if(!fragment.isValid())continue;const auto f=fragment.charFormat();QStringList tags;
                if(f.fontWeight()>=QFont::DemiBold)tags<<"b";if(f.fontItalic())tags<<"i";if(f.fontUnderline())tags<<"u";if(f.fontStrikeOut())tags<<"s";
                int common=0;while(common<active.size()&&common<tags.size()&&active[common]==tags[common])++common;
                for(int n=active.size()-1;n>=common;--n)result+="[/"+active[n]+']';for(int n=common;n<tags.size();++n)result+='['+tags[n]+']';active=tags;result+=literal(fragment.text());
                if(result.size()>social::bbcode::MaxSource)return result; // bounded failure, never send truncated source
            }
            for(auto i=active.crbegin();i!=active.crend();++i)result+="[/"+*i+']';
        }
        return result;
    }
    QString wireError()const{
        const auto source=wireSource();const auto utf8=source.toUtf8();if(source.size()>social::bbcode::MaxSource||utf8.toBase64().size()>16000)return "Message is too large after formatting. Shorten it.";
        if(QString::fromUtf8(utf8)!=source)return "Message contains unsupported Unicode.";
        if(fingerprint()==originalFingerprint_)return {};social::bbcode::Parser parser(source);parser.parse();return parser.valid()?QString{}:QString("Message formatting exceeds the supported limit. Simplify it.");
    }
    void toggle(const QString& tag){
        auto cursor=textCursor();const auto old=cursor.charFormat();QTextCharFormat format;
        if(tag=="b")format.setFontWeight(old.fontWeight()>=QFont::DemiBold?QFont::Normal:QFont::Bold);
        else if(tag=="i")format.setFontItalic(!old.fontItalic());else if(tag=="u")format.setFontUnderline(!old.fontUnderline());else if(tag=="s")format.setFontStrikeOut(!old.fontStrikeOut());else return;
        guarded([&]{mergeCurrentCharFormat(format);});setFocus();if(formatChanged)formatChanged();
    }
protected:
    void inputMethodEvent(QInputMethodEvent* event)override{guarded([&]{QPlainTextEdit::inputMethodEvent(event);});}
    void keyPressEvent(QKeyEvent* event)override{if(event->matches(QKeySequence::Undo)||event->matches(QKeySequence::Redo)){QPlainTextEdit::keyPressEvent(event);return;}guarded([&]{QPlainTextEdit::keyPressEvent(event);});}
    bool canInsertFromMimeData(const QMimeData* data)const override{return data&&data->hasText();}
    void insertFromMimeData(const QMimeData* data)override{if(data&&data->hasText())guarded([&]{textCursor().insertText(data->text());});}
};
}
