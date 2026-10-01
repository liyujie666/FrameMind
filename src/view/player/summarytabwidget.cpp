#include "view/player/summarytabwidget.h"
#include "view/player/videosummarycard.h"

#include "viewmodel/videoanalysisviewmodel.h"
#include "service/themeservice.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QFrame>
#include <QComboBox>
#include <QPushButton>

SummaryTabWidget::SummaryTabWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);
    auto* controls=new QHBoxLayout;
    m_typeSelector=new QComboBox(this);m_typeSelector->addItem(tr("自动识别"),-1);
    for(auto type:{VideoContentType::Meeting,VideoContentType::Interview,VideoContentType::Educational,VideoContentType::Presentation,VideoContentType::Tutorial,VideoContentType::Unknown}) m_typeSelector->addItem(contentTypeLabel(type),int(type));
    controls->addWidget(m_typeSelector,1);auto* rebuild=new QPushButton(tr("按此策略构建"),this);controls->addWidget(rebuild);
    connect(rebuild,&QPushButton::clicked,this,[this]{if(!m_vm) return;const int type=m_typeSelector->currentData().toInt();if(type<0) m_vm->rebuild(true);else m_vm->changeType(VideoContentType(type));});
    auto* cancel=new QPushButton(tr("取消"),this);controls->addWidget(cancel);connect(cancel,&QPushButton::clicked,this,[this]{if(m_vm) m_vm->cancelBuild();});
    root->addLayout(controls);m_buildState=new QLabel(this);m_buildState->setWordWrap(true);root->addWidget(m_buildState);

    // ---- 进度区域 ----
    m_progressArea = new QWidget(this);
    m_progressArea->setAttribute(Qt::WA_StyledBackground, false);
    auto* progressLayout = new QVBoxLayout(m_progressArea);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(4);

    m_progressLabel = new QLabel(tr("准备中..."), m_progressArea);
    m_progressLabel->setStyleSheet(
        "font-size: 12px; color: #888; background: transparent; border: none;");

    m_progressBar = new QProgressBar(m_progressArea);
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setFixedHeight(6);
    m_progressBar->setTextVisible(false);
    m_progressBar->setStyleSheet(
        "QProgressBar { border: none; border-radius: 3px; background: #2D2D3D; }"
        "QProgressBar::chunk { border-radius: 3px; background: #2979FF; }");

    progressLayout->addWidget(m_progressLabel);
    progressLayout->addWidget(m_progressBar);

    m_progressArea->hide();
    root->addWidget(m_progressArea);

    // ---- 滚动内容区 ----
    m_scroll = new QScrollArea(this);
    m_scroll->setWidgetResizable(true);
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    m_scrollContent = new QWidget(m_scroll);
    m_scrollContent->setAttribute(Qt::WA_StyledBackground, false);
    m_contentLayout = new QVBoxLayout(m_scrollContent);
    m_contentLayout->setContentsMargins(0, 4, 6, 4);
    m_contentLayout->setSpacing(12);

    // 空状态
    m_emptyLabel = new QLabel(tr("暂无摘要，请先打开视频"), m_scrollContent);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setStyleSheet(
        "color: #888; font-size: 13px; background: transparent; border: none;");
    m_contentLayout->addWidget(m_emptyLabel, 1);

    // 全视频概览卡片：标题、类型标签、复制操作和自适应正文。
    m_summaryCard = new VideoSummaryCard(m_scrollContent);
    m_summaryCard->hide();
    m_contentLayout->addWidget(m_summaryCard);

    // 场景描述区（初始隐藏）
    m_scenesSection = new QWidget(m_scrollContent);
    m_scenesSection->setAttribute(Qt::WA_StyledBackground, false);
    m_scenesSection->hide();
    m_scenesLayout = new QVBoxLayout(m_scenesSection);
    m_scenesLayout->setContentsMargins(0, 0, 0, 0);
    m_scenesLayout->setSpacing(8);

    auto* scenesTitle = new QLabel(tr("场景描述"), m_scenesSection);
    scenesTitle->setStyleSheet(
        "font-size: 12px; font-weight: 600; color: #888;"
        "background: transparent; border: none;");
    m_scenesLayout->addWidget(scenesTitle);

    m_contentLayout->addWidget(m_scenesSection);
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
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, &SummaryTabWidget::onThemeChanged);
        onThemeChanged();
    }
}

