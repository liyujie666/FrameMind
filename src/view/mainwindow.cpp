#include "view/mainwindow.h"

#include "view/common/customtitlebar.h"
#include "view/common/settingsdialog.h"
#include "view/sidebar/sidebarview.h"
#include "view/player/playerview.h"
#include "view/chat/chatview.h"
#include "view/filelist/filelistview.h"
#include "view/common/themedpanel.h"
#include "view/common/segmentedcontrol.h"
#include "viewmodel/playerviewmodel.h"
#include "viewmodel/chatviewmodel.h"
#include "viewmodel/filelistviewmodel.h"
#include "service/settingsservice.h"
#include "service/agentservice.h"
#include "service/filemanagerservice.h"
#include "service/themeservice.h"

#include <QStackedWidget>
#include <QStackedLayout>
#include <QSplitter>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QGridLayout>
#include <QWidget>
#include <QLabel>
#include <QLineEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMenuBar>
#include <QMenu>
#include <QActionGroup>
#include <QFileDialog>
#include <QStandardPaths>
#include <QFileInfo>
#include <QIcon>
#include <QToolButton>
#include <QResizeEvent>

namespace {

/// 一行分析结果占位（左侧列表用）
QWidget* makeAnalysisRow(QWidget* parent, int widthPct,
                         const QColor& dotColor, const QColor& barColor)
{
    auto* w = new QWidget(parent);
    w->setAttribute(Qt::WA_StyledBackground, false);
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(0, 0, 0, 0);
    h->setSpacing(8);

    auto* dot = new QLabel(w);
    dot->setFixedSize(10, 10);
    dot->setStyleSheet(QString("background:%1; border-radius:2px;").arg(dotColor.name()));

    auto* bar = new QLabel(w);
    bar->setFixedHeight(8);
    bar->setStyleSheet(QString("background:%1; border-radius:4px;").arg(barColor.name()));
    bar->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    h->addWidget(dot);
    h->addWidget(bar, widthPct);
    h->addStretch(100 - widthPct);
    return w;
}

QWidget* makeTimelineBar(QWidget* parent, int marks,
                         const QColor& bar, const QColor& mark)
{
    auto* w = new QWidget(parent);
    w->setFixedHeight(28);
    w->setStyleSheet(QString("background:%1; border-radius:14px;").arg(bar.name()));
    auto* h = new QHBoxLayout(w);
    h->setContentsMargins(12, 6, 12, 6);
    h->setSpacing(0);
    h->addStretch(1);
    for (int i = 0; i < marks; ++i) {
        auto* m = new QLabel(w);
        m->setFixedSize(3, 16);
        m->setStyleSheet(QString("background:%1; border-radius:1px;").arg(mark.name()));
        h->addWidget(m);
        h->addStretch(1);
    }
    return w;
}

/// 构造一个"分析 Tab"占位内容（供 SegmentedControl 切换用）
QWidget* buildAnalysisTabContent(QWidget* parent, ThemeService* theme, int variant)
{
    const QColor barCol = theme
        ? theme->color(QStringLiteral("surfaceVariant"))
        : QColor("#2D2D3D");
    const QColor markCol = theme
        ? theme->color(QStringLiteral("textPrimary"))
        : QColor("#E0E0E0");
    const QColor dotCol = theme
        ? theme->color(QStringLiteral("primary"))
        : QColor("#2979FF");

    auto* w = new QWidget(parent);
    w->setAttribute(Qt::WA_StyledBackground, false);
    auto* body = new QHBoxLayout(w);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(20);

    auto* leftCol = new QVBoxLayout();
    leftCol->setSpacing(8);

    // 按 variant 调整不同 Tab 的条目宽度模式，视觉上有差异
    QVector<int> pcts;
    switch (variant) {
        case 0:  pcts = { 60, 55, 65, 45 }; break;   // 时间线
        case 1:  pcts = { 70, 40, 55, 60 }; break;   // 检测
        default: pcts = { 50, 65, 40, 55 }; break;   // 字幕
    }
    for (int pct : pcts) {
        leftCol->addWidget(makeAnalysisRow(w, pct, dotCol, barCol));
    }
    leftCol->addStretch(1);
    body->addLayout(leftCol, 1);

    auto* rightCol = new QVBoxLayout();
    rightCol->setSpacing(10);
    rightCol->addWidget(makeTimelineBar(w, 3, barCol, markCol));

    auto makeSub = [&](int leftPct, int widthPct) {
        auto* bar = new QLabel(w);
        bar->setFixedHeight(8);
        bar->setStyleSheet(QString("background:%1; border-radius:4px;").arg(barCol.name()));
        bar->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::Fixed);
        auto* row = new QHBoxLayout();
        row->addStretch(leftPct);
        row->addWidget(bar, widthPct);
        row->addStretch(qMax(0, 100 - leftPct - widthPct));
        rightCol->addLayout(row);
    };
    makeSub(0, 70);
    makeSub(15, 55);

