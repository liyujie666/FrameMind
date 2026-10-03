#include "view/player/timelinetabwidget.h"

#include "viewmodel/videoanalysisviewmodel.h"
#include "service/themeservice.h"
#include "view/player/analysisuistyle.h"
#include "view/player/analysiscombobox.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QEvent>
#include <QTimer>
#include <QComboBox>
#include <QKeyEvent>
#include <QEnterEvent>
#include <QFocusEvent>
#include <algorithm>
#include <functional>
#include <QJsonDocument>

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
        setFocusPolicy(Qt::StrongFocus);
    }

    void setHighlighted(bool h) {
        if (m_highlighted == h) return;
        m_highlighted = h;
        if (m_playingLabel) m_playingLabel->setVisible(h);
        update();
    }

    void setTimelineStyle(bool first, bool last, QLabel* playingLabel) {
        m_timeline = true; m_first = first; m_last = last; m_playingLabel = playingLabel;
        m_playingLabel->hide();
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
    void keyPressEvent(QKeyEvent* e) override {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter || e->key() == Qt::Key_Space) {
            emit clicked(m_seekMs); e->accept(); return;
        }
        QWidget::keyPressEvent(e);
    }
    void enterEvent(QEnterEvent* e) override { m_hovered = true; update(); QWidget::enterEvent(e); }
    void leaveEvent(QEvent* e) override { m_hovered = false; update(); QWidget::leaveEvent(e); }
    void focusInEvent(QFocusEvent* e) override { QWidget::focusInEvent(e); update(); }
    void focusOutEvent(QFocusEvent* e) override { QWidget::focusOutEvent(e); update(); }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton) emit clicked(m_seekMs);
        QWidget::mousePressEvent(e);
    }

    // 完全自绘：背景填充 + 圆角边框，不调父类 paintEvent，
    // 防止 QSS 全局规则（QWidget { background: #0D1117 }）覆盖自定义颜色
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF r = QRectF(rect()).adjusted(m_timeline ? 28.5 : .5, .5, -.5, -.5);
        QPainterPath path;
        path.addRoundedRect(r, m_timeline ? 12 : 8, m_timeline ? 12 : 8);
        p.fillPath(path, m_highlighted ? m_highlight : m_hovered ? AnalysisUi::blend(m_bg, m_highlightBorder, .04) : m_bg);
        QPen pen(hasFocus() ? m_highlightBorder : m_highlighted && m_timeline
            ? AnalysisUi::blend(m_border, m_highlightBorder, .22) : m_highlighted ? m_highlightBorder : m_border);
        pen.setWidthF(1.0);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        if (m_timeline) {
            p.setPen(QPen(m_border, 1.5));
            p.drawLine(QPointF(10, m_first ? 30 : 0), QPointF(10, m_last ? 30 : height()));
            p.setPen(Qt::NoPen);
            p.setBrush(m_highlighted ? m_highlightBorder : AnalysisUi::blend(m_border, m_highlightBorder, .18));
            p.drawEllipse(QPointF(10, 30), 4, 4);
            if (m_highlighted) p.fillRect(QRectF(28, 20, 3, qMax(0, height() - 40)), m_highlightBorder);
        }
    }

