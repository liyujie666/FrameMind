#pragma once

#include "view/player/analysisuistyle.h"
#include "viewmodel/videoanalysisviewmodel.h"
#include <QGridLayout>
#include <QTimer>

class AnalysisProgressRing final : public QWidget {
public:
    explicit AnalysisProgressRing(QWidget* parent) : QWidget(parent), m_colors(nullptr) {
        setFixedSize(48, 48); setAutoFillBackground(false);
        setStyleSheet(QStringLiteral("background:transparent; border:none;"));
    }
    void setColors(const AnalysisUi::Colors& colors) { m_colors = colors; update(); }
    void setProgress(int percent, bool interrupted) {
        m_percent = qBound(0, percent, 100); m_interrupted = interrupted;
        setAccessibleName(tr("构建进度 %1%").arg(m_percent)); update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
        const QRectF circle = QRectF(rect()).adjusted(4, 4, -4, -4);
        p.setPen(QPen(AnalysisUi::cardBorder(m_colors), 4)); p.setBrush(Qt::NoBrush); p.drawEllipse(circle);
        const QColor ink = m_interrupted ? m_colors.warning
            : AnalysisUi::blend(m_colors.primary, m_colors.success, m_percent / 100.);
        p.setPen(QPen(ink, 4, Qt::SolidLine, Qt::RoundCap));
        if (m_percent > 0) p.drawArc(circle, 90 * 16, -qRound(360 * 16 * m_percent / 100.));
        auto f = font(); f.setPixelSize(11); f.setWeight(QFont::DemiBold); p.setFont(f);
        p.setPen(m_colors.text); p.drawText(rect(), Qt::AlignCenter, QStringLiteral("%1%").arg(m_percent));
    }
private:
    AnalysisUi::Colors m_colors;
    int m_percent = 0;
    bool m_interrupted = false;
};

// One shared header for every analysis tab. The timer repaints elapsed time;
// the service owns the clock, so tab/video switches never restart a build timer.
class AnalysisBuildProgressWidget final : public QWidget {
public:
    explicit AnalysisBuildProgressWidget(QWidget* parent) : QWidget(parent) {
        setStyleSheet(QStringLiteral("background:transparent; border:none;"));
        m_layout = new QGridLayout(this); m_layout->setContentsMargins(0, 0, 0, 0); m_layout->setHorizontalSpacing(10); m_layout->setVerticalSpacing(2);
        m_ring = new AnalysisProgressRing(this);
        m_stage = new AnalysisUi::PreviewLabel({}, this); m_stage->setMinimumWidth(40);
        m_elapsed = new QLabel(this); m_elapsed->setTextFormat(Qt::PlainText); m_elapsed->setWordWrap(true);
        layoutProgress(true);
        m_timer = new QTimer(this); m_timer->setInterval(1000);
        connect(m_timer, &QTimer::timeout, this, [this] { sync(); }); hide();
    }
    void setThemeService(ThemeService* theme) {
        if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
        m_theme = theme;
        if (m_theme) connect(m_theme, &ThemeService::themeChanged, this, [this] { applyTheme(); });
        applyTheme();
    }
    void setViewModel(VideoAnalysisViewModel* vm) {
        if (m_vm) disconnect(m_vm, nullptr, this, nullptr);
        m_vm = vm;
        if (m_vm) {
            connect(m_vm, &VideoAnalysisViewModel::progressChanged, this, [this] { sync(); });
            connect(m_vm, &VideoAnalysisViewModel::indexingChanged, this, [this] { sync(); });
            connect(m_vm, &VideoAnalysisViewModel::buildStateChanged, this, [this] { sync(); });
            connect(m_vm, &VideoAnalysisViewModel::contentProfileReady, this, [this] { sync(); });
            connect(m_vm, &QObject::destroyed, this, [this] { m_timer->stop(); hide(); });
        }
        sync();
    }
protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (m_layout && (event->size().width() < 300) != m_compact) layoutProgress(event->size().width() < 300);
    }
private:
    void layoutProgress(bool compact) {
        m_compact = compact;
        while (auto* item = m_layout->takeAt(0)) delete item;
        m_layout->addWidget(m_ring, 0, 0, compact ? 2 : 1, 1);
        m_layout->addWidget(m_stage, 0, 1); m_layout->setColumnStretch(1, 1);
        m_layout->addWidget(m_elapsed, compact ? 1 : 0, compact ? 1 : 2);
    }
    void applyTheme() {
        const AnalysisUi::Colors c(m_theme); m_ring->setColors(c);
        m_stage->setStyleSheet(QStringLiteral("color:%1; background:transparent; border:none; font-size:13px; font-weight:400;").arg(c.secondary.name()));
        m_elapsed->setStyleSheet(QStringLiteral("color:%1; background:transparent; border:none; font-size:12px;").arg(c.secondary.name()));
    }
    void sync() {
        if (!m_vm || !m_vm->isIndexing() || m_vm->currentVideoPath().isEmpty()
            || m_vm->buildState().buildId.isEmpty()) {
            m_timer->stop(); hide(); return;
        }
        m_ring->setProgress(m_vm->indexPercent(), false);
        const QString stage = m_vm->indexStageLabel().isEmpty() ? tr("准备构建") : m_vm->indexStageLabel();
        m_stage->setFullText(stage);
        const auto ms = m_vm->buildElapsedMs();
        if (ms < 0) m_elapsed->setText(tr("耗时未记录"));
        else {
            const qint64 seconds = ms / 1000;
            const auto time = QStringLiteral("%1:%2").arg(seconds / 60, 2, 10, QLatin1Char('0')).arg(seconds % 60, 2, 10, QLatin1Char('0'));
            m_elapsed->setText(tr("已用时 %1").arg(time));
        }
        setToolTip(tr("圆环表示构建流程进度，内容完整性以区域状态为准") + "\n" + stage + "\n" + m_elapsed->text());
        if (!m_timer->isActive()) m_timer->start();
        show();
    }
    ThemeService* m_theme = nullptr;
    QPointer<VideoAnalysisViewModel> m_vm;
    AnalysisProgressRing* m_ring = nullptr;
    AnalysisUi::PreviewLabel* m_stage = nullptr;
    QLabel* m_elapsed = nullptr;
    QTimer* m_timer = nullptr;
    QGridLayout* m_layout = nullptr;
    bool m_compact = true;
};