    rightCol->addStretch(1);
    body->addLayout(rightCol, 2);
    return w;
}

} // namespace

MainWindow::MainWindow(PlayerViewModel* playerVM,
                       ChatViewModel* chatVM,
                       FileListViewModel* fileListVM,
                       SettingsService* settings,
                       AgentService* agent,
                       FileManagerService* fileService,
                       ThemeService* theme,
                       LLMProviderService* providers,
                       QWidget* parent)
    : QMainWindow(parent)
    , m_playerVM(playerVM)
    , m_chatVM(chatVM)
    , m_fileListVM(fileListVM)
    , m_settings(settings)
    , m_agent(agent)
    , m_fileService(fileService)
    , m_theme(theme)
    , m_providers(providers)
{
    setWindowTitle(QStringLiteral("Frame Mind"));
    resize(1400, 900);
    // 只设最小尺寸
    setMinimumSize(960, 600);

    // 去掉原生窗口标题栏
    setWindowFlags(windowFlags() | Qt::FramelessWindowHint);

    auto* central = new QWidget(this);
    central->setAutoFillBackground(true);
    auto* rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    // 顶部自定义标题栏
    m_titleBar = new CustomTitleBar(m_theme, this);
    rootLayout->addWidget(m_titleBar);

    // 主内容区域（左侧导航 + 页面栈）
    auto* contentWidget = new QWidget(central);
    contentWidget->setAttribute(Qt::WA_StyledBackground, false);
    auto* contentLayout = new QHBoxLayout(contentWidget);
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(0);

    m_sidebar = new SidebarView(contentWidget);
    if (m_theme) m_sidebar->setThemeService(m_theme);

    m_pageStack = new QStackedWidget(contentWidget);
    m_pageStack->addWidget(buildChatPage());      // index 0：对话页
    m_pageStack->addWidget(buildFilePage());      // index 1：文件列表页
    m_pageStack->addWidget(buildKnowledgePage()); // index 2：知识库页（占位）

    contentLayout->addWidget(m_sidebar);
    contentLayout->addWidget(m_pageStack, 1);

    rootLayout->addWidget(contentWidget, 1);
    setCentralWidget(central);

    // 连接设置按钮信号
    connect(m_sidebar, &SidebarView::settingsClicked,
            this, &MainWindow::onOpenSettings);

    if (m_playerView && m_playerVM) {
        m_playerView->setViewModel(m_playerVM);
        connect(m_playerView, &PlayerView::fullscreenChanged,
                this, &MainWindow::onPlayerFullscreenChanged);
    }
    if (m_chatView && m_chatVM) {
        m_chatView->setViewModel(m_chatVM);
        if (m_theme) m_chatView->setThemeService(m_theme);
    }
    if (m_fileListView && m_fileListVM) {
        m_fileListView->setViewModel(m_fileListVM);
        if (m_theme) m_fileListView->setThemeService(m_theme);
    }

    // Sidebar 路由 → QStackedWidget
    connect(m_sidebar, &SidebarView::pageRequested,
            this, &MainWindow::onNavRequested);

    // 主题切换：刷新页面背景 + 重建 Analysis 内容配色
    if (m_theme) {
        connect(m_theme, &ThemeService::themeChanged,
                this, &MainWindow::onThemeChanged);
        onThemeChanged(m_theme->isDark());
    }
}

