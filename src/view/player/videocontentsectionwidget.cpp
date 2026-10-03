#include "view/player/videocontentsectionwidget.h"
#include "view/player/analysisuistyle.h"
#include "service/rag/video_presentation_policy_registry.h"
#include <QAbstractButton>
#include <QApplication>
#include <QClipboard>
#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QToolButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QStyle>
#include <QStyleOptionFocusRect>
#include <QStringList>
#include <QJsonDocument>

namespace {
QLabel* textLabel(const QString& text, QWidget* parent, const char* name = "entryBody") {
    auto* label = new QLabel(text, parent);
    label->setObjectName(QString::fromLatin1(name));
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}
QString timeText(qint64 ms) {
    const auto seconds = ms / 1000;
    const auto minuteSecond = QStringLiteral("%1:%2").arg(seconds / 60 % 60, 2, 10, QLatin1Char('0'))
        .arg(seconds % 60, 2, 10, QLatin1Char('0'));
    return seconds < 3600 ? minuteSecond
        : QStringLiteral("%1:%2").arg(seconds / 3600, 2, 10, QLatin1Char('0')).arg(minuteSecond);
}
QString statusTitle(ArtifactState state) {
    switch (state) {
    case ArtifactState::Ready: return QObject::tr("已完成");
    case ArtifactState::Partial: return QObject::tr("部分完成");
    case ArtifactState::Skipped: return QObject::tr("无适用内容");
    case ArtifactState::Failed: return QObject::tr("暂不可用");
    case ArtifactState::Cancelled: return QObject::tr("已取消");
    case ArtifactState::Running: return QObject::tr("正在整理");
    default: return QObject::tr("等待生成");
    }
}
QString statusDetail(ArtifactState state) {
    switch (state) {
    case ArtifactState::Partial: return QObject::tr("部分内容仍待完善，存在理解或覆盖缺口");
    case ArtifactState::Skipped: return QObject::tr("没有证据充分且适用的内容");
    case ArtifactState::Failed: return QObject::tr("此区域暂不可用，可重新构建");
    case ArtifactState::Cancelled: return QObject::tr("内容整理已取消");
    case ArtifactState::Pending: return QObject::tr("正在等待生成此区域");
    default: return {};
    }
}
// The keyboard-accessible toggle owns the title and chevron, while header
// actions remain sibling buttons. Wrapped titles determine layout height.
class EntryHeader final : public QAbstractButton {
public:
    EntryHeader(const QString& title, QWidget* parent) : QAbstractButton(parent) {
        setObjectName("entryHeader"); setCheckable(true);
        setFocusPolicy(Qt::StrongFocus); setCursor(Qt::PointingHandCursor);
        setAccessibleName(title);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
        setMinimumHeight(36);
        auto* layout = new QHBoxLayout(this);
        layout->setContentsMargins(0, 4, 0, 4); layout->setSpacing(12);
        auto* label = textLabel(title, this, "entryTitle");
        label->setTextInteractionFlags(Qt::NoTextInteraction);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
        auto* indicator = new QLabel(this);
        indicator->setObjectName("entryChevron");
        indicator->setAttribute(Qt::WA_TransparentForMouseEvents);
        indicator->setFixedSize(16, 16);
        layout->addWidget(indicator, 0, Qt::AlignVCenter);
        layout->addWidget(label, 1);
        connect(this, &QAbstractButton::toggled, this, [indicator](bool open) { indicator->setProperty("expanded", open); });
    }
    QSize sizeHint() const override { return layout() ? layout()->sizeHint().expandedTo(QSize(0, 36)) : QSize(0, 36); }
    QSize minimumSizeHint() const override { return QSize(0, 36); }
    bool hasHeightForWidth() const override { return layout() && layout()->hasHeightForWidth(); }
    int heightForWidth(int width) const override { return qMax(36, layout() ? layout()->totalHeightForWidth(width) : 36); }
protected:
    void paintEvent(QPaintEvent*) override {
        if (hasFocus()) {
            QStyleOptionFocusRect option;
            option.initFrom(this); option.rect = rect().adjusted(1, 1, -1, -1);
            QPainter painter(this);
            style()->drawPrimitive(QStyle::PE_FrameFocusRect, &option, &painter, this);
        }
    }
};
}

