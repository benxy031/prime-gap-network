#include "ui/network_metrics_view.h"
#include "ui/network_metrics_caption.h"
#include "ui/rational_formats.h"
#include "ui/render/graph_border_ticks.h"
#include "ui/render/horizon_graph_controls.h"
#include <QDateTime>
#include <QEvent>
#include <QJsonArray>
#include <QButtonGroup>
#include <QFontMetricsF>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <optional>
#include <algorithm>
#include <cmath>
#include <limits>

namespace pgn::ui {
NetworkMetricsHub::NetworkMetricsHub(Reader reader,QObject* parent):QObject(parent),reader_(std::move(reader)){
    timer_.setInterval(5000);
    connect(&timer_,&QTimer::timeout,this,[this]{
        const bool visible=std::any_of(subscribers_.begin(),subscribers_.end(),[](const auto& item){return item.owner&&item.owner->isVisible();});
        if(visible||snapshot_.isEmpty())refresh();
    });
    timer_.start();
}
void NetworkMetricsHub::subscribe(QWidget* owner,std::function<void(const QJsonObject&,const QString&)> receive){
    subscribers_.push_back({owner,std::move(receive)});
    if(owner)owner->installEventFilter(this);
    if(!snapshot_.isEmpty()||!error_.isEmpty())subscribers_.back().receive(snapshot_,error_);
}
bool NetworkMetricsHub::eventFilter(QObject* watched,QEvent* event){
    if(event->type()==QEvent::Show){
        const QPointer<QWidget> page=qobject_cast<QWidget*>(watched);
        // Show arrives before normal child visibility is settled. One queued
        // shared refresh covers Mining as well as either graph, without a
        // second request for sibling shows or rapid tab switches.
        QTimer::singleShot(0,this,[this,page]{
            if(page&&page->isVisible()&&!busy_&&(!lastDispatch_.isValid()||lastDispatch_.elapsed()>=5000))refresh();
        });
    }
    return QObject::eventFilter(watched,event);
}
void NetworkMetricsHub::setSpan(int seconds){
    if(seconds!=86400&&seconds!=604800)return;
    if(span_==seconds)return;span_=seconds;refresh();
}
void NetworkMetricsHub::observeTip(const QString& genesis,const QString& hash,qint64 height){
    if(genesis==genesis_&&hash==tipHash_&&height==tipHeight_)return;
    const bool changedBranch=(!genesis_.isEmpty()&&genesis!=genesis_)||height<tipHeight_||
        (height==tipHeight_&&hash!=tipHash_)||
        (height==snapshot_.value("height").toInteger(-1)&&QJsonValue(hash)!=snapshot_.value("bestblockhash"));
    genesis_=genesis;tipHash_=hash;tipHeight_=height;
    if(changedBranch){
        ++branchGeneration_;
        snapshot_={};
        error_="Network branch changed · refreshing verified metrics";
        setProperty("pgn_network_metrics_error",error_);
        notifySubscribers();
    }
    refresh();
}
void NetworkMetricsHub::refresh(){
    if(busy_){pending_=true;return;}
    lastDispatch_.start();busy_=true;const auto span=span_;const auto generation=branchGeneration_;
    const QPointer<NetworkMetricsHub> guard(this);
    reader_(span,[guard,span,generation](QJsonObject snapshot,QString error){
        if(!guard)return;guard->busy_=false;
        if(span!=guard->span_||generation!=guard->branchGeneration_){
            guard->pending_=false;guard->refresh();return;
        }
        if(error.isEmpty())guard->snapshot_=std::move(snapshot);
        const bool changed=guard->error_!=error;guard->error_=error;
        if(changed)guard->setProperty("pgn_network_metrics_error",error);
        guard->notifySubscribers();
        if(!guard)return;
        if(guard->pending_){guard->pending_=false;guard->refresh();}
    });
}
void NetworkMetricsHub::notifySubscribers(){
    std::erase_if(subscribers_,[](const auto& item){return item.owner.isNull();});
    const auto subscribers=subscribers_;
    const auto snapshot=snapshot_;
    const auto error=error_;
    for(const auto& item:subscribers)if(item.owner)item.receive(snapshot,error);
}
namespace {
QString networkRate(double value){
    static const char* suffixes[]{"","k","M","G","T","P"};unsigned unit=0;
    while(value>=1000&&unit<5){value/=1000;++unit;}
    return (value!=0?QString("≈ "):QString{})+QString::number(value,'f',value<10?2:value<100?1:0)+suffixes[unit]+" pos/s";
}
class WalletNetworkChart final : public QWidget {
public:
    explicit WalletNetworkChart(QWidget* parent) : QWidget(parent)
    {
        setObjectName("pgn-wallet-network-chart");
        setMinimumHeight(174);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
        setMouseTracking(true);
        setAccessibleName("Network hashrate and difficulty history");
        setToolTip(interactionTip());
    }