private:
    int64_t m_seekMs      = 0;
    bool    m_highlighted = false;
    bool m_hovered = false, m_timeline = false, m_first = false, m_last = false;
    QLabel* m_playingLabel = nullptr;
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

    connect(m_scroll->verticalScrollBar(), &QScrollBar::sliderPressed,
            this, [this]() { m_userScrolling = true; });
    connect(m_scroll->verticalScrollBar(), &QScrollBar::sliderReleased,
            this, [this]() {
                if (!m_scrollResetTimer) {
                    m_scrollResetTimer = new QTimer(this);
                    m_scrollResetTimer->setSingleShot(true);
                    connect(m_scrollResetTimer, &QTimer::timeout, this,
                            [this]() { m_userScrolling = false; });
                }
                m_scrollResetTimer->start(3000);
            });
    m_scroll->viewport()->installEventFilter(this);
    m_scroll->verticalScrollBar()->installEventFilter(this);

    m_container = new QWidget(m_scroll);
    m_container->setAttribute(Qt::WA_StyledBackground, false);
    m_cardLayout = new QVBoxLayout(m_container);
    m_cardLayout->setContentsMargins(0, 14, 0, 0);
    m_cardLayout->setSpacing(12);
    m_cardLayout->addStretch(1);
    m_scroll->setWidget(m_container);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 8, 16, 12);
    root->setSpacing(8);
    auto* toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(0, 0, 0, 0);
    m_mode=new AnalysisUi::ComboBox(this, true);m_mode->addItems({tr("内容章节"),tr("镜头")});
    m_mode->setObjectName("timelineMode"); m_mode->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_mode->setAccessibleName(tr("时间线显示方式"));
    toolbar->addWidget(m_mode); toolbar->addStretch();
    m_timelineMeta = new QLabel(this); m_timelineMeta->setTextFormat(Qt::PlainText);
    toolbar->addWidget(m_timelineMeta); root->addLayout(toolbar);
    connect(m_mode,&QComboBox::currentIndexChanged,this,[this]{refreshTimeline();});
    m_feedback = new QLabel(this);
    m_feedback->setTextFormat(Qt::PlainText); m_feedback->setWordWrap(true); m_feedback->hide();
    root->addWidget(m_feedback);
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
    onThemeChanged();
}

void TimelineTabWidget::setViewModel(VideoAnalysisViewModel* vm)
{
    if (m_vm == vm) return;
    if (m_vm) disconnect(m_vm, nullptr, this, nullptr);
    m_vm = vm;
    showInteractionMessage({});
    m_shots.clear(); m_chapters.clear(); m_videoId.clear(); m_buildId.clear();
    m_currentPosMs = 0; m_totalDurationMs = 0; m_userScrolling = false;
    if (m_scrollResetTimer) m_scrollResetTimer->stop();
    if (m_vm) {
        connect(m_vm, &VideoAnalysisViewModel::presentationReady, this,
            [this](const QString& video, const QString& build, const VideoPresentation& p) {
                if (!m_vm || video != m_vm->displayedVideoId() || build != m_vm->displayedBuildId()) return;
                const bool changedVideo = video != m_videoId;
                showInteractionMessage({});
                m_videoId = video; m_buildId = build; m_chapters = p.chapters;
                if (video.isEmpty()) { m_shots.clear(); m_totalDurationMs = 0; }
                if (changedVideo) {
                    m_currentPosMs = 0; m_userScrolling = false;
                    if (m_scrollResetTimer) m_scrollResetTimer->stop();
                    m_scroll->verticalScrollBar()->setValue(0);
                }
                refreshTimeline();
            });
        connect(m_vm, &VideoAnalysisViewModel::scenesReady, this, &TimelineTabWidget::onScenesReady);
        connect(m_vm, &VideoAnalysisViewModel::sceneDescribed, this, &TimelineTabWidget::onSceneDescribed);
        connect(m_vm, &VideoAnalysisViewModel::sceneFused, this, &TimelineTabWidget::onSceneFused);
        connect(m_vm, &VideoAnalysisViewModel::indexingChanged, this, [this] { refreshTimeline(); });
        m_videoId = m_vm->displayedVideoId(); m_buildId = m_vm->displayedBuildId();
        m_shots = m_vm->scenes(); m_chapters = m_vm->chapters();
    }
    refreshTimeline();
}

void TimelineTabWidget::showInteractionMessage(const QString& text) {
    m_feedback->setText(text); m_feedback->setVisible(!text.isEmpty());
}

void TimelineTabWidget::onPositionChanged(int64_t posMs)
{
    m_currentPosMs = posMs;
    updateHighlight(posMs);
}

void TimelineTabWidget::onScenesReady(const QVector<Scene>& scenes)
{
    if (m_vm && m_vm->displayedVideoId() != m_videoId) {
        m_currentPosMs = 0; m_userScrolling = false;
        m_scroll->verticalScrollBar()->setValue(0);
    }
    m_shots = scenes;
    if (m_vm) { m_chapters = m_vm->chapters(); m_videoId = m_vm->displayedVideoId(); m_buildId = m_vm->displayedBuildId(); }
    if (!scenes.isEmpty()) {
        m_totalDurationMs = scenes.last().endMs;
    }
    refreshTimeline();
}