QWidget* MainWindow::buildChatPage()
{
    m_chatPage = new QWidget(this);
    m_chatPage->setAttribute(Qt::WA_StyledBackground, true);
    m_chatPage->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    // 页面底色由 onThemeChanged 应用

    auto* pageLayout = new QHBoxLayout(m_chatPage);
    pageLayout->setContentsMargins(16, 16, 16, 16);
    pageLayout->setSpacing(12);

    // ---- 左侧列（播放器容器 + 分析容器）----
    m_leftContainer = new QWidget(m_chatPage);
    m_leftContainer->setAttribute(Qt::WA_StyledBackground, false);
    m_leftContainer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    auto* leftLayout = new QVBoxLayout(m_leftContainer);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(8);

    // 播放器容器（ThemedPanel 圆角卡片，内嵌 PlayerView）
    m_playerPanel = new ThemedPanel(m_leftContainer);
    m_playerPanel->setRadius(16);
    m_playerPanel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    if (m_theme) m_playerPanel->setThemeService(m_theme);

    auto* playerPanelLayout = new QVBoxLayout(m_playerPanel);
    playerPanelLayout->setContentsMargins(16, 16, 16, 16);
    playerPanelLayout->setSpacing(0);

    m_playerView = new PlayerView(m_playerPanel);
    if (m_theme) m_playerView->setThemeService(m_theme);
    playerPanelLayout->addWidget(m_playerView, 1); // stretch=1 填满剩余空间

    // 分析容器
    m_analysisPanel = buildAnalysisPanel(m_leftContainer);

    leftLayout->addWidget(m_playerPanel, 65);
    leftLayout->addWidget(m_analysisPanel, 35);

    // ---- 右侧：ChatView 圆角卡片 ----
    m_chatView = new ChatView(m_chatPage);
    m_chatView->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored);
    m_chatView->setMinimumWidth(340);
    m_chatView->setMaximumWidth(520);

    pageLayout->addWidget(m_leftContainer, 3);
    pageLayout->addWidget(m_chatView, 2);

    return m_chatPage;
}

ThemedPanel* MainWindow::buildAnalysisPanel(QWidget* parent)
{
    auto* panel = new ThemedPanel(parent);
    panel->setRadius(16);
    panel->setMinimumHeight(200);
    panel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    if (m_theme) panel->setThemeService(m_theme);

    auto* v = new QVBoxLayout(panel);
    v->setContentsMargins(20, 14, 20, 14);
    v->setSpacing(12);

    // 顶部：标题 + 滑块 Tab
    auto* top = new QHBoxLayout();
    top->setSpacing(16);

    auto* title = new QLabel(tr("Analysis Results"), panel);
    title->setAttribute(Qt::WA_StyledBackground, false);
    title->setStyleSheet(QStringLiteral(
        "font-size:14px; font-weight:600; background:transparent; border:none;"));
    top->addWidget(title);

    top->addStretch(1);

    m_analysisTabs = new SegmentedControl(panel);
    m_analysisTabs->setItems({ tr("时间线"), tr("检测"), tr("字幕") });
    m_analysisTabs->setFixedWidth(260);
    if (m_theme) m_analysisTabs->setThemeService(m_theme);
    top->addWidget(m_analysisTabs);

    v->addLayout(top);

    // 中间：StackedLayout 承载三个 Tab 页
    m_analysisStack = new QStackedLayout();
    m_analysisStack->setContentsMargins(0, 0, 0, 0);
    // 三个占位内容
    m_analysisStack->addWidget(buildAnalysisTabContent(panel, m_theme, 0));
    m_analysisStack->addWidget(buildAnalysisTabContent(panel, m_theme, 1));
    m_analysisStack->addWidget(buildAnalysisTabContent(panel, m_theme, 2));

    auto* stackHost = new QWidget(panel);
    stackHost->setAttribute(Qt::WA_StyledBackground, false);
    stackHost->setLayout(m_analysisStack);
    v->addWidget(stackHost, 1);

    connect(m_analysisTabs, &SegmentedControl::currentChanged,
            this, [this](int idx) {
                if (m_analysisStack) m_analysisStack->setCurrentIndex(idx);
            });

    return panel;
}

QWidget* MainWindow::buildFilePage()
{
    m_fileListView = new FileListView(this);
    connect(m_fileListView, &FileListView::openRequested,
            this, &MainWindow::onOpenVideoPath);
    return m_fileListView;
}

QWidget* MainWindow::buildKnowledgePage()
{
    auto* page = new QWidget(this);
    page->setAttribute(Qt::WA_StyledBackground, true);
    auto* v = new QVBoxLayout(page);
    v->setAlignment(Qt::AlignCenter);
    v->setSpacing(12);

    auto* icon = new QLabel(page);
    icon->setPixmap(QIcon(QStringLiteral(":/icons/knowledge.svg")).pixmap(64, 64));
    icon->setAlignment(Qt::AlignCenter);
    icon->setStyleSheet(QStringLiteral("background:transparent;"));
    auto* title = new QLabel(tr("知识库"), page);
    title->setAlignment(Qt::AlignCenter);
    title->setStyleSheet(QStringLiteral(
        "font-size:16px; font-weight:600; background:transparent;"));
    auto* hint = new QLabel(tr("知识库能力将在后续版本中开放"), page);
    hint->setAlignment(Qt::AlignCenter);
    hint->setStyleSheet(QStringLiteral(
        "color:#8B8B8B; font-size:13px; background:transparent;"));

    v->addWidget(icon);
    v->addWidget(title);
    v->addWidget(hint);
    return page;
}