    void setSnapshot(const QJsonObject& snapshot)
    {
        const auto values = snapshot.value("points").toArray();
        const auto span = snapshot.value("span_seconds").toInteger();
        if (span != displayedSpan_) {
            displayedSpan_ = span;
            view_.reset();
            dragging_ = false;
            unsetCursor();
        }
        samples_.clear();
        samples_.reserve(values.size());
        const double cadence = std::max(1., double(span) / 255.);
        const double sourceWindow = std::max(1., snapshot.value("window_seconds").toDouble(600.));
        const double tau = std::max(3. * sourceWindow, cadence);
        const double gapSeconds = std::max(3. * cadence, sourceWindow);
        for (const auto& value : values) {
            const auto source = value.toObject();
            Sample sample;
            sample.source = source;
            sample.timeMs = source.value("time").toDouble() * 1000.;
            sample.target = source.value("merit_target_q16").toDouble() / 65536.;
            const auto seconds = source.value("window_seconds").toDouble();
            if (seconds > 0.) {
                sample.rate = source.value("reported_positions").toString().toDouble() / seconds;
                sample.smoothed = sample.rate;
                if (!samples_.empty() && samples_.back().smoothed) {
                    const auto& previous = samples_.back();
                    const double elapsed = (sample.timeMs - previous.timeMs) / 1000.;
                    sample.rateContinues = elapsed > 0. && elapsed <= gapSeconds;
                    if (sample.rateContinues) {
                        // The source values already cover overlapping report windows.
                        // Filter the rates by elapsed chain time without summing their work.
                        const double weight = -std::expm1(-elapsed / tau);
                        sample.smoothed = std::lerp(*previous.smoothed, *sample.rate, weight);
                    }
                }
            }
            samples_.push_back(std::move(sample));
        }
        filterSeconds_ = tau;
        if (view_ && !samples_.empty()) view_ = clampView(*view_);
        clearHover();
        update();
        notifyZoom();
    }

    void setSmoothed(bool value)
    {
        smooth_ = value;
        clearHover();
        update();
    }

    void resetZoom()
    {
        view_.reset();
        dragging_ = false;
        unsetCursor();
        clearHover();
        update();
        notifyZoom();
    }

    std::function<void(bool)> zoomChanged;

protected:
    void paintEvent(QPaintEvent*) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QRectF card = QRectF(rect()).adjusted(1, 1, -1, -1);
        painter.setPen(QColor("#256573"));
        painter.setBrush(QColor("#061b22"));
        painter.drawRoundedRect(card, 8, 8);

        painter.setFont(QFont("Segoe UI", 9, QFont::DemiBold));
        painter.setPen(rateColor());
        painter.drawText(card.adjusted(12, 6, -12, 0), Qt::AlignLeft | Qt::AlignTop,
                         "NETWORK HASHRATE · pos/s");
        painter.setPen(meritColor());
        painter.drawText(card.adjusted(12, 6, -12, 0), Qt::AlignRight | Qt::AlignTop,
                         "DIFFICULTY · merit");

