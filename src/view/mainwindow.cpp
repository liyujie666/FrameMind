#include "view/mainwindow.h"

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
                       QWidget* parent)
    : QMainWindow(parent)
    , m_playerVM(playerVM)
    , m_chatVM(chatVM)
    , m_fileListVM(fileListVM)
    , m_settings(settings)
    , m_agent(agent)
    , m_fileService(fileService)
    , m_theme(theme)
{
    setWindowTitle(QStringLiteral("Frame Mind"));
    resize(1360, 820);
    setMinimumSize(1100, 700);

    auto* central = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_sidebar = new SidebarView(central);
    if (m_theme) m_sidebar->setThemeService(m_theme);

    m_pageStack = new QStackedWidget(central);
    m_pageStack->addWidget(buildChatPage());      // index 0：对话页
    m_pageStack->addWidget(buildFilePage());      // index 1：文件列表页
    m_pageStack->addWidget(buildKnowledgePage()); // index 2：知识库页（占位）

    rootLayout->addWidget(m_sidebar);
    rootLayout->addWidget(m_pageStack, 1);
    setCentralWidget(central);

    buildMenu();

    if (m_playerView && m_playerVM) {
        m_playerView->setViewModel(m_playerVM);
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
    // 页面底色由 onThemeChanged 应用

    auto* pageLayout = new QHBoxLayout(m_chatPage);
    pageLayout->setContentsMargins(16, 16, 16, 16);
    pageLayout->setSpacing(12);

    // ---- 左侧列（播放器容器 + 分析容器）----
    auto* left = new QWidget(m_chatPage);
    left->setAttribute(Qt::WA_StyledBackground, false);
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(12);

    // 播放器容器（ThemedPanel 圆角卡片，内嵌 PlayerView）
    m_playerPanel = new ThemedPanel(left);
    m_playerPanel->setRadius(16);
    if (m_theme) m_playerPanel->setThemeService(m_theme);

    auto* playerPanelLayout = new QVBoxLayout(m_playerPanel);
    playerPanelLayout->setContentsMargins(16, 16, 16, 16);
    playerPanelLayout->setAlignment(Qt::AlignCenter);

    m_playerView = new PlayerView(m_playerPanel);
    if (m_theme) m_playerView->setThemeService(m_theme);
    playerPanelLayout->addWidget(m_playerView, 0, Qt::AlignHCenter | Qt::AlignTop);

    // 分析容器
    m_analysisPanel = buildAnalysisPanel(left);

    leftLayout->addWidget(m_playerPanel, 1);
    leftLayout->addWidget(m_analysisPanel, 0);

    // ---- 右侧：ChatView 圆角卡片 ----
    m_chatView = new ChatView(m_chatPage);
    m_chatView->setMinimumWidth(340);
    m_chatView->setMaximumWidth(520);

    pageLayout->addWidget(left, 3);
    pageLayout->addWidget(m_chatView, 2);

    return m_chatPage;
}

QWidget* MainWindow::buildAnalysisPanel(QWidget* parent)
{
    auto* panel = new ThemedPanel(parent);
    panel->setRadius(16);
    panel->setMinimumHeight(200);
    panel->setMaximumHeight(240);
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

void MainWindow::buildMenu()
{
    auto* fileMenu = menuBar()->addMenu(tr("文件"));

    auto* openAction = fileMenu->addAction(tr("打开视频..."));
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::onOpenVideo);

    auto* aiAction = fileMenu->addAction(tr("AI 设置..."));
    connect(aiAction, &QAction::triggered, this, &MainWindow::onAiSettings);

    fileMenu->addSeparator();
    auto* quitAction = fileMenu->addAction(tr("退出"));
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);

    // 外观菜单：主题切换
    auto* viewMenu = menuBar()->addMenu(tr("外观"));
    m_themeGroup = new QActionGroup(this);
    m_themeGroup->setExclusive(true);

    struct Item { const char* text; int mode; };
    const Item items[] = {
        { QT_TR_NOOP("跟随系统"), 0 },
        { QT_TR_NOOP("亮色"),      1 },
        { QT_TR_NOOP("暗色"),      2 },
    };
    for (const auto& it : items) {
        auto* act = viewMenu->addAction(tr(it.text));
        act->setCheckable(true);
        m_themeGroup->addAction(act);
        const int mode = it.mode;
        connect(act, &QAction::triggered, this,
                [this, mode]() { onSelectThemeMode(mode); });
        if (m_theme) {
            const int cur = static_cast<int>(m_theme->themeMode());
            if (cur == mode) act->setChecked(true);
        } else if (mode == 2) {
            act->setChecked(true);
        }
    }
}

void MainWindow::onSelectThemeMode(int mode)
{
    if (!m_theme) return;
    m_theme->setThemeMode(static_cast<ThemeService::ThemeMode>(mode));
}

void MainWindow::onThemeChanged(bool /*isDark*/)
{
    applyPageBackground();

    // 同步 QActionGroup 勾选态（例如通过 API 切换主题时）
    if (m_themeGroup && m_theme) {
        const int cur = static_cast<int>(m_theme->themeMode());
        const auto actions = m_themeGroup->actions();
        for (int i = 0; i < actions.size(); ++i) {
            actions[i]->setChecked(i == cur);
        }
    }

    // 重建 Analysis Tab 内容（简单粗暴刷新占位配色）
    if (m_analysisStack) {
        while (m_analysisStack->count() > 0) {
            QWidget* w = m_analysisStack->widget(0);
            m_analysisStack->removeWidget(w);
            w->deleteLater();
        }
        m_analysisStack->addWidget(buildAnalysisTabContent(nullptr, m_theme, 0));
        m_analysisStack->addWidget(buildAnalysisTabContent(nullptr, m_theme, 1));
        m_analysisStack->addWidget(buildAnalysisTabContent(nullptr, m_theme, 2));
        if (m_analysisTabs) m_analysisStack->setCurrentIndex(m_analysisTabs->currentIndex());
    }
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

void MainWindow::onAiSettings()
{
    if (!m_settings) return;

    QDialog dlg(this);
    dlg.setWindowTitle(tr("AI 设置"));
    dlg.setMinimumWidth(440);

    auto* form = new QFormLayout(&dlg);
    auto* endpointEdit = new QLineEdit(&dlg);
    endpointEdit->setText(m_settings->get(QStringLiteral("llm.endpoint"),
                                          QStringLiteral("https://api.openai.com/v1")));
    auto* modelEdit = new QLineEdit(&dlg);
    modelEdit->setText(m_settings->get(QStringLiteral("llm.model"),
                                       QStringLiteral("gpt-4o")));
    auto* keyEdit = new QLineEdit(&dlg);
    keyEdit->setEchoMode(QLineEdit::Password);
    keyEdit->setText(m_settings->secretGet(QStringLiteral("secret.llm.api_key")));
    keyEdit->setPlaceholderText(tr("sk-...（仅存于系统密钥库，不入数据库）"));

    form->addRow(tr("Endpoint"), endpointEdit);
    form->addRow(tr("模型"), modelEdit);
    form->addRow(tr("API Key"), keyEdit);

    auto* buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    const QString endpoint = endpointEdit->text().trimmed();
    const QString model = modelEdit->text().trimmed();
    m_settings->set(QStringLiteral("llm.endpoint"), endpoint);
    m_settings->set(QStringLiteral("llm.model"), model);
    m_settings->secretSet(QStringLiteral("secret.llm.api_key"),
                          keyEdit->text().trimmed());

    if (m_agent) {
        m_agent->setEndpoint(endpoint);
        m_agent->setModel(model);
    }
}