void MainWindow::onOpenSettings()
{
    if (!m_settings) return;

    auto* dlg = new SettingsDialog(m_theme, m_settings, m_agent, m_providers, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose);

    // 连接设置对话框的打开视频信号
    connect(dlg, &SettingsDialog::openVideoRequested,
            this, &MainWindow::onOpenVideoPath);

    dlg->exec();
}

void MainWindow::onThemeChanged(bool /*isDark*/)
{
    // 暂停整个主窗口更新，避免逐个 widget 刷新带来的闪烁和卡顿
    setUpdatesEnabled(false);

    applyPageBackground();

    // 重建 Analysis Tab 内容（占位配色刷新）
    if (m_analysisStack) {
        const int prevIdx = m_analysisTabs ? m_analysisTabs->currentIndex() : 0;
        while (m_analysisStack->count() > 0) {
            QWidget* w = m_analysisStack->widget(0);
            m_analysisStack->removeWidget(w);
            delete w;  // 直接 delete，不用 deleteLater（已从布局移除）
        }
        m_analysisStack->addWidget(buildAnalysisTabContent(nullptr, m_theme, 0));
        m_analysisStack->addWidget(buildAnalysisTabContent(nullptr, m_theme, 1));
        m_analysisStack->addWidget(buildAnalysisTabContent(nullptr, m_theme, 2));
        m_analysisStack->setCurrentIndex(prevIdx);
    }

    setUpdatesEnabled(true);
}

void MainWindow::applyPageBackground()
{
    if (!m_theme) return;
    const QString bg = m_theme->color(QStringLiteral("background")).name();
    const QString text = m_theme->color(QStringLiteral("textPrimary")).name();

    if (m_chatPage) {
        m_chatPage->setStyleSheet(QString("background:%1; color:%2;").arg(bg, text));
    }
    if (m_pageStack) {
        m_pageStack->setStyleSheet(QString("QStackedWidget { background:%1; }").arg(bg));
    }
}

void MainWindow::onNavRequested(int index)
{
    if (!m_pageStack) return;
    if (index < 0 || index >= m_pageStack->count()) return;
    m_pageStack->setCurrentIndex(index);
}

void MainWindow::onOpenVideo()
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("打开视频"), dir,
        tr("视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.ts *.webm);;所有文件 (*.*)"));
    if (path.isEmpty()) return;
    onOpenVideoPath(path);
}

void MainWindow::onOpenVideoPath(const QString& path)
{
    if (path.isEmpty()) return;
    if (m_playerVM) m_playerVM->openFile(path);
    if (m_fileService) m_fileService->addToRecent(path);
    setWindowTitle(QStringLiteral("Frame Mind - ") + QFileInfo(path).fileName());

    if (m_pageStack && m_pageStack->currentIndex() != 0) {
        m_pageStack->setCurrentIndex(0);
    }
}

void MainWindow::onPlayerFullscreenChanged(bool fullscreen)
{
    if (!fullscreen && m_playerView) {
        // 退出全屏后，确保 PlayerView 重新归位到播放器面板布局中
        if (m_playerPanel && m_playerPanel->layout()) {
            auto* layout = qobject_cast<QVBoxLayout*>(m_playerPanel->layout());
            if (layout && layout->indexOf(m_playerView) < 0) {
                layout->addWidget(m_playerView, 1);
            }
        }
        m_playerView->show();
    }
}

void MainWindow::resizeEvent(QResizeEvent* event)
{
    QMainWindow::resizeEvent(event);

    if (m_resizeGuard > 0) return;
    ++m_resizeGuard;

    // 比例分配：左侧播放器+分析占 60%，右侧聊天占 40%
    if (!m_leftContainer || !m_chatView) {
        --m_resizeGuard;
        return;
    }

    int w = event->size().width();
    const int pad = 16;
    const int gap = 12;
    const int availW = w - 2 * pad - gap;
    const int leftW = qRound(availW * 0.6);
    const int chatW = availW - leftW;

    const int chatMinW = 340;
    const int leftMinW = 200;
    if (leftW < leftMinW || chatW < chatMinW) {
        --m_resizeGuard;
        return;
    }

    // 只设置最小宽度，高度由布局自适应
    m_analysisPanel->setMinimumWidth(leftW - 16); // 留出边距

    --m_resizeGuard;
}