        const QRectF plot = plotRect();
        if (plot.width() < 8. || plot.height() < 8.) return;
        if (samples_.empty()) {
            setProperty("pgn_rate_plot_bounds", QVariantList{});
            setProperty("pgn_merit_plot_bounds", QVariantList{});
            setProperty("pgn_network_chart_geometry", QJsonArray{});
            painter.setPen(QColor("#91acb4"));
            painter.drawText(plot, Qt::AlignCenter, "Waiting for chain observations");
            return;
        }

        const auto time = visibleView();
        const auto axis = verticalAxes(plot.height());
        const auto mapX = [&](double value) { return plot.left() + (value - time.minTimeMs) * plot.width() / (time.maxTimeMs - time.minTimeMs); };
        const auto mapY = [&](double value, const Axis& range) { return plot.bottom() - (value - range.low) * plot.height() / (range.high - range.low); };
        QFont axisFont("Segoe UI");
        axisFont.setPixelSize(11);
        painter.setFont(axisFont);
        const QFontMetricsF metrics(axisFont);

        tics(painter, plot, axis.rate, true, mapY);
        tics(painter, plot, axis.merit, false, mapY);

        const double span = time.maxTimeMs - time.minTimeMs;
        const auto date = [](double value) { return QDateTime::fromMSecsSinceEpoch(qRound64(value)); };
        const bool crossesDay = date(time.minTimeMs).date() != date(time.maxTimeMs).date();
        const QString format = span >= 2. * 86400000. ? "dd MMM" :
            crossesDay ? "dd MMM HH:mm" : span < 3600000. ? "HH:mm:ss" : "HH:mm";
        const double labelWidth = metrics.horizontalAdvance(date(time.maxTimeMs).toString(format)) + 8.;
        // Use the shared GapMiner time steps and let the border filter remove colliding labels.
        const double stepMs = FrontierNiceTimeStepMs(.7 * span / std::max(2., std::floor(plot.width() / (labelWidth + 8.))));
        std::vector<double> roundedTimes;
        if (stepMs >= 86400000.) {
            const auto stepDays = std::max(1, int(std::round(stepMs / 86400000.)));
            auto day = date(time.minTimeMs).date().addDays(1);
            for (auto stamp = day.startOfDay(); stamp.toMSecsSinceEpoch() < time.maxTimeMs; stamp = day.startOfDay()) {
                if (stamp.isValid()) roundedTimes.push_back(stamp.toMSecsSinceEpoch());
                day = day.addDays(stepDays);
            }
        } else {
            const auto offset = date(time.minTimeMs).offsetFromUtc() * 1000.;
            const auto first = std::ceil((time.minTimeMs + offset) / stepMs) * stepMs - offset;
            for (double stamp = first; stamp < time.maxTimeMs; stamp += stepMs) roundedTimes.push_back(stamp);
        }
        const auto ticks = FrontierBorderTicks(time.minTimeMs, time.maxTimeMs, std::move(roundedTimes),
                                               plot.width(), labelWidth);
        QVariantList displayedTicks;
        QString previousLabel;
        for (const auto stamp : ticks) {
            const double x = mapX(stamp);
            painter.setPen(QColor("#16363f"));
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            const auto label = date(stamp).toString(format);
            if (label != previousLabel) {
                const double labelLeft = std::clamp(x - labelWidth / 2., card.left() + 2., card.right() - labelWidth - 2.);
                painter.setPen(QColor("#91b5bd"));
                painter.drawText(QRectF(labelLeft, plot.bottom() + 7., labelWidth, 16.), Qt::AlignHCenter, label);
                displayedTicks.append(stamp);
                previousLabel = label;
            }
        }
        painter.setPen(QColor("#306270"));
        painter.setBrush(Qt::NoBrush);
        painter.drawRect(plot);