void TimelineTabWidget::refreshTimeline() {
    const int scrollPosition = m_scroll->verticalScrollBar()->value();
    m_scenes = m_shots;
    m_totalDurationMs = 0;
    if (!m_shots.isEmpty()) m_totalDurationMs = m_shots.last().endMs;
    if (!m_chapters.isEmpty()) m_totalDurationMs = qMax(m_totalDurationMs, int64_t(m_chapters.last().endMs));
    buildCards();
    m_cardLayout->activate();
    updateHighlight(m_currentPosMs, false);
    m_scroll->verticalScrollBar()->setValue(scrollPosition);
}

void TimelineTabWidget::onSceneDescribed(int sceneId, const QString& description)
{
    if (m_mode->currentIndex() == 0) return;
    if (!m_descLabels.contains(sceneId)) return;
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
    lbl->setText(displayText);
    lbl->setVisible(true);
}

void TimelineTabWidget::onSceneFused(int sceneId, const SceneFusion& fusion)
{
    if (m_mode->currentIndex() == 0) return;
    if (!m_descLabels.contains(sceneId)) return;
    QLabel* label = m_descLabels[sceneId];
    if (!label) return;

    QString displayText;
    if (fusion.relation == AudioVisualRelation::Independent
        && !fusion.audioSummary.isEmpty()) {
        // 音画无关时在 UI 明确分述，避免用户把同期音频误认成画面事实
        displayText = tr("视觉：%1\n同期音频：%2\n[%3 · %4]")
                          .arg(fusion.visualDescription,
                               fusion.audioSummary,
                               SceneFusion::relationLabel(fusion.relation),
                               QString::number(fusion.confidence, 'f', 2));
    } else {
        displayText = fusion.fusedDescription;
        if (fusion.hasAudio()) {
            displayText += tr("\n[%1 · %2]")
                               .arg(SceneFusion::relationLabel(fusion.relation),
                                    QString::number(fusion.confidence, 'f', 2));
        }
    }
    label->setText(displayText);
    label->setVisible(true);
}

void TimelineTabWidget::onThemeChanged()
{
    ++m_themeRevision;
    applyScrollStyle();
    refreshTimeline();
}

void TimelineTabWidget::applyScrollStyle()
{
    const AnalysisUi::Colors c(m_theme);
    static_cast<AnalysisUi::ComboBox*>(m_mode)->setThemeColors(c);
    m_timelineMeta->setStyleSheet(QStringLiteral("color:%1; font-size:12px; background:transparent; border:none;").arg(c.secondary.name()));
    m_feedback->setStyleSheet(QStringLiteral("color:%1; font-size:12px; background:transparent; border:none;").arg(c.secondary.name()));
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
    m_cardRanges.clear();
    // 删除所有非 stretch 子项
    while (m_cardLayout->count() > 0) {
        QLayoutItem* item = m_cardLayout->takeAt(0);
        if (item->widget()) { item->widget()->hide(); item->widget()->setEnabled(false); item->widget()->deleteLater(); }
        delete item;
    }
}