void SummaryTabWidget::setViewModel(VideoAnalysisViewModel* vm)
{
    if (m_vm == vm) return;
    if (m_vm) disconnect(m_vm, nullptr, this, nullptr);
    m_vm = vm;
    if (!m_vm) return;
    connect(m_vm,&VideoAnalysisViewModel::contentProfileReady,this,[this](const VideoContentProfile& p){
        m_typeSelector->setCurrentIndex(p.userOverride?m_typeSelector->findData(int(p.primaryType)):0);
        m_buildState->setText(tr("%1 · %2").arg(contentTypeLabel(p.primaryType),p.userOverride?tr("用户指定"):tr("自动识别／回退")));
        m_summaryCard->setContentType(contentTypeLabel(p.primaryType));
    });
    connect(m_vm,&VideoAnalysisViewModel::buildStateChanged,this,[this](const VideoBuildManifest& m){
        QString state=m.state==ArtifactState::Ready?tr("完整"):m.state==ArtifactState::Partial?tr("部分完成"):m.state==ArtifactState::Cancelled?tr("已取消"):tr("构建失败");
        m_buildState->setText(tr("%1 · %2").arg(contentTypeLabel(m.profile.primaryType),state));
        m_buildState->setToolTip(m.diagnostics.join('\n'));
    });
    connect(m_vm,&VideoAnalysisViewModel::semanticUnitsReady,this,[this](const QVector<SemanticUnit>& units){
        if(auto* title=qobject_cast<QLabel*>(m_scenesLayout->itemAt(0)->widget())) title->setText(units.isEmpty()?tr("场景描述"):tr("语义单元"));
        while(m_scenesLayout->count()>1) {auto* item=m_scenesLayout->takeAt(1);if(item->widget()) item->widget()->deleteLater();delete item;}
        for(const auto& u:units) {
            auto* label=new QLabel(m_scenesSection);label->setWordWrap(true);label->setTextFormat(Qt::PlainText);label->setTextInteractionFlags(Qt::TextSelectableByMouse);
            label->setText(QString("[%1-%2s] %3 · %4\n%5\n%6").arg(u.startMs/1000).arg(u.endMs/1000).arg(u.title,u.state==ArtifactState::Ready?tr("完整"):tr("部分完成"),u.fusedDescription,u.sourceChunkIds.isEmpty()?tr("无原始证据"):tr("原始来源：%1 条；已处理 %2/%3 页").arg(u.sourceChunkIds.size()).arg(u.coverage.processedPages).arg(u.coverage.totalPages)));
            m_scenesLayout->addWidget(label);
        }
        m_scenesSection->setVisible(!units.isEmpty());
    });

    connect(m_vm, &VideoAnalysisViewModel::progressChanged,
            this, &SummaryTabWidget::onProgressChanged);
    connect(m_vm, &VideoAnalysisViewModel::indexingChanged,
            this, &SummaryTabWidget::onIndexingChanged);
    connect(m_vm, &VideoAnalysisViewModel::summaryReady,
            this, &SummaryTabWidget::onSummaryReady);
    connect(m_vm, &VideoAnalysisViewModel::sceneDescribed,
            this, &SummaryTabWidget::onSceneDescribed);
    // 切换视频时 VM 会先 emit scenesReady({}) 清空 → 触发此处重置 UI
    connect(m_vm, &VideoAnalysisViewModel::scenesReady,
            this, [this](const QVector<Scene>& scenes) {
                if (scenes.isEmpty()) resetContent();
            });

    // 恢复已有状态
    if (!m_vm->videoSummary().isEmpty()) {
        onSummaryReady(m_vm->videoSummary());
    } else if (m_vm->isIndexing()) {
        onProgressChanged(m_vm->indexPercent(), m_vm->indexStageLabel());
        onIndexingChanged(true);
    }
}