        QPainterPath rates, targets;
        std::vector<QPointF> rateDots;
        bool hasPreviousRate = false;
        QPointF previousTarget;
        bool hasPreviousTarget = false;
        for (const auto& sample : samples_) {
            const double x = mapX(sample.timeMs);
            const QPointF target(x, mapY(sample.target, axis.merit));
            if (!hasPreviousTarget) targets.moveTo(target);
            else {
                targets.lineTo(x, previousTarget.y());
                targets.lineTo(target);
            }
            previousTarget = target;
            hasPreviousTarget = true;
            const auto rate = viewRate(sample);
            if (!rate) {
                hasPreviousRate = false;
                continue;
            }
            const QPointF point(x, mapY(*rate, axis.rate));
            if (hasPreviousRate && sample.rateContinues) rates.lineTo(point);
            else {
                rates.moveTo(point);
                rateDots.push_back(point);
            }
            hasPreviousRate = true;
        }

        painter.save();
        painter.setClipRect(plot.adjusted(-1, -1, 1, 1));
        painter.setPen(QPen(meritColor(), 1.65));
        painter.drawPath(targets);
        if (samples_.size() == 1) painter.drawEllipse(previousTarget, 2.5, 2.5);
        painter.setPen(QPen(rateColor(), 1.85));
        painter.drawPath(rates);
        for (const auto& dot : rateDots) painter.drawEllipse(dot, 1.65, 1.65);
        if (hover_) {
            const auto& sample = samples_[*hover_];
            const double x = mapX(sample.timeMs);
            painter.setPen(QPen(QColor("#b0d6dc"), 1, Qt::DashLine));
            painter.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
            painter.setBrush(QColor("#061b22"));
            painter.setPen(QPen(meritColor(), 2));
            painter.drawEllipse(QPointF(x, mapY(sample.target, axis.merit)), 3.5, 3.5);
            if (const auto rate = viewRate(sample)) {
                painter.setPen(QPen(rateColor(), 2));
                painter.drawEllipse(QPointF(x, mapY(*rate, axis.rate)), 3.5, 3.5);
            }
        }
        painter.restore();

        setProperty("pgn_rate_plot_bounds", QVariantList{time.minTimeMs, time.maxTimeMs, axis.rate.low, axis.rate.high});
        setProperty("pgn_merit_plot_bounds", QVariantList{time.minTimeMs, time.maxTimeMs, axis.merit.low, axis.merit.high});
        setProperty("pgn_network_chart_time_ticks", displayedTicks);
        QJsonObject geometry{{"x", plot.x()}, {"y", plot.y()}, {"width", plot.width()}, {"height", plot.height()},
                             {"horizontal_ticks", qint64(displayedTicks.size())}, {"samples", qint64(samples_.size())},
                             {"axis_font_px", 11}, {"left_axis", "Reported network hashrate (positions/s)"},
                             {"right_axis", "Target difficulty (merit)"}};
        setProperty("pgn_network_chart_geometry", QJsonArray{geometry});
    }

    void mousePressEvent(QMouseEvent* event) override
    {
        if ((event->button() == Qt::RightButton || event->button() == Qt::LeftButton)
            && view_ && plotRect().contains(event->position())) {
            dragOrigin_ = event->position();
            dragView_ = visibleView();
            dragButton_ = event->button();
            dragging_ = true;
            setCursor(Qt::ClosedHandCursor);
            clearHover();
            event->accept();
            return;
        }
        QWidget::mousePressEvent(event);
    }

