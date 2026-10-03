#include "view/player/summarytabwidget.h"
#include "view/player/videosummarycard.h"
#include "view/player/videocontentsectionwidget.h"
#include "view/player/analysisuistyle.h"
#include "view/player/analysiscombobox.h"

#include "viewmodel/videoanalysisviewmodel.h"
#include "service/themeservice.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QComboBox>
#include <QToolButton>
#include <QGridLayout>
#include <QResizeEvent>
#include <QTimer>

SummaryTabWidget::SummaryTabWidget(QWidget* parent)
    : QWidget(parent)
{
    setObjectName("summaryTab"); setAttribute(Qt::WA_StyledBackground, true);
    setAutoFillBackground(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(16, 8, 16, 12);
    root->setSpacing(10);
    m_controlsArea = new QWidget(this);
    auto* controls=new QGridLayout(m_controlsArea); m_controlsLayout = controls;
    controls->setContentsMargins(0, 0, 0, 0); controls->setSpacing(8);
    m_typeBadge = new QLabel(this); m_typeBadge->setObjectName("analysisTypeBadge");
    m_typeBadge->setTextFormat(Qt::PlainText); m_typeBadge->hide(); controls->addWidget(m_typeBadge, 0, 0);
    m_typeSelector=new AnalysisUi::ComboBox(this);m_typeSelector->addItem(tr("自动识别"),-1);
    m_typeSelector->setObjectName("analysisTypeSelector"); m_typeSelector->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    m_typeSelector->setAccessibleName(tr("视频内容类型"));
    for(auto type:{VideoContentType::Meeting,VideoContentType::Interview,VideoContentType::Educational,VideoContentType::Presentation,VideoContentType::Tutorial,VideoContentType::Documentary,VideoContentType::Drama,VideoContentType::News,VideoContentType::Vlog,VideoContentType::Unknown}) m_typeSelector->addItem(contentTypeLabel(type),int(type));
    controls->addWidget(m_typeSelector, 0, 1); controls->setColumnStretch(2, 1);
    m_rebuildButton = new QToolButton(this); m_rebuildButton->setObjectName("analysisGhost");
    m_rebuildButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon); m_rebuildButton->setText(tr("重新构建"));
    m_rebuildButton->setToolTip(tr("按所选视频类型重新整理内容")); m_rebuildButton->setCursor(Qt::PointingHandCursor);
    m_rebuildButton->setEnabled(false); controls->addWidget(m_rebuildButton, 0, 3);
    connect(m_rebuildButton,&QToolButton::clicked,this,[this]{if(!m_vm) return;const int type=m_typeSelector->currentData().toInt();if(type<0) m_vm->rebuild(true);else m_vm->changeType(VideoContentType(type));});
    m_cancelButton = new QToolButton(this); m_cancelButton->setObjectName("analysisGhost"); m_cancelButton->setText(tr("取消"));
    m_cancelButton->setCursor(Qt::PointingHandCursor); m_cancelButton->hide(); controls->addWidget(m_cancelButton, 0, 4);
    connect(m_cancelButton,&QToolButton::clicked,this,[this]{if(m_vm) m_vm->cancelBuild();});
    m_buildState = new QLabel(m_controlsArea); m_buildState->setObjectName("analysisBuildState");
    m_buildState->setTextFormat(Qt::PlainText); m_buildState->setWordWrap(true); m_buildState->hide();
    layoutControls(false); root->addWidget(m_controlsArea);

    // ---- 滚动内容区 ----
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_scrollContent = new QWidget(m_scroll);
    m_scrollContent->setAttribute(Qt::WA_StyledBackground, false);
    m_contentLayout = new QVBoxLayout(m_scrollContent);
    m_contentLayout->setContentsMargins(0, 8, 0, 0);
    m_contentLayout->setSpacing(22);

    // 空状态
    m_emptyLabel = new QLabel(tr("暂无摘要，请先打开视频"), m_scrollContent);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setStyleSheet(
        "color: #888; font-size: 13px; background: transparent; border: none;");
    m_contentLayout->addWidget(m_emptyLabel, 1);

    // 全视频概览卡片：标题、复制操作和自适应正文。
    m_previousOverviewLabel = new QLabel(m_scrollContent);
    m_previousOverviewLabel->setWordWrap(true);
    m_previousOverviewLabel->setTextFormat(Qt::PlainText);
    m_previousOverviewLabel->setStyleSheet("color: #888; font-size: 12px; background: transparent; border: none;");
    m_previousOverviewLabel->hide();
    m_contentLayout->addWidget(m_previousOverviewLabel);
    m_summaryCard = new VideoSummaryCard(m_scrollContent);
    m_summaryCard->hide();
    m_contentLayout->addWidget(m_summaryCard);

    m_interactionMessage = new QLabel(m_scrollContent);
    m_interactionMessage->setTextFormat(Qt::PlainText); m_interactionMessage->setWordWrap(true); m_interactionMessage->hide();
    m_contentLayout->addWidget(m_interactionMessage);
    m_contentSection = new VideoContentSectionWidget(m_scrollContent);
    m_exploreSection = new VideoExploreSectionWidget(m_scrollContent);
    m_contentLayout->addWidget(m_contentSection); m_contentLayout->addWidget(m_exploreSection);
    connect(m_contentSection, &VideoContentSectionWidget::reviewRequested, this, &SummaryTabWidget::reviewRequested);
    connect(m_exploreSection, &VideoContentSectionWidget::reviewRequested, this, &SummaryTabWidget::reviewRequested);
    connect(m_exploreSection, &VideoContentSectionWidget::questionRequested, this, &SummaryTabWidget::questionRequested);
    m_contentLayout->addStretch();

    m_scroll->setWidget(m_scrollContent);
    root->addWidget(m_scroll, 1);

    applyScrollStyle();
    applyStyles();
}

