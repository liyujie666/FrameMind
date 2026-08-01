#include "view/player/timelinetabwidget.h"

#include "viewmodel/videoanalysisviewmodel.h"
#include "service/themeservice.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>

// ---- 可点击场景卡片 ----
// 继承 QWidget 而非 QFrame，完全自绘，避免 QSS 系统绘制覆盖自定义背景色
class SceneCard : public QWidget {
    Q_OBJECT
public:
    explicit SceneCard(int64_t seekMs, QWidget* parent = nullptr)
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

    void setColors(const QColor& bg, const QColor& highlight,
                   const QColor& border, const QColor& highlightBorder) {
        m_bg              = bg;
        m_highlight       = highlight;
        m_border          = border;
        m_highlightBorder = highlightBorder;
        update();
    }

signals:
    void clicked(int64_t posMs);

protected:
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) emit clicked(m_seekMs);
        QWidget::mousePressEvent(e);
    }

    // 完全自绘：背景填充 + 圆角边框，不调父类 paintEvent，
    // 防止 QSS 全局规则（QWidget { background: #0D1117 }）覆盖自定义颜色
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
        QPainterPath path;
        path.addRoundedRect(r, 8, 8);
        p.fillPath(path, m_highlighted ? m_highlight : m_bg);
        QPen pen(m_highlighted ? m_highlightBorder : m_border);
        pen.setWidthF(1.0);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
    }

private:
    int64_t m_seekMs      = 0;
    bool    m_highlighted = false;
    QColor  m_bg, m_highlight, m_border, m_highlightBorder;
};

#include "view/player/timelinetabwidget.moc"

// ---- TimelineTabWidget ----

TimelineTabWidget::TimelineTabWidget(QWidget* parent)
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
    m_cardLayout = new QVBoxLayout(m_container);
    m_cardLayout->setContentsMargins(0, 0, 0, 0);
    m_cardLayout->setSpacing(8);
    m_cardLayout->addStretch(1);
    m_scroll->setWidget(m_container);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    root->addWidget(m_scroll);

    // 初始空状态提示
    auto* empty = new QLabel(tr("暂无场景数据，请先打开视频"), m_container);
    empty->setAlignment(Qt::AlignCenter);
    empty->setStyleSheet(QStringLiteral(
        "color: #888; font-size: 13px; background: transparent; border: none;"));
    m_cardLayout->insertWidget(0, empty);
    m_descLabels.clear();

    applyScrollStyle();
}

void TimelineTabWidget::setThemeService(ThemeService* theme)
{
    if (m_theme == theme) return;
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, &TimelineTabWidget::onThemeChanged);
    }
}

void TimelineTabWidget::setViewModel(VideoAnalysisViewModel* vm)
{
    if (m_vm == vm) return;
    if (m_vm) disconnect(m_vm, nullptr, this, nullptr);
    m_vm = vm;
    if (m_vm) {
        connect(m_vm, &VideoAnalysisViewModel::scenesReady,
                this, &TimelineTabWidget::onScenesReady);
        connect(m_vm, &VideoAnalysisViewModel::sceneDescribed,
                this, &TimelineTabWidget::onSceneDescribed);
        // 立即刷新（可能已有数据）
        if (!m_vm->scenes().isEmpty()) onScenesReady(m_vm->scenes());
    }
}

void TimelineTabWidget::onPositionChanged(int64_t posMs)
{
    m_currentPosMs = posMs;
    updateHighlight(posMs);
}

void TimelineTabWidget::onScenesReady(const QVector<Scene>& scenes)
{
    m_scenes = scenes;
    if (!scenes.isEmpty()) {
        m_totalDurationMs = scenes.last().endMs;
    }
    buildCards();
    updateHighlight(m_currentPosMs);
}

void TimelineTabWidget::onSceneDescribed(int sceneId, const QString& description)
{
    if (sceneId < 0 || sceneId >= m_descLabels.size()) return;
    QLabel* lbl = m_descLabels[sceneId];
    if (!lbl) return;

    // 从 JSON 里提取 summary 字段，若解析失败则截断原始文本
    QString displayText = description;
    const int sumIdx = description.indexOf(QStringLiteral("\"summary\""));
    if (sumIdx >= 0) {
        const int colon = description.indexOf(QLatin1Char(':'), sumIdx);
        if (colon >= 0) {
            const int q1 = description.indexOf(QLatin1Char('"'), colon + 1);
            const int q2 = description.indexOf(QLatin1Char('"'), q1 + 1);
            if (q1 >= 0 && q2 > q1) {
                displayText = description.mid(q1 + 1, q2 - q1 - 1);
            }
        }
    }
    lbl->setText(displayText.left(100));
    lbl->setVisible(true);
}

void TimelineTabWidget::onThemeChanged()
{
    applyScrollStyle();
    if (!m_scenes.isEmpty()) buildCards();
}

void TimelineTabWidget::applyScrollStyle()
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

// ---- card construction ----