    void mouseMoveEvent(QMouseEvent* event) override
    {
        if (dragging_) {
            const double ratio = (event->position().x() - dragOrigin_.x()) / std::max(1., plotRect().width());
            view_ = clampView(FrontierPanSingleGraphAxis(dragView_, true, ratio));
            update();
            event->accept();
            return;
        }

        const auto plot = plotRect();
        if (samples_.empty() || !plot.contains(event->position())) {
            clearHover();
            return;
        }
        const auto visible = visibleView();
        const double at = visible.minTimeMs + (event->position().x() - plot.left()) / plot.width() *
                          (visible.maxTimeMs - visible.minTimeMs);
        auto next = std::lower_bound(samples_.begin(), samples_.end(), at,
                                     [](const Sample& point, double time) { return point.timeMs < time; });
        if (next == samples_.end() || (next != samples_.begin() && at - (next - 1)->timeMs < next->timeMs - at)) --next;
        if (next->timeMs < visible.minTimeMs || next->timeMs > visible.maxTimeMs) {
            clearHover();
            return;
        }
        const auto index = std::size_t(next - samples_.begin());
        if (hover_ == index) return;
        hover_ = index;
        const auto& source = next->source;
        QString tip = QDateTime::fromMSecsSinceEpoch(qRound64(next->timeMs)).toString("dd MMM yyyy HH:mm:ss");
        tip += QString(" · ledger %1\nTarget merit: %2\nExact target: %3/65536")
            .arg(source.value("height").toInteger())
            .arg(meritTargetText(source.value("merit_target_q16").toInteger()))
            .arg(source.value("merit_target_q16").toInteger());
        if (next->rate) {
            tip += "\nMeasured: " + reportedRateText(source.value("reported_positions").toString(),
                                                       source.value("window_seconds").toInteger());
            if (smooth_) {
                tip += QString("\nSmoothed estimate: %1 · %2 min decay")
                    .arg(networkRate(*next->smoothed)).arg(filterSeconds_ / 60., 0, 'g', 2);
            }
        }
        if (source.value("audited_positions").isString() && source.value("window_seconds").toInteger()>0)
            tip += "\nAudited: " + reportedRateText(source.value("audited_positions").toString(),
                                                       source.value("window_seconds").toInteger());
        auto observation = source;
        observation.insert("sample_index", qint64(index));
        if (const auto rate = viewRate(*next)) observation.insert("visible_rate", *rate);
        setProperty("pgn_network_chart_hover", observation);
        setToolTip(tip);
        update();
    }

    void mouseReleaseEvent(QMouseEvent* event) override
    {
        if (dragging_ && event->button() == dragButton_) {
            dragging_ = false;
            unsetCursor();
            event->accept();
            return;
        }
        QWidget::mouseReleaseEvent(event);
    }

    void wheelEvent(QWheelEvent* event) override
    {
        const auto plot = plotRect();
        if (samples_.size() < 2 || !plot.contains(event->position())) {
            event->ignore();
            return;
        }
        const double steps = event->pixelDelta().isNull() ? event->angleDelta().y() / 120. : event->pixelDelta().y() / 50.;
        if (steps != 0.) {
            const auto full = fullView();
            const auto next = clampView(FrontierZoomGraphView(visibleView(),
                (event->position().x() - plot.left()) / plot.width(), .5, steps, 1000.));
            if (next.maxTimeMs - next.minTimeMs >= full.maxTimeMs - full.minTimeMs) view_.reset();
            else view_ = next;
            clearHover();
            update();
            notifyZoom();
        }
        event->accept();
    }

    void leaveEvent(QEvent* event) override
    {
        clearHover();
        QWidget::leaveEvent(event);
    }

    bool event(QEvent* event) override
    {
        if (event->type() == QEvent::UngrabMouse || event->type() == QEvent::Hide) {
            dragging_ = false;
            unsetCursor();
        }
        return QWidget::event(event);
    }

private:
    struct Sample {
        QJsonObject source;
        double timeMs{}, target{};
        std::optional<double> rate, smoothed;
        bool rateContinues{};
    };

    struct Axis { double low, high, step; };
    struct Axes { Axis rate, merit; };

    static QColor rateColor() { return QColor("#65e6c2"); }
    static QColor meritColor() { return QColor("#ffd166"); }
    static QString interactionTip() { return "Scroll to zoom around the pointer. Drag left or right to pan."; }

