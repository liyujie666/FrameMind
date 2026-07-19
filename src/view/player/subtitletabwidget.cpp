#include "view/player/subtitletabwidget.h"

#include "viewmodel/videoanalysisviewmodel.h"
#include "service/themeservice.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

// ---- 可点击字幕行 ----
class SubtitleRow : public QWidget {
    Q_OBJECT
public:
    explicit SubtitleRow(int64_t seekMs, QWidget* parent = nullptr)
        : QWidget(parent), m_seekMs(seekMs)
    {
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_StyledBackground, false);
        setAutoFillBackground(false);
    }

    void setHighlighted(bool h) {
        if (m_highlighted == h) return;
        m_highlighted = h;
        update();
    }

    void setHighlightColor(const QColor& c) { m_hlColor = c; update(); }

signals:
    void clicked(int64_t posMs);

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) emit clicked(m_seekMs);
        QWidget::mousePressEvent(e);
    }

    void paintEvent(QPaintEvent*) override {
        if (!m_highlighted) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(QRectF(rect()).adjusted(0.5,0.5,-0.5,-0.5), 6, 6);
        p.fillPath(path, m_hlColor);
    }

private:
    int64_t m_seekMs      = 0;
    bool    m_highlighted = false;
    QColor  m_hlColor     { 0x1A, 0x2A, 0x4A };
};

#include "view/player/subtitletabwidget.moc"

// ---- SubtitleTabWidget ----

SubtitleTabWidget::SubtitleTabWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);

    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_container = new QWidget(m_scroll);
    m_container->setAttribute(Qt::WA_StyledBackground, false);
    m_rowLayout = new QVBoxLayout(m_container);
    m_rowLayout->setContentsMargins(0, 0, 0, 0);
    m_rowLayout->setSpacing(2);

    auto* empty = new QLabel(tr("暂无字幕，请先打开含音频的视频"), m_container);
    empty->setAlignment(Qt::AlignCenter);
    empty->setStyleSheet(
        "color: #888; font-size: 13px; background: transparent; border: none;");
    m_rowLayout->addWidget(empty);
    m_rowLayout->addStretch(1);

    m_scroll->setWidget(m_container);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(m_scroll);

    applyScrollStyle();
}

void SubtitleTabWidget::setThemeService(ThemeService* theme)
{
    if (m_theme == theme) return;
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, &SubtitleTabWidget::onThemeChanged);
    }
}

void SubtitleTabWidget::setViewModel(VideoAnalysisViewModel* vm)
{
    if (m_vm == vm) return;
    if (m_vm) disconnect(m_vm, nullptr, this, nullptr);
    m_vm = vm;
    if (!m_vm) return;

    connect(m_vm, &VideoAnalysisViewModel::speechSegmentsReady,
            this, &SubtitleTabWidget::onSpeechSegmentsReady);

    if (!m_vm->speechSegments().isEmpty())
        onSpeechSegmentsReady(m_vm->speechSegments());
}

void SubtitleTabWidget::onPositionChanged(int64_t posMs)
{
    m_currentPosMs = posMs;
    updateHighlight(posMs);
}

void SubtitleTabWidget::onSpeechSegmentsReady(const QVector<SpeechSegment>& segments)
{
    m_segments = segments;
    buildRows();
    updateHighlight(m_currentPosMs);
}

void SubtitleTabWidget::onThemeChanged()
{
    applyScrollStyle();
    if (!m_segments.isEmpty()) buildRows();
}

