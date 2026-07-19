#include "view/knowledge/knowledgeview.h"
#include "view/knowledge/videoindexcard.h"
#include "view/knowledge/chunkbrowserwidget.h"
#include "view/knowledge/searchpreviewwidget.h"
#include "viewmodel/knowledgeviewmodel.h"
#include "service/themeservice.h"

#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QScrollArea>
#include <QStackedWidget>
#include <QSplitter>
#include <QLabel>
#include <QLineEdit>
#include <QToolButton>
#include <QButtonGroup>
#include <QFrame>
#include <QTimer>
#include <QMessageBox>

KnowledgeView::KnowledgeView(QWidget* parent)
    : QWidget(parent)
{
    setAttribute(Qt::WA_StyledBackground, true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* root = new QHBoxLayout(this);
    root->setContentsMargins(16, 16, 16, 16);
    root->setSpacing(12);

    auto* leftContainer = new QWidget(this);
    leftContainer->setFixedWidth(300);
    leftContainer->setAttribute(Qt::WA_StyledBackground, true);
    buildLeftPanel(leftContainer);
    root->addWidget(leftContainer);

    auto* rightContainer = new QWidget(this);
    rightContainer->setAttribute(Qt::WA_StyledBackground, true);
    rightContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    buildRightPanel(rightContainer);
    root->addWidget(rightContainer, 1);
}

void KnowledgeView::buildLeftPanel(QWidget* parent)
{
    auto* v = new QVBoxLayout(parent);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);

    // Title row
    auto* titleRow = new QHBoxLayout();
    titleRow->setSpacing(6);

    auto* titleLabel = new QLabel(tr("知识库"), parent);
    titleLabel->setStyleSheet(QStringLiteral(
        "font-size:15px; font-weight:700; background:transparent;"));
    titleRow->addWidget(titleLabel);
    titleRow->addStretch(1);

    m_refreshButton = new QToolButton(parent);
    m_refreshButton->setFixedSize(28, 28);
    m_refreshButton->setIconSize(QSize(18, 18));
    m_refreshButton->setToolTip(tr("刷新列表"));
    m_refreshButton->setCursor(Qt::PointingHandCursor);
    connect(m_refreshButton, &QToolButton::clicked,
            this, &KnowledgeView::refresh);
    titleRow->addWidget(m_refreshButton);

    m_cleanupButton = new QToolButton(parent);
    m_cleanupButton->setFixedSize(28, 28);
    m_cleanupButton->setIconSize(QSize(16, 16));
    m_cleanupButton->setToolTip(tr("清理 30 天前的过期索引"));
    m_cleanupButton->setCursor(Qt::PointingHandCursor);
    connect(m_cleanupButton, &QToolButton::clicked,
            this, &KnowledgeView::onCleanupRequested);
    titleRow->addWidget(m_cleanupButton);

    v->addLayout(titleRow);

    // Filter bar
    m_filterEdit = new QLineEdit(parent);
    m_filterEdit->setPlaceholderText(tr("过滤视频…"));
    m_filterEdit->setFixedHeight(30);
    connect(m_filterEdit, &QLineEdit::textChanged,
            this, &KnowledgeView::updateListFilter);
    v->addWidget(m_filterEdit);

    // Count label
    m_videosCountLabel = new QLabel(tr("共 0 个视频"), parent);
    m_videosCountLabel->setStyleSheet(QStringLiteral(
        "font-size:11px;color:#8B8B8B;background:transparent;"));
    v->addWidget(m_videosCountLabel);

    // Scroll area
    m_videoListArea = new QScrollArea(parent);
    m_videoListArea->setWidgetResizable(true);
    m_videoListArea->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_videoListArea->setFrameShape(QFrame::NoFrame);

    m_videoListHost = new QWidget();
    m_videoListHost->setAttribute(Qt::WA_StyledBackground, false);
    m_videoListLayout = new QVBoxLayout(m_videoListHost);
    m_videoListLayout->setContentsMargins(2, 2, 2, 2);
    m_videoListLayout->setSpacing(2);
    m_videoListLayout->addStretch(1);

    m_videoListArea->setWidget(m_videoListHost);
    v->addWidget(m_videoListArea, 1);

    // Status bar
    m_statusBar = new QLabel(parent);
    m_statusBar->hide();
    m_statusBar->setWordWrap(true);
    m_statusBar->setStyleSheet(QStringLiteral(
        "font-size:11px;color:#8B8B8B;background:transparent;"));
    v->addWidget(m_statusBar);
}