VideoContentSectionWidget::VideoContentSectionWidget(QWidget* parent) : VideoContentSectionWidget(false, parent) {}
VideoContentSectionWidget::VideoContentSectionWidget(bool explore, QWidget* parent) : QWidget(parent), m_explore(explore) {
    setObjectName(explore ? "videoExploreSection" : "videoContentSection");
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
    m_layout = new QVBoxLayout(this);
    m_layout->setContentsMargins(0, 10, 0, 0); m_layout->setSpacing(10);
    hide();
}
void VideoContentSectionWidget::setThemeService(ThemeService* theme) {
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;
    if (m_theme) connect(m_theme, &ThemeService::themeChanged, this, [this] { applyTheme(); });
    applyTheme();
}
void VideoContentSectionWidget::setSnapshot(const QString& videoId, const QString& buildId, const VideoPresentation& p) {
    if (m_videoId != videoId || m_buildId != buildId) {
        ++m_renderGeneration;
        m_expandedEntries.clear(); m_expansionInitialized = false;
        m_rowPayloads.clear();
    }
    m_videoId = videoId; m_buildId = buildId; m_presentation = p;
    render();
}
void VideoContentSectionWidget::clear() { setSnapshot({}, {}, {}); }
void VideoContentSectionWidget::setQuestionBusy(bool busy) {
    m_busy = busy;
    for (const auto& button : m_questions) if (button) button->setEnabled(!busy && m_indexReady);
}
void VideoContentSectionWidget::setIndexReady(bool ready) {
    m_indexReady = ready;
    for (const auto& button : m_questions) if (button) {
        button->setEnabled(!m_busy && ready);
        button->setToolTip(ready ? tr("在视频聊天中提出这个问题") : tr("索引就绪后可提问"));
    }
}
void VideoContentSectionWidget::render() {
    const auto generation = m_renderGeneration;
    m_questions.clear();
    auto previousRows = m_rowsById;
    m_rowsById.clear();
    QSet<QWidget*> cachedRows;
    for (auto* row : previousRows) cachedRows.insert(row);
    while (auto* item = m_layout->takeAt(0)) {
        if (auto* widget = item->widget()) if (!cachedRows.contains(widget)) {
            widget->hide(); widget->setEnabled(false); widget->deleteLater();
        }
        delete item;
    }
    const auto disposeOldRows = [&] {
        for (auto* row : previousRows) {
            row->setProperty("renderActive", false); row->hide(); row->setEnabled(false); row->deleteLater();
        }
        for (auto it = m_rowPayloads.begin(); it != m_rowPayloads.end();) {
            if (!m_rowsById.contains(it.key())) it = m_rowPayloads.erase(it); else ++it;
        }
    };
    const auto reuseRow = [&](const QString& key, const QByteArray& payload) -> QWidget* {
        QWidget* row = nullptr;
        if (m_rowPayloads.value(key) == payload) row = previousRows.take(key);
        if (row) m_rowsById.insert(key, row);
        m_rowPayloads.insert(key, payload);
        return row;
    };
    if (m_videoId.isEmpty() || m_buildId.isEmpty() || m_presentation.policyId.isEmpty()) { disposeOldRows(); hide(); return; }
    const auto policy = VideoPresentationPolicyRegistry::byId(m_presentation.policyId);
    if (!m_presentation.codecValid || m_presentation.schemaVersion != "presentation_v1" ||
        m_presentation.policySelection.isEmpty() || policy.id != m_presentation.policyId) { disposeOldRows(); hide(); return; }
    const auto section = m_explore ? m_presentation.secondarySection : m_presentation.primarySection;
    if (!section.codecValid) { disposeOldRows(); hide(); return; }
    const AnalysisUi::Colors colors(m_theme);
    auto* header = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(0, 0, 16, 0); headerLayout->setSpacing(10);
    headerLayout->addWidget(textLabel(m_explore ? policy.secondaryTitle : policy.primaryTitle, header, "regionHeading"), 1);
    auto* status = textLabel(statusTitle(section.state), header, "regionStatus");
    status->setToolTip(statusDetail(section.state));
    headerLayout->addWidget(status, 0, Qt::AlignTop); m_layout->addWidget(header);
    const auto detail = statusDetail(section.state);
    if (!detail.isEmpty() && section.state != ArtifactState::Partial) {
        auto* detailRow = new QWidget(this);
        auto* detailLayout = new QHBoxLayout(detailRow);
        detailLayout->setContentsMargins(0, 0, 0, 0);
        detailLayout->addWidget(textLabel(detail, detailRow, "regionDetail"));
        m_layout->addWidget(detailRow);
    }
    const bool hasContent = section.state == ArtifactState::Ready || section.state == ArtifactState::Partial || section.state == ArtifactState::Running;
    if (hasContent && !m_expansionInitialized && !section.entries.isEmpty()) {
        if (!m_explore) m_expandedEntries.insert(section.entries.first().entryId);
        m_expansionInitialized = true;
    }
    if (hasContent) for (const auto& entry : section.entries) {
        const QString rowKey = "entry:" + entry.entryId;
        QJsonObject payload{{"entry", entry.toJson()}};
        for (const auto& anchor : m_presentation.anchors) if (anchor.anchorId == entry.anchorId) payload["anchor"] = anchor.toJson();
        if (auto* row = reuseRow(rowKey, QJsonDocument(payload).toJson(QJsonDocument::Compact))) {
            m_layout->addWidget(row); continue;
        }
        auto* card = new QFrame(this);
        card->setProperty("analysisRowId", rowKey);
        card->setProperty("renderActive", true); m_rowsById.insert(rowKey, card);
        card->setObjectName("contentEntry"); card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Maximum);
        auto* layout = new QVBoxLayout(card);
        layout->setContentsMargins(16, 12, 16, 12); layout->setSpacing(10);
        auto* titleRow = new QWidget(card);
        auto* titleLayout = new QHBoxLayout(titleRow); titleLayout->setContentsMargins(0, 0, 0, 0); titleLayout->setSpacing(8);
        auto* expand = new EntryHeader(entry.title, titleRow); titleLayout->addWidget(expand, 1); layout->addWidget(titleRow);
        QWidget* previewRow = nullptr;
        QHBoxLayout* previewLayout = nullptr;
        bool hasPreview = false;
        if (m_explore) {
            previewRow = new QWidget(card);
            auto* preview = new AnalysisUi::PreviewLabel(entry.body.isEmpty() && !entry.points.isEmpty()
                ? entry.points.first().text : entry.body, previewRow);
            preview->setObjectName("entryPreview");
            hasPreview = !preview->toolTip().isEmpty();
            previewLayout = new QHBoxLayout(previewRow); previewLayout->setContentsMargins(0, 0, 0, 0); previewLayout->setSpacing(8);
            previewLayout->addWidget(preview, 1); layout->addWidget(previewRow);
        }
        auto* details = new QWidget(card);
        auto* body = new QVBoxLayout(details); body->setContentsMargins(0, 0, 0, 0); body->setSpacing(10);
        QString copied = entry.title;
        // The primary region displays key points only when expanded; its body
        // is retained in the snapshot for compatibility and empty-point fallback.
        QStringList displayedPoints;
        for (const auto& point : entry.points) {
            if (!point.text.trimmed().isEmpty()) displayedPoints.append(point.text);
        }
        if (m_explore && !entry.body.isEmpty()) {
            body->addWidget(textLabel(entry.body, details)); copied += "\n" + entry.body;
        } else if (!m_explore && displayedPoints.isEmpty() && !entry.body.trimmed().isEmpty()) {
            displayedPoints.append(entry.body);
        }
        for (const auto& point : displayedPoints) {
            auto* pointRow = new QWidget(details);
            auto* pointLayout = new QHBoxLayout(pointRow); pointLayout->setContentsMargins(0, 0, 0, 0); pointLayout->setSpacing(8);
            pointLayout->addWidget(textLabel(QStringLiteral("•"), pointRow, "pointBullet"), 0, Qt::AlignTop);
            pointLayout->addWidget(textLabel(point, pointRow), 1);
            body->addWidget(pointRow); copied += "\n• " + point;
        }
        if (entry.kind == "action_item") {
            const auto owner = entry.attributes["owner"].toString(), deadline = entry.attributes["deadline"].toString();
            const auto attributes = tr("负责人：%1；期限：%2").arg(owner.isEmpty() ? tr("未明确") : owner, deadline.isEmpty() ? tr("未明确") : deadline);
            body->addWidget(textLabel(attributes, details, "entryAttributes")); copied += "\n" + attributes;
        }
        layout->addWidget(details);
        auto* actionArea = new QWidget(card);
        auto* actions = new QHBoxLayout(actionArea); actions->setContentsMargins(0, 0, 0, 0); actions->setSpacing(8);
        for (const auto& anchor : m_presentation.anchors) if (!entry.anchorId.isEmpty() && anchor.anchorId == entry.anchorId) {
            auto* review = new QToolButton(actionArea); review->setObjectName(m_explore ? "analysisReview" : "analysisHeaderReview");
            review->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            review->setIcon(AnalysisUi::icon(AnalysisUi::Icon::Play, colors.primary));
            review->setText((anchor.precision == "unit" ? tr("章节起点 %1") : tr("回看 %1")).arg(timeText(anchor.startMs)));
            review->setToolTip(anchor.precision == "unit" ? tr("定位到该内容所在单元起点") : tr("回看该要点对应的视频证据"));
            review->setAccessibleName(review->text() + " " + entry.title); review->setCursor(Qt::PointingHandCursor);
            const auto video = m_videoId, build = m_buildId, id = anchor.anchorId;
            const auto requestReview = [this, generation, video, build, id, card] {
                if (generation == m_renderGeneration && card->property("renderActive").toBool()) emit reviewRequested(video, build, id);
            };
            connect(review, &QToolButton::clicked, this, requestReview); actions->addWidget(review);
            if (previewLayout) {
                auto* compactReview = new QToolButton(previewRow);
                compactReview->setObjectName("analysisGhost"); compactReview->setProperty("reviewAction", true);
                compactReview->setText(anchor.precision == "unit" ? tr("起点 %1").arg(timeText(anchor.startMs)) : timeText(anchor.startMs));
                compactReview->setToolTip(review->toolTip()); compactReview->setAccessibleName(review->accessibleName());
                compactReview->setCursor(Qt::PointingHandCursor);
                connect(compactReview, &QToolButton::clicked, this, requestReview);
                previewLayout->addWidget(compactReview); hasPreview = true;
            }
            break;
        }
        if (m_explore) actions->addStretch();
        auto* copy = new QToolButton(actionArea); copy->setObjectName("analysisGhost"); copy->setProperty("copyAction", true);
        copy->setIcon(AnalysisUi::copyIcon(colors)); copy->setIconSize(QSize(16, 16));
        copy->setToolTip(m_explore ? tr("复制完整条目") : tr("复制条目要点")); copy->setAccessibleName(tr("复制 %1").arg(entry.title)); copy->setCursor(Qt::PointingHandCursor);
        connect(copy, &QToolButton::clicked, card, [copied, card] {
            if (card->property("renderActive").toBool()) QApplication::clipboard()->setText(copied);
        }); actions->addWidget(copy);
        if (m_explore) layout->addWidget(actionArea);
        else {
            actionArea->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Preferred);
            titleLayout->addWidget(actionArea, 0, Qt::AlignVCenter);
        }
        connect(expand, &QAbstractButton::toggled, this, [this, generation, id = entry.entryId, card, details, previewRow, actionArea, hasPreview, expand](bool open) {
            if (generation != m_renderGeneration || !card->property("renderActive").toBool()) return;
            if (open) m_expandedEntries.insert(id); else m_expandedEntries.remove(id);
            details->setVisible(open); actionArea->setVisible(!m_explore || open);
            if (previewRow) previewRow->setVisible(!open && hasPreview);
            expand->setToolTip(open ? tr("收起要点") : tr("展开要点"));
            card->setProperty("expanded", open); card->style()->unpolish(card); card->style()->polish(card); card->update();
            const AnalysisUi::Colors c(m_theme);
            for (auto* chevron : expand->findChildren<QLabel*>(QStringLiteral("entryChevron")))
                chevron->setPixmap(AnalysisUi::icon(open ? AnalysisUi::Icon::ChevronUp : AnalysisUi::Icon::ChevronDown, c.secondary).pixmap(16, 16));
        });
        const bool open = m_expandedEntries.contains(entry.entryId);
        card->setProperty("expanded", open); details->setVisible(open); actionArea->setVisible(!m_explore || open);
        if (previewRow) previewRow->setVisible(!open && hasPreview);
        expand->setChecked(open); expand->setToolTip(open ? tr("收起要点") : tr("展开要点"));
        m_layout->addWidget(card);
    }
    if (m_explore && hasContent) for (const auto& question : section.questions) {
        const auto rowKey = "question:" + question.questionId;
        if (auto* row = reuseRow(rowKey, QJsonDocument(question.toJson()).toJson(QJsonDocument::Compact))) {
            for (auto* ask : row->findChildren<QPushButton*>()) m_questions.append(QPointer<QPushButton>(ask));
            m_layout->addWidget(row); continue;
        }
        auto* row = new QFrame(this); row->setObjectName("questionRow");
        row->setProperty("analysisRowId", rowKey);
        row->setProperty("renderActive", true); m_rowsById.insert(rowKey, row);
        auto* layout = new QHBoxLayout(row); layout->setContentsMargins(16, 12, 16, 12); layout->setSpacing(12);
        layout->addWidget(textLabel(question.text, row, "questionText"), 1);
        auto* ask = new QPushButton(tr("提问"), row); ask->setObjectName("analysisAsk");
        ask->setLayoutDirection(Qt::RightToLeft); // Native button places the trailing arrow after its label.
        ask->setIcon(AnalysisUi::icon(AnalysisUi::Icon::ArrowRight, colors.primary));
        ask->setCursor(Qt::PointingHandCursor); ask->setToolTip(tr("在视频聊天中提出这个问题"));
        ask->setAccessibleName(tr("提问：%1").arg(question.text));
        ask->setEnabled(!m_busy && m_indexReady); m_questions.append(QPointer<QPushButton>(ask));
        const auto video = m_videoId, build = m_buildId, id = question.questionId;
        connect(ask, &QPushButton::clicked, this, [this, generation, video, build, id, row] {
            if (m_busy || !m_indexReady || generation != m_renderGeneration || !row->property("renderActive").toBool()) return;
            setQuestionBusy(true);
            emit questionRequested(video, build, id);
        }); layout->addWidget(ask, 0, Qt::AlignTop); m_layout->addWidget(row);
    }
    disposeOldRows(); setIndexReady(m_indexReady);
    applyTheme(); show();
}
void VideoContentSectionWidget::applyTheme() {
    const AnalysisUi::Colors c(m_theme);
    const auto state = m_explore ? m_presentation.secondarySection.state : m_presentation.primarySection.state;
    setStyleSheet(QStringLiteral(
        "QWidget { background:transparent; border:none; }"
        "QWidget#videoContentSection, QWidget#videoExploreSection { background:transparent; border:none; }"
        "QLabel { color:%1; background:transparent; border:none; font-size:14px; }"
        "QLabel#regionHeading { font-size:18px; font-weight:600; }"
        "QLabel#regionStatus, QLabel#regionDetail, QLabel#entryPreview, QLabel#entryAttributes { color:%2; font-size:12px; }"
        "QLabel#regionStatus { color:%5; }"
        "QLabel#entryTitle { font-size:15px; font-weight:600; } QLabel#pointBullet { color:%6; }"
        "QFrame#contentEntry { background:%4; border:1px solid %3; border-radius:10px; }"
        "QFrame#contentEntry[expanded=\"true\"] { background:%4; border:1px solid %3; border-radius:10px; }"
        "QFrame#questionRow { background:%4; border:1px solid %3; border-radius:10px; }"
        "QLabel#questionText { font-size:14px; }"
    ).arg(c.text.name(), c.secondary.name(), AnalysisUi::cardBorder(c).name(), AnalysisUi::cardSurface(c).name(), AnalysisUi::statusColor(c, state).name(), c.primary.name())
        + AnalysisUi::actionStyles(c)
        + QStringLiteral(
            "QToolButton#analysisHeaderReview { color:%1; background:transparent; border:none; border-radius:8px; padding:7px 9px; font-size:12px; }"
            "QToolButton#analysisHeaderReview:hover, QToolButton#analysisHeaderReview:focus { background:%2; border:none; }")
            .arg(c.primary.name(), c.soft.name()));
    for (auto* chevron : findChildren<QLabel*>(QStringLiteral("entryChevron")))
        chevron->setPixmap(AnalysisUi::icon(chevron->property("expanded").toBool() ? AnalysisUi::Icon::ChevronUp : AnalysisUi::Icon::ChevronDown, c.secondary).pixmap(16, 16));
    for (auto* button : findChildren<QToolButton*>()) if (!button->property("reviewAction").toBool()) {
        if (button->property("copyAction").toBool())
            button->setIcon(AnalysisUi::copyIcon(c));
        else button->setIcon(AnalysisUi::icon(AnalysisUi::Icon::Play, c.primary));
    }
    for (const auto& ask : m_questions) if (ask) ask->setIcon(AnalysisUi::icon(AnalysisUi::Icon::ArrowRight, c.primary));
}