void SubtitleTabWidget::applyScrollStyle()
{
    const bool dark    = m_theme ? m_theme->isDark() : true;
    const QColor thumb = m_theme ? m_theme->color("scrollThumb")
                                 : QColor(dark ? "#3A3A4A" : "#C0C0C0");
    const QColor thumbHover = dark ? thumb.lighter(130) : thumb.darker(120);

    m_scroll->setStyleSheet(QString(
        "QScrollArea { background: transparent; border: none; }"
        "QScrollBar:vertical {"
        "  width: 5px; background: transparent; margin: 0; border-radius: 2px;"
        "}"
        "QScrollBar::handle:vertical {"
        "  background: %1; border-radius: 2px; min-height: 24px;"
        "}"
        "QScrollBar::handle:vertical:hover {"
        "  background: %2;"
        "}"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {"
        "  height: 0px;"
        "}"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
        "  background: transparent;"
        "}"
    ).arg(thumb.name(), thumbHover.name()));
}

void SubtitleTabWidget::clearRows()
{
    m_rows.clear();
    m_currentRow = -1;
    while (m_rowLayout->count() > 0) {
        QLayoutItem* item = m_rowLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
}

void SubtitleTabWidget::buildRows()
{
    clearRows();

    if (m_segments.isEmpty()) {
        auto* empty = new QLabel(tr("暂无字幕数据"), m_container);
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(
            "color: #888; font-size: 13px; background: transparent; border: none;");
        m_rowLayout->addWidget(empty);
        m_rowLayout->addStretch(1);
        return;
    }

    m_rows.reserve(m_segments.size());
    for (const SpeechSegment& seg : m_segments) {
        QWidget* row = makeSubtitleRow(seg);
        m_rows.append(row);
        m_rowLayout->addWidget(row);
    }
    m_rowLayout->addStretch(1);
}

QWidget* SubtitleTabWidget::makeSubtitleRow(const SpeechSegment& seg)
{
    const QColor hlColor   = m_theme ? m_theme->color("primaryContainer") : QColor("#1A2A4A");
    const QColor timeColor = m_theme ? m_theme->color("primary")          : QColor("#2979FF");
    const QColor textColor = m_theme ? m_theme->color("textPrimary")      : QColor("#E0E0E0");

    auto* row = new SubtitleRow(seg.startMs, m_container);
    row->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    row->setHighlightColor(hlColor);

    connect(row, &SubtitleRow::clicked, this, &SubtitleTabWidget::seekRequested);

    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(8, 6, 8, 6);
    layout->setSpacing(10);

    // 时间戳
    auto* timeLabel = new QLabel(
        QStringLiteral("%1").arg(formatMs(seg.startMs)), row);
    timeLabel->setFixedWidth(50);
    timeLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    timeLabel->setStyleSheet(QString(
        "font-size: 11px; font-family: monospace; color: %1;"
        "background: transparent; border: none;").arg(timeColor.name()));

    // 分隔线
    auto* sep = new QFrame(row);
    sep->setFrameShape(QFrame::VLine);
    sep->setFixedWidth(1);
    sep->setStyleSheet(QString("background: %1;")
        .arg((m_theme ? m_theme->color("border") : QColor("#3A3A4A")).name()));

    // 转写文本
    auto* textLabel = new QLabel(seg.text, row);
    textLabel->setWordWrap(true);
    textLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    textLabel->setStyleSheet(QString(
        "font-size: 13px; color: %1; background: transparent; border: none;")
        .arg(textColor.name()));

    layout->addWidget(timeLabel);
    layout->addWidget(sep);
    layout->addWidget(textLabel, 1);

    return row;
}

void SubtitleTabWidget::updateHighlight(int64_t posMs)
{
    int newRow = -1;
    for (int i = 0; i < m_segments.size(); ++i) {
        if (posMs >= m_segments[i].startMs && posMs < m_segments[i].endMs) {
            newRow = i;
            break;
        }
    }
    if (newRow == m_currentRow) return;

    // 取消旧高亮
    if (m_currentRow >= 0 && m_currentRow < m_rows.size()) {
        if (auto* r = qobject_cast<SubtitleRow*>(m_rows[m_currentRow]))
            r->setHighlighted(false);
    }
    m_currentRow = newRow;

    // 设置新高亮并自动滚动
    if (m_currentRow >= 0 && m_currentRow < m_rows.size()) {
        if (auto* r = qobject_cast<SubtitleRow*>(m_rows[m_currentRow])) {
            r->setHighlighted(true);
            m_scroll->ensureWidgetVisible(r, 0, 40);
        }
    }
}

// static
QString SubtitleTabWidget::formatMs(int64_t ms)
{
    const int totalSec = int(ms / 1000);
    const int h = totalSec / 3600;
    const int m = (totalSec % 3600) / 60;
    const int s = totalSec % 60;
    if (h > 0)
        return QString::asprintf("%02d:%02d:%02d", h, m, s);
    return QString::asprintf("%02d:%02d", m, s);
}