void KnowledgeView::buildRightPanel(QWidget* parent)
{
    auto* v = new QVBoxLayout(parent);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(0);

    m_rightStack = new QStackedWidget(parent);

    // Page 0: empty state
    m_emptyState = new QWidget();
    {
        auto* ev = new QVBoxLayout(m_emptyState);
        ev->setAlignment(Qt::AlignCenter);
        ev->setSpacing(12);

        auto* iconLabel = new QLabel(m_emptyState);
        iconLabel->setAlignment(Qt::AlignCenter);
        iconLabel->setPixmap(
            QIcon(QStringLiteral(":/icons/knowledge_light.png")).pixmap(56, 56));
        iconLabel->setStyleSheet(QStringLiteral("background:transparent;"));
        ev->addWidget(iconLabel);

        auto* msgLabel = new QLabel(tr("从左侧选择一个视频\n查看其知识库详情"), m_emptyState);
        msgLabel->setAlignment(Qt::AlignCenter);
        msgLabel->setStyleSheet(QStringLiteral(
            "font-size:14px;color:#8B8B8B;background:transparent;"));
        ev->addWidget(msgLabel);

        auto* subLabel = new QLabel(
            tr("打开并分析视频后，知识条目将自动积累"), m_emptyState);
        subLabel->setAlignment(Qt::AlignCenter);
        subLabel->setStyleSheet(QStringLiteral(
            "font-size:11px;color:#666;background:transparent;"));
        ev->addWidget(subLabel);
    }

    // Page 1: content
    m_contentWidget = new QWidget();
    {
        auto* cv = new QVBoxLayout(m_contentWidget);
        cv->setContentsMargins(0, 0, 0, 0);
        cv->setSpacing(8);

        // 视频文件名标题
        m_videoTitleLabel = new QLabel(m_contentWidget);
        m_videoTitleLabel->setAttribute(Qt::WA_StyledBackground, false);
        m_videoTitleLabel->setStyleSheet(QStringLiteral(
            "font-size:14px;font-weight:700;background:transparent;"));
        m_videoTitleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
        m_videoTitleLabel->setTextInteractionFlags(Qt::NoTextInteraction);
        cv->addWidget(m_videoTitleLabel);

        // Tab strip
        auto* tabRow = new QHBoxLayout();
        tabRow->setSpacing(4);
        tabRow->setContentsMargins(0, 0, 0, 0);

        auto* tabGroup = new QButtonGroup(m_contentWidget);
        tabGroup->setExclusive(true);

        m_detailTabBtn = new QToolButton(m_contentWidget);
        m_detailTabBtn->setText(tr("知识详情"));
        m_detailTabBtn->setCheckable(true);
        m_detailTabBtn->setChecked(true);
        m_detailTabBtn->setCursor(Qt::PointingHandCursor);

        m_searchTabBtn = new QToolButton(m_contentWidget);
        m_searchTabBtn->setText(tr("检索测试"));
        m_searchTabBtn->setCheckable(true);
        m_searchTabBtn->setCursor(Qt::PointingHandCursor);

        tabGroup->addButton(m_detailTabBtn, 0);
        tabGroup->addButton(m_searchTabBtn, 1);

        tabRow->addWidget(m_detailTabBtn);
        tabRow->addWidget(m_searchTabBtn);
        tabRow->addStretch(1);
        cv->addLayout(tabRow);

        // Tab content stack
        m_tabStack = new QStackedWidget(m_contentWidget);

        m_chunkBrowser  = new ChunkBrowserWidget(m_tabStack);
        m_searchPreview = new SearchPreviewWidget(m_tabStack);

        m_tabStack->addWidget(m_chunkBrowser);   // index 0
        m_tabStack->addWidget(m_searchPreview);  // index 1
        cv->addWidget(m_tabStack, 1);

        connect(m_detailTabBtn, &QToolButton::toggled,
                this, [this](bool checked) {
            if (checked) m_tabStack->setCurrentIndex(0);
            applyTheme();
        });
        connect(m_searchTabBtn, &QToolButton::toggled,
                this, [this](bool checked) {
            if (checked) m_tabStack->setCurrentIndex(1);
            applyTheme();
        });
    }

    m_rightStack->addWidget(m_emptyState);    // 0
    m_rightStack->addWidget(m_contentWidget); // 1
    m_rightStack->setCurrentIndex(0);
    v->addWidget(m_rightStack, 1);
}