void SummaryTabWidget::resetContent()
{
    // 清空摘要
    m_summaryCard->clear();
    m_summaryCard->hide();

    // 删除所有动态添加的场景描述条目（保留第一个子 widget：scenesTitle）
    // 从 layout 里逐项移除并销毁，跳过 index 0（scenesTitle label）
    while (m_scenesLayout->count() > 1) {
        QLayoutItem* item = m_scenesLayout->takeAt(1);
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    m_renderedSceneCount = 0;
    m_scenesSection->hide();

    // 重置进度条
    m_progressBar->setValue(0);
    m_progressLabel->setText(tr("准备中..."));
    m_progressArea->hide();

    // 显示空状态
    m_emptyLabel->show();
}

void SummaryTabWidget::onProgressChanged(int percent, const QString& label)
{
    m_progressBar->setValue(percent);
    m_progressLabel->setText(label);
    m_progressArea->setVisible((m_vm && m_vm->isIndexing()) || m_summaryCard->isHidden());
}

void SummaryTabWidget::onIndexingChanged(bool isIndexing)
{
    if(isIndexing) m_progressArea->show();
    if (!isIndexing) {
        m_progressBar->setValue(100);
    }
}

void SummaryTabWidget::onSummaryReady(const QString& summary)
{
    if (summary.trimmed().isEmpty()) return;
    m_emptyLabel->hide();
    m_summaryCard->setSummary(summary);
    if(m_vm) m_summaryCard->setContentType(contentTypeLabel(m_vm->contentProfile().primaryType));
    m_summaryCard->show();
    m_progressArea->hide();
}

void SummaryTabWidget::onSceneDescribed(int sceneId, const QString& description)
{
    if (description.trimmed().isEmpty()) return;

    m_emptyLabel->hide();
    addSceneDescEntry(sceneId, description.trimmed());
    m_scenesSection->show();
}

void SummaryTabWidget::onThemeChanged()
{
    applyScrollStyle();
    applyStyles();
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
    const bool dark = m_theme ? m_theme->isDark() : true;
    const QColor subText    = m_theme ? m_theme->color("textSecondary") : QColor(dark ? "#8B8B8B" : "#6B6B6B");
    const QColor primary    = m_theme ? m_theme->color("primary")       : QColor(dark ? "#2979FF" : "#1565C0");
    const QColor surfaceVar = m_theme ? m_theme->color("surfaceVariant"): QColor(dark ? "#2D2D3D" : "#F5F5F5");
    m_emptyLabel->setStyleSheet(QString(
        "font-size:13px; color:%1; background:transparent; border:none;")
        .arg(subText.name()));

    m_progressLabel->setStyleSheet(QString(
        "font-size: 12px; color: %1; background: transparent; border: none;")
        .arg(subText.name()));

    m_progressBar->setStyleSheet(QString(
        "QProgressBar { border: none; border-radius: 3px; background: %1; }"
        "QProgressBar::chunk { border-radius: 3px; background: %2; }")
        .arg(surfaceVar.name(), primary.name()));
}

void SummaryTabWidget::addSceneDescEntry(int sceneId, const QString& description)
{
    auto* entry = new QWidget(m_scenesSection);
    entry->setAttribute(Qt::WA_StyledBackground, false);
    auto* h = new QHBoxLayout(entry);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);

    const bool dark       = m_theme ? m_theme->isDark() : true;
    const QColor primary  = m_theme ? m_theme->color("primary")      : QColor(dark ? "#2979FF" : "#1565C0");
    const QColor textColor= m_theme ? m_theme->color("textPrimary")  : QColor(dark ? "#E0E0E0" : "#1A1A1A");
    const QColor subText  = m_theme ? m_theme->color("textSecondary"): QColor(dark ? "#8B8B8B" : "#6B6B6B");

    auto* dot = new QLabel(entry);
    dot->setFixedSize(8, 8);
    dot->setStyleSheet(QString(
        "background: %1; border-radius: 4px;").arg(primary.name()));

    auto* idLabel = new QLabel(tr("场景 %1").arg(sceneId + 1), entry);
    idLabel->setFixedWidth(52);
    idLabel->setStyleSheet(QString(
        "font-size: 11px; font-weight: 600; color: %1;"
        "background: transparent; border: none;").arg(primary.name()));

    auto* descLabel = new QLabel(description, entry);
    descLabel->setWordWrap(true);
    descLabel->setStyleSheet(QString(
        "font-size: 12px; color: %1; background: transparent; border: none;")
        .arg(textColor.name()));

    h->addWidget(dot);
    h->addWidget(idLabel);
    h->addWidget(descLabel, 1);

    // 插入到 scenesLayout 的末尾（stretch 之前，无 stretch 则直接 addWidget）
    m_scenesLayout->addWidget(entry);
    ++m_renderedSceneCount;
}