void TimelineTabWidget::clearCards()
{
    m_cards.clear();
    m_descLabels.clear();
    // 删除所有非 stretch 子项
    while (m_cardLayout->count() > 0) {
        QLayoutItem* item = m_cardLayout->takeAt(0);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
}

void TimelineTabWidget::buildCards()
{
    clearCards();

    if (m_scenes.isEmpty()) {
        auto* empty = new QLabel(tr("暂无场景数据"), m_container);
        empty->setAlignment(Qt::AlignCenter);
        empty->setStyleSheet(
            "color: #888; font-size: 13px; background: transparent; border: none;");
        m_cardLayout->addWidget(empty);
        m_cardLayout->addStretch(1);
        return;
    }

    m_cards.resize(m_scenes.size());
    m_descLabels.resize(m_scenes.size());

    for (int i = 0; i < m_scenes.size(); ++i) {
        QWidget* card = makeSceneCard(m_scenes[i], m_totalDurationMs);
        m_cards[i]    = card;
        m_cardLayout->addWidget(card);
    }
    m_cardLayout->addStretch(1);
}

QWidget* TimelineTabWidget::makeSceneCard(const Scene& scene, int64_t totalDurationMs)
{
    const QColor bgColor       = m_theme ? m_theme->color("surfaceVariant") : QColor("#2A2A3A");
    const QColor hlColor       = m_theme ? m_theme->color("primaryContainer") : QColor("#1A2A4A");
    const QColor borderColor   = m_theme ? m_theme->color("border") : QColor("#3A3A4A");
    const QColor hlBorderColor = m_theme ? m_theme->color("primary") : QColor("#2979FF");
    const QColor textColor     = m_theme ? m_theme->color("textPrimary") : QColor("#E0E0E0");
    const QColor subTextColor  = m_theme ? m_theme->color("textSecondary") : QColor("#888");
    const QColor barColor      = m_theme ? m_theme->color("border") : QColor("#3A3A4A");
    const QColor fillColor     = m_theme ? m_theme->color("primary") : QColor("#2979FF");

    auto* card = new SceneCard(scene.startMs, m_container);
    card->setColors(bgColor, hlColor, borderColor, hlBorderColor);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    connect(card, &SceneCard::clicked, this, &TimelineTabWidget::seekRequested);

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);

    // ---- 顶部行：序号 + 时间区间 ----
    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(8);

    auto* indexLabel = new QLabel(tr("场景 %1").arg(scene.id + 1), card);
    indexLabel->setStyleSheet(QString(
        "font-size: 12px; font-weight: 600; color: %1; background: transparent; border: none;")
        .arg(fillColor.name()));

    auto* timeLabel = new QLabel(
        QStringLiteral("%1  →  %2")
            .arg(formatMs(scene.startMs), formatMs(scene.endMs)),
        card);
    timeLabel->setStyleSheet(QString(
        "font-size: 11px; color: %1; background: transparent; border: none;")
        .arg(subTextColor.name()));

    topRow->addWidget(indexLabel);
    topRow->addStretch(1);
    topRow->addWidget(timeLabel);
    layout->addLayout(topRow);

    // ---- 描述文本（初始隐藏，VLM 完成后填入）----
    auto* descLabel = new QLabel(card);
    descLabel->setWordWrap(true);
    descLabel->setVisible(false);
    descLabel->setStyleSheet(QString(
        "font-size: 12px; color: %1; background: transparent; border: none;")
        .arg(textColor.name()));
    layout->addWidget(descLabel);
    m_descLabels[scene.id] = descLabel;

    // ---- 进度条：显示该场景在全片中的位置 ----
    if (totalDurationMs > 0) {
        auto* barOuter = new QWidget(card);
        barOuter->setFixedHeight(4);
        barOuter->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        barOuter->setStyleSheet(QString(
            "background: %1; border-radius: 2px;").arg(barColor.name()));

        const double startPct = double(scene.startMs) / double(totalDurationMs);
        const double endPct   = double(scene.endMs)   / double(totalDurationMs);
        const double widthPct = endPct - startPct;

        auto* barInner = new QWidget(barOuter);
        barInner->setFixedHeight(4);
        barInner->setStyleSheet(QString(
            "background: %1; border-radius: 2px;").arg(fillColor.name()));

        // 用相对布局放置 fill bar
        auto* pbarLayout = new QHBoxLayout(barOuter);
        pbarLayout->setContentsMargins(0, 0, 0, 0);
        pbarLayout->setSpacing(0);
        const int leftPct   = qRound(startPct * 100);
        const int widthPctI = qMax(2, qRound(widthPct * 100));
        const int rightPct  = qMax(0, 100 - leftPct - widthPctI);
        if (leftPct > 0)   pbarLayout->addStretch(leftPct);
        pbarLayout->addWidget(barInner, widthPctI);
        if (rightPct > 0)  pbarLayout->addStretch(rightPct);

        layout->addWidget(barOuter);
    }

    // 如果描述已存在（缓存命中），立即填充
    if (m_vm) {
        const QString existingDesc = m_vm->sceneDescription(scene.id);
        if (!existingDesc.isEmpty()) {
            onSceneDescribed(scene.id, existingDesc);
        }
    }

    return card;
}

void TimelineTabWidget::updateHighlight(int64_t posMs)
{
    for (int i = 0; i < m_scenes.size() && i < m_cards.size(); ++i) {
        auto* card = qobject_cast<SceneCard*>(m_cards[i]);
        if (!card) continue;
        const bool active = m_scenes[i].contains(posMs);
        card->setHighlighted(active);

        // 自动滚动到活跃卡片
        if (active) {
            m_scroll->ensureWidgetVisible(card, 0, 20);
        }
    }
}

// static
QString TimelineTabWidget::formatMs(int64_t ms)
{
    const int totalSec = int(ms / 1000);
    const int h = totalSec / 3600;
    const int m = (totalSec % 3600) / 60;
    const int s = totalSec % 60;
    if (h > 0)
        return QString::asprintf("%02d:%02d:%02d", h, m, s);
    return QString::asprintf("%02d:%02d", m, s);
}
