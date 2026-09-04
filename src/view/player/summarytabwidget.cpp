#include "view/player/summarytabwidget.h"

#include "viewmodel/videoanalysisviewmodel.h"
#include "service/themeservice.h"

#include <QScrollArea>
#include <QScrollBar>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QTextBrowser>
#include <QFrame>

SummaryTabWidget::SummaryTabWidget(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, false);
    setAutoFillBackground(false);

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);

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
    m_contentLayout->addStretch(1);
    m_emptyLabel = new QLabel(tr("暂无摘要，请先打开视频"), m_scrollContent);
    m_emptyLabel->setAlignment(Qt::AlignCenter);
    m_emptyLabel->setWordWrap(true);
    m_emptyLabel->setStyleSheet(
        "color: #888; font-size: 13px; background: transparent; border: none;");
    m_contentLayout->addWidget(m_emptyLabel, 1);

    m_contentLayout->addStretch(1);

    // 全视频摘要（QTextBrowser 支持 Markdown 渲染）
    m_summaryBrowser = new QTextBrowser(m_scrollContent);
    m_summaryBrowser->setReadOnly(true);
    m_summaryBrowser->setOpenLinks(false);
    m_summaryBrowser->setFrameShape(QFrame::NoFrame);
    m_summaryBrowser->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_summaryBrowser->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_summaryBrowser->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    // 让 QTextBrowser 高度随内容自动伸展
    m_summaryBrowser->document()->setDocumentMargin(0);
    connect(m_summaryBrowser->document(), &QTextDocument::contentsChanged,
            this, [this]() {
                const int h = int(m_summaryBrowser->document()->size().height()) + 4;
                m_summaryBrowser->setFixedHeight(qMax(h, 20));
            });
    m_summaryBrowser->hide();
    m_contentLayout->addWidget(m_summaryBrowser);

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
    m_contentLayout->addStretch(1);

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
    m_summaryBrowser->clear();
    m_summaryBrowser->hide();

    // 删除所有动态添加的场景描述条目（保留第一个子 widget：scenesTitle）
    const QList<QObject*> children = m_scenesLayout->parentWidget()
                                         ? m_scenesSection->children()
                                         : QList<QObject*>{};
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
    m_progressArea->setVisible(true);
}

void SummaryTabWidget::onIndexingChanged(bool isIndexing)
{
    if (!isIndexing) {
        m_progressBar->setValue(100);
    }
}

void SummaryTabWidget::onSummaryReady(const QString& summary)
{
    m_emptyLabel->hide();
    m_summaryBrowser->setMarkdown(summary);
    m_summaryBrowser->show();
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
    const QColor textColor  = m_theme ? m_theme->color("textPrimary")   : QColor(dark ? "#E0E0E0" : "#1A1A1A");
    const QColor subText    = m_theme ? m_theme->color("textSecondary") : QColor(dark ? "#8B8B8B" : "#6B6B6B");
    const QColor primary    = m_theme ? m_theme->color("primary")       : QColor(dark ? "#2979FF" : "#1565C0");
    const QColor surfaceVar = m_theme ? m_theme->color("surfaceVariant"): QColor(dark ? "#2D2D3D" : "#F5F5F5");
    const QColor surface    = m_theme ? m_theme->color("surface")       : QColor(dark ? "#1E1E2E" : "#FFFFFF");
    const QColor border     = m_theme ? m_theme->color("border")        : QColor(dark ? "#3A3A4A" : "#E0E0E0");

    // h2/h3 底部装饰线颜色：主题色低透明度
    const QString accentFaint = QString("rgba(%1,%2,%3,40)")
        .arg(primary.red()).arg(primary.green()).arg(primary.blue());
    // blockquote/code 背景
    const QString codeBg = surfaceVar.name();
    // 正文稍小的辅助色
    const QString subHex = subText.name();

    m_summaryBrowser->setStyleSheet(QString(
        "QTextBrowser {"
        "  background: transparent;"
        "  border: none;"
        "  color: %1;"
        "  font-size: 13px;"
        "  selection-background-color: %2;"
        "}"
        "QScrollBar:vertical { width: 0px; }"
    ).arg(textColor.name(), primary.lighter(160).name()));

    // Qt 的 QTextDocument 支持有限的 CSS，仅用它支持的属性
    m_summaryBrowser->document()->setDefaultStyleSheet(QString(
        "body   { color:%1; font-size:13px; line-height:1.8; margin:0; padding:0; }"

        "h1 { color:%2; font-size:17px; font-weight:700;"
        "     margin-top:14px; margin-bottom:6px;"
        "     padding-bottom:4px; border-bottom:2px solid %2; }"

        "h2 { color:%2; font-size:15px; font-weight:700;"
        "     margin-top:12px; margin-bottom:4px;"
        "     padding-bottom:3px; border-bottom:1px solid %3; }"

        "h3 { color:%2; font-size:13px; font-weight:600;"
        "     margin-top:10px; margin-bottom:3px; }"

        "p  { margin-top:4px; margin-bottom:6px; }"

        "ul, ol { margin-top:4px; margin-bottom:6px; margin-left:18px; padding-left:0; }"
        "li { margin-bottom:3px; }"

        "strong, b { color:%1; font-weight:700; }"
        "em, i     { font-style:italic; color:%4; }"

        "code { font-family:Consolas,'Courier New',monospace; font-size:12px;"
        "       background:%5; color:%2; padding:1px 4px; border-radius:3px; }"

        "pre  { background:%5; border-radius:6px; padding:8px 10px;"
        "       margin:6px 0; font-size:12px; }"

        "blockquote { border-left:3px solid %2; margin:6px 0 6px 4px;"
        "             padding:2px 8px; color:%4; }"

        "hr { border:none; border-top:1px solid %6; margin:10px 0; }"
    ).arg(
        textColor.name(),   // %1 body/strong
        primary.name(),     // %2 heading/accent
        accentFaint,        // %3 h2 border (faint)
        subHex,             // %4 em/blockquote
        codeBg,             // %5 code/pre bg
        border.name()       // %6 hr
    ));

    // 切换主题后须重新渲染已有内容
    if (m_summaryBrowser->isVisible()) {
        const QString md = m_summaryBrowser->toMarkdown();
        if (!md.trimmed().isEmpty()) m_summaryBrowser->setMarkdown(md);
    }

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