    QRectF plotRect() const
    {
        return QRectF(rect()).adjusted(74, 31, -69, -29);
    }

    FrontierGraphView fullView() const
    {
        const double first = samples_.front().timeMs;
        const double last = samples_.back().timeMs;
        return {first == last ? first - 30000. : first, first == last ? last + 30000. : last, 0., 1.};
    }

    FrontierGraphView visibleView() const { return view_ ? *view_ : fullView(); }

    FrontierGraphView clampView(FrontierGraphView view) const
    {
        const auto full = fullView();
        const double span = std::min(view.maxTimeMs - view.minTimeMs, full.maxTimeMs - full.minTimeMs);
        view.minTimeMs = std::clamp(view.minTimeMs, full.minTimeMs, full.maxTimeMs - span);
        view.maxTimeMs = view.minTimeMs + span;
        return view;
    }

    std::optional<double> viewRate(const Sample& sample) const { return smooth_ ? sample.smoothed : sample.rate; }

    Axes verticalAxes(double pixels) const
    {
        // Keep the scale steady throughout a drag. A release commits exactly the frame on screen.
        const int intervals = std::clamp(int(pixels / 23.), 3, 10);
        double rate = 0., low = samples_.front().target, high = low;
        for (const auto& sample : samples_) {
            if (const auto value = viewRate(sample)) rate = std::max(rate, *value);
            low = std::min(low, sample.target);
            high = std::max(high, sample.target);
        }
        const double padding = std::max(.025, (high - low) * .08);
        const double meritStep = FrontierReadableNumericStep((high - low + 2. * padding) / intervals);
        const double rateStep = FrontierNiceNumericStep((rate > 0. ? rate * 1.08 : 1.) / intervals);
        return {{0., std::ceil((rate > 0. ? rate * 1.08 : 1.) / rateStep) * rateStep, rateStep},
                {std::max(0., std::floor((low - padding) / meritStep) * meritStep),
                 std::ceil((high + padding) / meritStep) * meritStep, meritStep}};
    }