void TimelineTabWidget::buildCards()
{
    const int position = m_scroll->verticalScrollBar()->value();
    QPointer<QWidget> anchor;
    QString anchorKey;
    int anchorOffset = 0;
    QHash<QString, QWidget*> previous;
    for (auto* card : m_cards) {
        previous.insert(card->property("timelineKey").toString(), card);
        if (card->y() <= position) {
            anchor = card; anchorOffset = position - card->y(); anchorKey = card->property("timelineKey").toString();
        }
    }
    while (auto* item = m_cardLayout->takeAt(0)) {
        if (auto* widget = item->widget()) if (!m_cards.contains(widget)) { widget->hide(); widget->deleteLater(); }
        delete item;
    }
    m_cards.clear(); m_cardRanges.clear(); m_descLabels.clear();
    const bool chapters = m_mode->currentIndex() == 0;
    m_timelineMeta->setText(m_totalDurationMs > 0 ? (m_vm
        ? tr("%1 · %2").arg(contentTypeLabel(m_vm->displayedContentProfile().primaryType), formatMs(m_totalDurationMs))
        : formatMs(m_totalDurationMs)) : QString());
    if (chapters ? m_chapters.isEmpty() : m_scenes.isEmpty()) {
        auto* empty = new QLabel(chapters ? (m_vm && !m_vm->currentVideoPath().isEmpty()
            ? (m_vm->isIndexing() ? tr("正在生成内容章节") : tr("尚无内容章节，请重新构建")) : tr("请先打开视频")) : tr("暂无镜头数据"), m_container);
        empty->setTextFormat(Qt::PlainText); empty->setWordWrap(true); empty->setAlignment(Qt::AlignCenter);
        m_cardLayout->addWidget(empty);
    }
    const auto append = [&](const QString& key, const QByteArray& payload, qint64 start, qint64 end,
                            const std::function<QWidget*()>& create) {
        auto* card = previous.value(key);
        if (card && card->property("timelinePayload").toByteArray() == payload) previous.remove(key);
        else card = create();
        card->setProperty("timelineKey", key); card->setProperty("timelinePayload", payload);
        m_cards.append(card); m_cardRanges.append({start, end}); m_cardLayout->addWidget(card);
    };
    const auto identity = m_videoId + ":" + m_buildId + ":" + QString::number(m_themeRevision) + ":";
    if (chapters) for (int i = 0; i < m_chapters.size(); ++i) {
        const auto& chapter = m_chapters[i];
        append(identity + "chapter:" + chapter.chapterId, QJsonDocument(chapter.toJson()).toJson(QJsonDocument::Compact),
            chapter.startMs, chapter.endMs, [&, i] { return makeChapterCard(m_chapters[i], i); });
    } else for (const auto& scene : m_scenes) {
        const auto payload = QJsonDocument(QJsonObject{{"start", qint64(scene.startMs)}, {"end", qint64(scene.endMs)},
            {"duration", qint64(m_totalDurationMs)}, {"description", m_vm ? m_vm->sceneDescription(scene.id) : scene.description}}).toJson(QJsonDocument::Compact);
        append(identity + "scene:" + QString::number(scene.id), payload, scene.startMs, scene.endMs,
            [&] { return makeSceneCard(scene, m_totalDurationMs); });
        if (auto* label = m_cards.last()->findChild<QLabel*>("sceneDescription")) m_descLabels.insert(scene.id, label);
    }
    for (auto* card : previous) { card->hide(); card->setEnabled(false); card->deleteLater(); }
    m_cardLayout->addStretch(1);
    const auto revision = ++m_refreshRevision;
    QTimer::singleShot(0, this, [this, revision, anchor, anchorKey, anchorOffset, position] {
        if (revision != m_refreshRevision) return;
        QWidget* target = anchor && m_cards.contains(anchor.data()) ? anchor.data() : nullptr;
        if (!target) for (auto* card : m_cards) if (card->property("timelineKey").toString() == anchorKey) { target = card; break; }
        m_cardLayout->activate();
        m_scroll->verticalScrollBar()->setValue(target ? target->y() + anchorOffset : position);
    });
}