void SummaryTabWidget::setThemeService(ThemeService* theme)
{
    if (m_theme == theme) return;
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;
    m_summaryCard->setThemeService(theme);
    m_contentSection->setThemeService(theme); m_exploreSection->setThemeService(theme);
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, &SummaryTabWidget::onThemeChanged);
    }
    onThemeChanged();
}

void SummaryTabWidget::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    if (!m_controlsLayout) return;
    const bool compact = event->size().width() - 32 < 550;
    if (compact != m_compactControls) layoutControls(compact);
}
void SummaryTabWidget::layoutControls(bool compact) {
    m_compactControls = compact;
    while (auto* item = m_controlsLayout->takeAt(0)) delete item;
    for (int column = 0; column < 6; ++column) m_controlsLayout->setColumnStretch(column, 0);
    m_controlsLayout->addWidget(m_typeBadge, 0, 0, Qt::AlignLeft);
    if (compact) {
        m_controlsLayout->addWidget(m_typeSelector, 1, 0, Qt::AlignLeft);
        m_controlsLayout->addWidget(m_buildState, 1, 1, Qt::AlignLeft);
        m_controlsLayout->setColumnStretch(2, 1);
        m_controlsLayout->addWidget(m_rebuildButton, 2, 0, Qt::AlignLeft);
        m_controlsLayout->addWidget(m_cancelButton, 2, 1, Qt::AlignLeft);
    } else {
        m_controlsLayout->addWidget(m_typeSelector, 0, 1, Qt::AlignLeft);
        m_controlsLayout->addWidget(m_buildState, 0, 2, Qt::AlignLeft);
        m_controlsLayout->setColumnStretch(3, 1);
        m_controlsLayout->addWidget(m_rebuildButton, 0, 4);
        m_controlsLayout->addWidget(m_cancelButton, 0, 5);
    }
}