void KnowledgeView::setViewModel(KnowledgeViewModel* vm)
{
    if (m_vm == vm) return;
    if (m_vm) disconnect(m_vm, nullptr, this, nullptr);
    m_vm = vm;

    if (m_chunkBrowser)  m_chunkBrowser->setViewModel(vm);
    if (m_searchPreview) m_searchPreview->setViewModel(vm);

    if (m_vm) {
        connect(m_vm, &KnowledgeViewModel::indexedVideosChanged,
                this, &KnowledgeView::onIndexedVideosChanged);
        connect(m_vm, &KnowledgeViewModel::videoIndexRemoved,
                this, &KnowledgeView::onVideoIndexRemoved);
        connect(m_vm, &KnowledgeViewModel::statusMessage,
                this, &KnowledgeView::onStatusMessage);
        connect(m_vm, &KnowledgeViewModel::staleIndexCleaned,
                this, &KnowledgeView::onStaleIndexCleaned);

        QTimer::singleShot(0, m_vm, &KnowledgeViewModel::loadIndexedVideos);
    }
}

void KnowledgeView::setThemeService(ThemeService* theme)
{
    if (m_theme == theme) return;
    if (m_theme) disconnect(m_theme, nullptr, this, nullptr);
    m_theme = theme;

    if (m_chunkBrowser)  m_chunkBrowser->setThemeService(theme);
    if (m_searchPreview) m_searchPreview->setThemeService(theme);
    for (auto* card : m_cards) card->setThemeService(theme);

    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, &KnowledgeView::onThemeChanged);
    }
    applyTheme();
}

void KnowledgeView::refresh()
{
    if (m_vm) m_vm->loadIndexedVideos();
}