QWidget* TimelineTabWidget::makeChapterCard(const VideoChapter& chapter, int ordinal) {
    const AnalysisUi::Colors c(m_theme);
    auto* card = new SceneCard(chapter.startMs, m_container);
    card->setColors(c.surface, c.soft, c.border, c.primary);
    card->setAccessibleName(tr("%1，%2至%3，回看本章").arg(chapter.title, formatMs(chapter.startMs), formatMs(chapter.endMs)));
    auto* layout = new QVBoxLayout(card); layout->setContentsMargins(46, 16, 18, 16); layout->setSpacing(10);
    const auto addText = [&](const QString& text) {
        auto* label = new QLabel(text, card); label->setTextFormat(Qt::PlainText); label->setWordWrap(true);
        label->setMinimumWidth(0);
        label->setStyleSheet(QString("color:%1; font-size:14px; background:transparent; border:none;").arg(c.text.name()));
        layout->addWidget(label); return label;
    };
    auto* titleRow = new QHBoxLayout; titleRow->setSpacing(8);
    auto* title = new QLabel(chapter.title, card); title->setTextFormat(Qt::PlainText); title->setWordWrap(true); title->setMinimumWidth(0);
    title->setStyleSheet(QStringLiteral("color:%1; font-size:16px; font-weight:600; background:transparent; border:none;").arg(c.text.name()));
    titleRow->addWidget(title, 1);
    auto* playing = new QLabel(tr("当前章节"), card);
    auto badgePolicy = playing->sizePolicy(); badgePolicy.setRetainSizeWhenHidden(true); playing->setSizePolicy(badgePolicy);
    playing->setStyleSheet(QStringLiteral("color:%1; font-size:11px; background:%2; border:none; border-radius:9px; padding:3px 8px;")
        .arg(c.primary.name(), AnalysisUi::blend(c.surface, c.primary, .1).name()));
    titleRow->addWidget(playing, 0, Qt::AlignTop);
    layout->addLayout(titleRow);
    card->setTimelineStyle(ordinal == 0, ordinal == m_chapters.size() - 1, playing);
    addText(tr("%1 — %2").arg(formatMs(chapter.startMs), formatMs(chapter.endMs)))
        ->setStyleSheet(QStringLiteral("color:%1; font-size:12px; background:transparent; border:none;").arg(c.secondary.name()));
    if (!chapter.description.isEmpty()) addText(chapter.description);
    if (chapter.state != ArtifactState::Ready) {
        QStringList details;
        for (const auto& reason : chapter.incompleteReasons) {
            if (reason == "page_understanding_failed") details << tr("存在未成功理解的证据页");
            else if (reason == "missing_evidence") details << tr("部分所需证据缺失");
            else if (reason == "page_understanding_incomplete" || reason == "unit_understanding_incomplete") details << tr("证据理解未完成");
            else if (reason == "unit_synthesis_incomplete") details << tr("单元综合未完成");
            else if (reason == "chapter_planning_incomplete") details << tr("章节划分待完善");
            else if (reason == "chapter_refinement_incomplete") details << tr("章节正文整理未完成");
        }
        details.removeDuplicates();
        auto* status = addText(chapter.state == ArtifactState::Partial
            ? (details.isEmpty() ? tr("部分内容整理未完成") : tr("部分完成：%1").arg(details.join(tr("；"))))
            : tr("此范围理解不可用"));
        status->setStyleSheet(QStringLiteral("color:%1; font-size:12px; background:transparent; border:none;").arg(c.secondary.name()));
    }
    const auto video = m_videoId, build = m_buildId, id = chapter.chapterId;
    connect(card, &SceneCard::clicked, this, [this, video, build, id](int64_t) {
        if (video == m_videoId && build == m_buildId) emit chapterRequested(video, build, id);
    });
    return card;
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

    const auto video = m_vm ? m_vm->displayedVideoId() : QString{};
    connect(card, &SceneCard::clicked, this, [this, video](int64_t ms) {
        if (m_vm && !video.isEmpty() && m_vm->displayedVideoId() == video) emit seekRequested(video, ms);
    });

    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(6);

    // ---- 顶部行：序号 + 时间区间 ----
    auto* topRow = new QHBoxLayout();
    topRow->setSpacing(8);

    auto* indexLabel = new QLabel(tr("镜头 %1").arg(scene.id + 1), card);
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
    descLabel->setObjectName("sceneDescription");
    descLabel->setTextFormat(Qt::PlainText);
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

void TimelineTabWidget::updateHighlight(int64_t posMs, bool followPlayback)
{
    for (int i = 0; i < m_cardRanges.size() && i < m_cards.size(); ++i) {
        auto* card = qobject_cast<SceneCard*>(m_cards[i]);
        if (!card) continue;
        const bool active = m_cardRanges[i].first <= posMs && posMs < m_cardRanges[i].second;
        card->setHighlighted(active);

        // 自动滚动到活跃卡片
        if (active && followPlayback && !m_userScrolling) {
            m_scroll->ensureWidgetVisible(card, 0, 20);
        }
    }
}

bool TimelineTabWidget::eventFilter(QObject* obj, QEvent* event)
{
    if ((obj == m_scroll->viewport() || obj == m_scroll->verticalScrollBar()) && event->type() == QEvent::Wheel) {
        m_userScrolling = true;
        if (!m_scrollResetTimer) {
            m_scrollResetTimer = new QTimer(this);
            m_scrollResetTimer->setSingleShot(true);
            connect(m_scrollResetTimer, &QTimer::timeout, this,
                    [this]() { m_userScrolling = false; });
        }
        m_scrollResetTimer->start(3000);
    }
    return QWidget::eventFilter(obj, event);
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