    template <typename MapY>
    static void tics(QPainter& painter, const QRectF& plot, const Axis& range, bool left, const MapY& mapY)
    {
        const double labelSpacing = 16.;
        const auto count = qRound((range.high - range.low) / range.step);
        const int precision = std::clamp(int(std::ceil(-std::log10(range.step))), 0, 6);
        const auto color = left ? rateColor() : meritColor();
        const auto first = std::ceil(range.low / range.step - 1.e-8);
        double lastY = plot.bottom() + labelSpacing;
        for (int tick = 0; tick <= count; ++tick) {
            const double value = (first + tick) * range.step;
            const double y = mapY(value, range);
            if (y < plot.top() - .1 || lastY - y < labelSpacing - .1) continue;
            if (left) {
                painter.setPen(QColor("#20424b"));
                painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            }
            const double edge = left ? plot.left() : plot.right();
            painter.setPen(color);
            painter.drawLine(QPointF(edge, y), QPointF(edge + (left ? -4. : 4.), y));
            const auto label = left ? networkRate(value).remove(" pos/s").remove("≈ ") : QString::number(value, 'f', precision);
            const QRectF labelRect(left ? edge - 70. : edge + 8., y - 8., 62., 16.);
            painter.drawText(labelRect, (left ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, label);
            lastY = y;
        }
    }

    void clearHover()
    {
        if (!hover_) return;
        hover_.reset();
        setProperty("pgn_network_chart_hover", QJsonObject{});
        setToolTip(interactionTip());
        update();
    }

    void notifyZoom()
    {
        if (zoomChanged) zoomChanged(view_.has_value());
    }

    std::vector<Sample> samples_;
    std::optional<std::size_t> hover_;
    std::optional<FrontierGraphView> view_;
    FrontierGraphView dragView_;
    QPointF dragOrigin_;
    Qt::MouseButton dragButton_{Qt::NoButton};
    qint64 displayedSpan_{};
    double filterSeconds_{600.};
    bool dragging_{}, smooth_{true};
};

class NetworkMetricsView final:public QWidget {
public:
    explicit NetworkMetricsView(NetworkMetricsHub* hub):hub_(hub){
        setObjectName("pgn-network-metrics-overview");
        setMinimumHeight(220);
        auto* layout=new QVBoxLayout(this);layout->setContentsMargins(0,0,0,0);
        auto* row=new QHBoxLayout;status_=new QLabel("Network metrics · waiting for verified Core data");status_->setWordWrap(true);row->addWidget(status_,1);
        auto* choices=new QButtonGroup(this);choices->setExclusive(true);
        for(const auto& choice:std::vector<std::pair<QString,int>>{{"24 h",86400},{"7 d",604800}}){
            auto* button=new HorizonGraphToggleButton(choice.first);
            button->setCheckable(true);
            button->setChecked(choice.second == hub_->span());
            button->setFixedSize(44, 22);
            choices->addButton(button);button->setObjectName("pgn-network-metrics-span-"+QString::number(choice.second));buttons_.push_back({choice.second,button});
            connect(button,&QPushButton::clicked,this,[this,seconds=choice.second]{
                if (combined_) combined_->resetZoom();
                for (const auto& item : buttons_) item.second->setChecked(item.first == seconds);
                hub_->setSpan(seconds);
            });
            row->addWidget(button);
        }
        combined_ = new WalletNetworkChart(this);
        {
            auto* smooth = new HorizonGraphToggleButton("Smooth");
            smooth->setFixedSize(62, 22);
            smooth->setChecked(true);
            smooth->setObjectName("pgn-wallet-network-chart-smooth");
            smooth->setToolTip("Smooth reported rate in chain time. Uncheck to view measured samples.");
            connect(smooth, &QPushButton::toggled, combined_, [this](bool on) { combined_->setSmoothed(on); });
            row->addWidget(smooth);

            auto* reset = new HorizonGraphResetButton("Reset zoom", 74);
            reset->setObjectName("pgn-wallet-network-chart-reset");
            connect(reset, &QPushButton::clicked, combined_, [this] { combined_->resetZoom(); });
            combined_->zoomChanged = [reset](bool zoomed) { reset->setZoomed(zoomed); };
            row->addWidget(reset);
        }
        layout->addLayout(row);
        layout->addWidget(combined_, 1);
        const QPointer<NetworkMetricsView> guard(this);
        hub_->subscribe(this,[guard](const QJsonObject& value,const QString& error){
            if(!guard)return;
            const bool changed=guard->snapshot_!=value||guard->error_!=error;
            if (guard->snapshot_ != value) guard->combined_->setSnapshot(value);
            guard->snapshot_=value;guard->error_=error;
            for(const auto& choice:guard->buttons_)choice.second->setChecked(choice.first==guard->hub_->span());
            const auto caption=networkMetricsCaption(value,error);
            if(guard->status_->text()!=caption)guard->status_->setText(caption);
            guard->status_->setToolTip(networkMetricsTechnicalTip(value));
            const QString style=error.isEmpty()?"color:#9bdacf":"color:#ffbd69";if(guard->status_->styleSheet()!=style)guard->status_->setStyleSheet(style);
            guard->setProperty("pgn_network_metrics_snapshot",value);guard->setProperty("pgn_network_metrics_error",error);
            if(changed)guard->update();
        });
    }
private:
    WalletNetworkChart* combined_{};
    NetworkMetricsHub* hub_;
    QLabel* status_{};
    QJsonObject snapshot_;
    QString error_;
    std::vector<std::pair<int,QPushButton*>> buttons_;
};
}
QWidget* createNetworkMetricsView(NetworkMetricsHub* hub){return new NetworkMetricsView(hub);}
}