void SummaryTabWidget::setViewModel(VideoAnalysisViewModel* vm)
{
    if (m_vm == vm) return;
    if (m_vm) disconnect(m_vm, nullptr, this, nullptr);
    m_vm = vm;
    resetContent();
    if (!m_vm) return;
    connect(m_vm, &VideoAnalysisViewModel::contentProfileReady, this, [this] { syncProfileAndState(); });
    connect(m_vm, &VideoAnalysisViewModel::buildStateChanged, this, [this] { syncProfileAndState(); });
    connect(m_vm, &VideoAnalysisViewModel::presentationReady, this, &SummaryTabWidget::renderPresentation);
    connect(m_vm, &VideoAnalysisViewModel::indexingChanged, this, &SummaryTabWidget::onIndexingChanged);
    connect(m_vm, &VideoAnalysisViewModel::summaryReady, this, &SummaryTabWidget::onSummaryReady);
    refreshSnapshot();
}
void SummaryTabWidget::refreshSnapshot() {
    if (!m_vm) { resetContent(); return; }
    syncProfileAndState();
    renderPresentation(m_vm->displayedVideoId(), m_vm->displayedBuildId(), m_vm->presentation());
    onSummaryReady(m_vm->videoSummary());
    onIndexingChanged(m_vm->isIndexing());
}
void SummaryTabWidget::setQuestionBusy(bool busy) {
    m_questionBusy = busy;
    m_exploreSection->setQuestionBusy(busy);
}
void SummaryTabWidget::showInteractionMessage(const QString& text) {
    m_interactionMessage->setText(text); m_interactionMessage->setVisible(!text.isEmpty());
}
void SummaryTabWidget::syncProfileAndState() {
    if (!m_vm) return;
    const auto profile = m_vm->contentProfile();
    m_typeBadge->setText(contentTypeLabel(profile.primaryType)); m_typeBadge->show();
    m_typeBadge->setToolTip(profile.reasoning);
    m_rebuildButton->setEnabled(!m_vm->currentVideoPath().isEmpty() && !m_vm->isIndexing());
    m_cancelButton->setVisible(m_vm->isIndexing());
    m_typeSelector->setCurrentIndex(profile.userOverride ? m_typeSelector->findData(int(profile.primaryType)) : 0);
    const auto build = m_vm->buildState();
    m_exploreSection->setIndexReady(m_vm->isDisplayedIndexReady());
    QString state;
    switch (build.state) {
    case ArtifactState::Running: state = tr("正在构建"); break;
    case ArtifactState::Ready: state = tr("已完成"); break;
    case ArtifactState::Partial: state = tr("部分完成"); break;
    case ArtifactState::Cancelled: state = tr("已取消"); break;
    case ArtifactState::Failed: state = tr("构建失败"); break;
    default: state = tr("尚未构建"); break;
    }
    m_buildState->setText(QStringLiteral("● ") + state);
    auto diagnostics = build.diagnostics;
    if (build.overviewState == ArtifactState::Failed) diagnostics.prepend(tr("视频概览暂不可用"));
    m_buildState->setToolTip(diagnostics.join('\n'));
    m_buildState->setProperty("artifactState", int(build.state));
    m_buildState->setVisible(!m_vm->currentVideoPath().isEmpty()); applyBuildStateStyle();
    updateOverviewIdentity();
}
void SummaryTabWidget::renderPresentation(const QString& video, const QString& build, const VideoPresentation& p) {
    if (!m_vm || video != m_vm->displayedVideoId() || build != m_vm->displayedBuildId()) return;
    preserveReadingPosition();
    showInteractionMessage({});
    if (video.isEmpty()) { resetContent(); syncProfileAndState(); return; }
    m_contentSection->setSnapshot(video, build, p); m_exploreSection->setSnapshot(video, build, p);
    setQuestionBusy(m_questionBusy);
    const bool legacy = build.isEmpty() || p.policySelection.isEmpty() || p.schemaVersion != "presentation_v1";
    const bool invalid = m_vm->displayedBuild().artifacts["content_rebuild_required"].toBool();
    m_emptyLabel->setText(m_vm->isPreview() && m_vm->isIndexing() ? tr("视频概览正在生成") :
        legacy || invalid ? tr("内容产物不完整，请重新构建") : tr("暂无概览"));
    m_emptyLabel->setVisible(m_vm->videoSummary().isEmpty());
    updateOverviewIdentity();
}
void SummaryTabWidget::resetContent() {
    ++m_refreshRevision;
    m_scroll->verticalScrollBar()->setValue(0);
    m_summaryCard->clear(); m_summaryCard->hide(); m_previousOverviewLabel->hide();
    m_contentSection->clear(); m_exploreSection->clear(); showInteractionMessage({});
    m_typeSelector->setCurrentIndex(0); m_buildState->clear(); m_buildState->setToolTip({}); m_buildState->hide();
    m_typeBadge->clear(); m_typeBadge->hide();
    m_rebuildButton->setEnabled(m_vm && !m_vm->currentVideoPath().isEmpty() && !m_vm->isIndexing());
    m_cancelButton->setVisible(m_vm && m_vm->isIndexing());
    m_emptyLabel->setText(tr("暂无内容，请先打开视频")); m_emptyLabel->show();
}

void SummaryTabWidget::onIndexingChanged(bool)
{
    updateOverviewIdentity();
    syncProfileAndState();
}

void SummaryTabWidget::updateOverviewIdentity()
{
    if (m_vm && m_vm->videoSummary().trimmed().isEmpty() && !m_vm->currentVideoPath().isEmpty()) {
        const auto state = m_vm->displayedBuild().overviewState;
        m_emptyLabel->setText(state == ArtifactState::Failed ? tr("视频概览暂不可用，可重新构建") :
            state == ArtifactState::Cancelled ? tr("视频概览生成已取消") :
            m_vm->isIndexing() ? (state == ArtifactState::Running ? tr("视频概览正在生成") : tr("等待生成视频概览")) : tr("暂无概览"));
        m_emptyLabel->show();
    }
    const bool previous = m_vm && m_vm->isShowingPreviousContent();
    m_previousOverviewLabel->setText(m_vm && m_vm->isIndexing()
        ? tr("正在重新生成，当前显示上次构建的内容") : tr("当前显示上次构建的内容"));
    m_previousOverviewLabel->setVisible(previous);
}

