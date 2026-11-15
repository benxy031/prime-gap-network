#pragma once
#include <QAbstractTextDocumentLayout>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QStackedWidget>
#include <QTextBlock>
#include <QTimer>
#include <cmath>
namespace pgn::ui {
// Measure actual laid-out text, not characters or a delay. Work is bounded by
// the six-line visible composer; further text scrolls within the editor.
class SocialComposeFrame final : public QStackedWidget {
    QPlainTextEdit* edit_;
    bool queued_{},fitting_{};
    void requestFit(){
        if(queued_||fitting_)return;
        queued_=true;QTimer::singleShot(0,this,[this]{queued_=false;fit();});
    }
    void fit(){
        fitting_=true;
        const int chrome=qMax(0,edit_->height()-edit_->viewport()->height());
        qreal content=2*edit_->document()->documentMargin();
        for(auto block=edit_->document()->begin();block.isValid()&&content<112;block=block.next())
            content+=edit_->document()->documentLayout()->blockBoundingRect(block).height();
        const int target=qBound(40,int(std::ceil(content))+chrome,128);
        if(edit_->height()!=target)edit_->setFixedHeight(target);
        if(height()!=target)setFixedHeight(target);
        fitting_=false;
    }
protected:
    void resizeEvent(QResizeEvent* event)override{
        QStackedWidget::resizeEvent(event);
        if(event->size().width()!=event->oldSize().width())requestFit();
    }
public:
    explicit SocialComposeFrame(QPlainTextEdit* edit):edit_(edit){
        setObjectName("pgn-social-compose-frame");addWidget(edit_);
        edit_->setFixedHeight(40);setFixedHeight(40);
        connect(edit_,&QPlainTextEdit::textChanged,this,[this]{requestFit();});
        connect(edit_->document()->documentLayout(),&QAbstractTextDocumentLayout::documentSizeChanged,this,[this]{requestFit();});
        requestFit();
    }
};
}