void KnowledgeView::onIndexedVideosChanged(
    const QVector<KnowledgeViewModel::VideoIndexSummary>& videos)
{
    for (auto* card : m_cards) {
        m_videoListLayout->removeWidget(card);
        card->deleteLater();
    }
    m_cards.clear();

    const QString filter = m_filterEdit ? m_filterEdit->text().trimmed() : QString();

    for (const auto& summary : videos) {
        if (!filter.isEmpty()) {
            if (!summary.fileName.contains(filter, Qt::CaseInsensitive) &&
                !summary.filePath.contains(filter, Qt::CaseInsensitive) &&
                !summary.videoId.contains(filter, Qt::CaseInsensitive))
                continue;
        }

        auto* card = new VideoIndexCard(summary, m_videoListHost);
        if (m_theme) card->setThemeService(m_theme);
        card->setSelected(summary.videoId == m_selectedVideoId);

        connect(card, &VideoIndexCard::selected,
                this, [this, summary](const QString& vid) {
            m_selectedVideoId = vid;
            for (auto* c : m_cards) c->setSelected(c->videoId() == vid);
            if (m_vm) m_vm->selectVideo(vid);
            // Update right panel title
            if (m_videoTitleLabel) {
                const QString name = summary.fileName.isEmpty()
                    ? summary.videoId.left(28) : summary.fileName;
                m_videoTitleLabel->setText(name);
                m_videoTitleLabel->setToolTip(summary.filePath);
            }
            m_rightStack->setCurrentIndex(1);
        });
        connect(card, &VideoIndexCard::removeRequested,
                this, [this](const QString& vid) {
            if (!m_vm) return;
            const auto ret = QMessageBox::question(
                this, tr("删除索引"),
                tr("确定要删除该视频的所有知识库索引吗？此操作无法撤销。"),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (ret == QMessageBox::Yes)
                m_vm->removeVideoIndex(vid);
        });

        m_videoListLayout->insertWidget(m_videoListLayout->count() - 1, card);
        m_cards.append(card);
    }

    m_videosCountLabel->setText(tr("共 %1 个视频").arg(m_cards.size()));
    updateEmptyState();
}

void KnowledgeView::onVideoIndexRemoved(const QString& videoId)
{
    if (m_selectedVideoId == videoId) {
        m_selectedVideoId.clear();
        m_rightStack->setCurrentIndex(0);
    }
}

void KnowledgeView::onStatusMessage(const QString& msg, bool isError)
{
    if (!m_statusBar) return;
    m_statusBar->setText(msg);
    m_statusBar->setStyleSheet(isError
        ? QStringLiteral("font-size:11px;color:#FF4C4C;background:transparent;")
        : QStringLiteral("font-size:11px;color:#8B8B8B;background:transparent;"));
    m_statusBar->show();
    QTimer::singleShot(3000, m_statusBar, &QLabel::hide);
}

void KnowledgeView::onStaleIndexCleaned(int count)
{
    Q_UNUSED(count)
}

void KnowledgeView::onCleanupRequested()
{
    if (!m_vm) return;
    const auto ret = QMessageBox::question(
        this, tr("清理过期索引"),
        tr("清理 30 天内未访问的视频索引，以释放存储空间。\n是否继续？"),
        QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
    if (ret == QMessageBox::Yes)
        m_vm->cleanupStale(30);
}

void KnowledgeView::updateEmptyState()
{
    if (!m_rightStack) return;
    if (m_selectedVideoId.isEmpty()) {
        m_rightStack->setCurrentIndex(0);
    }
}

void KnowledgeView::updateListFilter(const QString& /*filterText*/)
{
    if (!m_vm) return;
    onIndexedVideosChanged(m_vm->indexedVideos());
}

void KnowledgeView::onThemeChanged()
{
    applyTheme();
    for (auto* card : m_cards) card->update();
}

void KnowledgeView::applyTheme()
{
    if (!m_theme) return;
    const bool dark   = m_theme->isDark();
    const QString bg      = m_theme->color(QStringLiteral("background")).name();
    const QString surface = m_theme->color(QStringLiteral("surface")).name();
    const QString text    = m_theme->color(QStringLiteral("textPrimary")).name();
    const QString textSub = m_theme->color(QStringLiteral("textSecondary")).name();
    const QString border  = m_theme->color(QStringLiteral("border")).name();
    const QString accent  = m_theme->color(QStringLiteral("primary")).name();

    // 悬浮色：主题色加低透明度叠加，亮暗主题都契合
    const QColor accentClr = m_theme->color(QStringLiteral("primary"));
    const QString hoverBg  = QStringLiteral("rgba(%1,%2,%3,28)")
        .arg(accentClr.red()).arg(accentClr.green()).arg(accentClr.blue());

    // 滚动条颜色（对齐 TimelineTabWidget）
    const QColor thumbClr  = m_theme->color(QStringLiteral("scrollThumb"));
    const QString thumb      = thumbClr.name();
    const QString thumbHover = (dark ? thumbClr.lighter(130) : thumbClr.darker(120)).name();

    // 亮色主题用 _dark 图标，暗色主题用 _light 图标
    const QString iconSuffix = dark ? QStringLiteral("_light") : QStringLiteral("_dark");

    setStyleSheet(QString("KnowledgeView { background:%1; }").arg(bg));

    // --- 右侧标题颜色 ---
    if (m_videoTitleLabel)
        m_videoTitleLabel->setStyleSheet(
            QString("font-size:14px;font-weight:700;background:transparent;color:%1;")
            .arg(text));

    // --- 左栏列表滚动区（样式对齐 TimelineTabWidget::applyScrollStyle）---
    if (m_videoListArea) {
        m_videoListArea->setStyleSheet(QString(
            "QScrollArea { background:%1; border:1px solid %2; border-radius:12px; }"
            "QScrollBar:vertical {"
            "  width:5px; background:transparent; margin:0; border-radius:2px; }"
            "QScrollBar::handle:vertical {"
            "  background:%3; border-radius:2px; min-height:24px; }"
            "QScrollBar::handle:vertical:hover { background:%4; }"
            "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height:0; }"
            "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical {"
            "  background:transparent; }"
        ).arg(surface, border, thumb, thumbHover));
        m_videoListHost->setStyleSheet(QString("background:%1;").arg(surface));
    }

    if (m_filterEdit) {
        m_filterEdit->setStyleSheet(QString(
            "QLineEdit{background:%1;color:%2;border:1px solid %3;"
            "border-radius:8px;padding:2px 8px;font-size:12px;}"
            "QLineEdit:focus{border-color:%4;}"
        ).arg(surface, text, border, accent));
    }

    if (m_videosCountLabel)
        m_videosCountLabel->setStyleSheet(
            QString("font-size:11px;color:%1;background:transparent;").arg(textSub));

    // 刷新按钮
    if (m_refreshButton) {
        m_refreshButton->setIcon(
            QIcon(QStringLiteral(":/icons/replay") + iconSuffix + QStringLiteral(".png")));
        m_refreshButton->setStyleSheet(QString(
            "QToolButton{border:none;border-radius:6px;background:transparent;}"
            "QToolButton:hover{background:%1;}").arg(hoverBg));
    }

    // 清理按钮（无 delete 图标时降级为文字）
    if (m_cleanupButton) {
        const QIcon delIcon(
            QStringLiteral(":/icons/delete") + iconSuffix + QStringLiteral(".png"));
        if (!delIcon.isNull())
            m_cleanupButton->setIcon(delIcon);
        else
            m_cleanupButton->setText(QStringLiteral("✕"));
        m_cleanupButton->setStyleSheet(QString(
            "QToolButton{border:none;border-radius:6px;background:transparent;"
            "color:%1;font-size:12px;}"
            "QToolButton:hover{background:rgba(255,76,76,25);}").arg(textSub));
    }

    // --- Tab 按钮 ---
    auto makeTabStyle = [&](bool active) -> QString {
        if (active)
            return QString("QToolButton{background:%1;color:#FFF;border:none;"
                           "border-radius:8px;font-size:12px;font-weight:600;"
                           "padding:4px 12px;}").arg(accent);
        return QString("QToolButton{background:transparent;color:%1;border:none;"
                       "border-radius:8px;font-size:12px;padding:4px 12px;}"
                       "QToolButton:hover{background:%2;}").arg(text, hoverBg);
    };

    if (m_detailTabBtn)
        m_detailTabBtn->setStyleSheet(makeTabStyle(m_detailTabBtn->isChecked()));
    if (m_searchTabBtn)
        m_searchTabBtn->setStyleSheet(makeTabStyle(m_searchTabBtn->isChecked()));
}