void SummaryTabWidget::onSummaryReady(const QString& summary)
{
    preserveReadingPosition();
    if (summary.trimmed().isEmpty()) {
        m_summaryCard->clear();
        m_summaryCard->hide();
        updateOverviewIdentity();
        m_emptyLabel->show();
        return;
    }
    const bool incomplete = m_vm && (m_vm->displayedBuildId().isEmpty() || m_vm->presentation().policySelection.isEmpty() ||
        m_vm->displayedBuild().artifacts["content_rebuild_required"].toBool());
    m_emptyLabel->setVisible(incomplete);
    m_summaryCard->setSummary(summary);
    m_summaryCard->show();
    updateOverviewIdentity();
}

void SummaryTabWidget::onThemeChanged()
{
    applyScrollStyle();
    applyStyles();
}

void SummaryTabWidget::preserveReadingPosition() {
    const int position = m_scroll->verticalScrollBar()->value();
    QPointer<QWidget> anchor;
    QString anchorId, anchorRegion;
    int offset = 0, anchorTop = -1;
    for (auto* card : m_scrollContent->findChildren<QFrame*>()) {
        if (!card->isVisibleTo(m_scrollContent) || (card->objectName() != "contentEntry" &&
            card->objectName() != "questionRow" && card->objectName() != "summaryContentCard")) continue;
        const int top = card->mapTo(m_scrollContent, QPoint(0, 0)).y();
        if (top <= position && top > anchorTop) {
            anchor = card; anchorTop = top; offset = position - top;
            anchorId = card->property("analysisRowId").toString();
            anchorRegion = card->parentWidget()->objectName();
        }
    }
    const auto revision = ++m_refreshRevision;
    QTimer::singleShot(0, this, [this, revision, anchor, anchorId, anchorRegion, offset, position] {
        if (revision != m_refreshRevision) return;
        // Let the overview's deferred text-height update settle first.
        QTimer::singleShot(0, this, [this, revision, anchor, anchorId, anchorRegion, offset, position] {
            if (revision != m_refreshRevision) return;
            QWidget* target = anchor && anchor->isVisibleTo(m_scrollContent) ? anchor.data() : nullptr;
            if (!target && !anchorId.isEmpty()) for (auto* card : m_scrollContent->findChildren<QFrame*>())
                if (card->isVisibleTo(m_scrollContent) && card->property("analysisRowId").toString() == anchorId &&
                    card->parentWidget()->objectName() == anchorRegion) { target = card; break; }
            m_contentLayout->activate();
            m_scroll->verticalScrollBar()->setValue(target ? target->mapTo(m_scrollContent, QPoint(0, 0)).y() + offset : position);
        });
    });
}

void SummaryTabWidget::applyScrollStyle()
{
    const bool dark   = m_theme ? m_theme->isDark() : true;
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

void SummaryTabWidget::applyStyles()
{
    const AnalysisUi::Colors c(m_theme);
    static_cast<AnalysisUi::ComboBox*>(m_typeSelector)->setThemeColors(c);
    setStyleSheet(QStringLiteral(
        "QWidget { background:transparent; border:none; }"
        "QWidget#summaryTab { background:%3; border:none; }"
        "QLabel#analysisTypeBadge { color:%1; background:%2; border:1px solid %4; border-radius:8px; padding:6px 10px; font-size:12px; }")
        .arg(c.primary.name(), c.soft.name(), c.dark ? (m_theme ? m_theme->color("background").name() : QStringLiteral("#121212")) : QStringLiteral("#FFFFFF"), AnalysisUi::cardBorder(c).name()) + AnalysisUi::actionStyles(c));
    m_rebuildButton->setIcon(AnalysisUi::icon(AnalysisUi::Icon::Refresh, c.primary));
    const auto secondaryStyle = QStringLiteral("color:%1; font-size:12px; background:transparent; border:none;").arg(c.secondary.name());
    applyBuildStateStyle();
    m_previousOverviewLabel->setStyleSheet(secondaryStyle);
    m_interactionMessage->setStyleSheet(secondaryStyle);
    const bool dark = m_theme ? m_theme->isDark() : true;
    const QColor subText    = m_theme ? m_theme->color("textSecondary") : QColor(dark ? "#8B8B8B" : "#6B6B6B");
    m_emptyLabel->setStyleSheet(QString(
        "font-size:13px; color:%1; background:transparent; border:none;")
        .arg(subText.name()));

}

void SummaryTabWidget::applyBuildStateStyle() {
    const AnalysisUi::Colors c(m_theme);
    const QColor ink = AnalysisUi::statusColor(c, static_cast<ArtifactState>(m_buildState->property("artifactState").toInt()));
    m_buildState->setStyleSheet(QStringLiteral(
        "color:%1; background:%2; border:1px solid %3; border-radius:12px; padding:5px 10px; font-size:12px;")
        .arg(ink.name(), AnalysisUi::blend(c.surface, ink, c.dark ? .12 : .05).name(),
             AnalysisUi::blend(c.surface, ink, c.dark ? .28 : .16).name()));
}
