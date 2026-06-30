#include "view/mainwindow.h"

#include "view/sidebar/sidebarview.h"
#include "view/player/playerview.h"
#include "view/chat/chatview.h"
#include "viewmodel/playerviewmodel.h"
#include "viewmodel/chatviewmodel.h"
#include "service/settingsservice.h"
#include "service/agentservice.h"

#include <QStackedWidget>
#include <QSplitter>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QWidget>
#include <QTabWidget>
#include <QLabel>
#include <QLineEdit>
#include <QDialog>
#include <QDialogButtonBox>
#include <QMenuBar>
#include <QMenu>
#include <QFileDialog>
#include <QStandardPaths>
#include <QFileInfo>

MainWindow::MainWindow(PlayerViewModel* playerVM,
                       ChatViewModel* chatVM,
                       SettingsService* settings,
                       AgentService* agent,
                       QWidget* parent)
    : QMainWindow(parent)
    , m_playerVM(playerVM)
    , m_chatVM(chatVM)
    , m_settings(settings)
    , m_agent(agent)
{
    setWindowTitle(QStringLiteral("Frame Mind"));
    resize(1280, 760);

    auto* central = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);

    m_sidebar = new SidebarView(central);
    m_pageStack = new QStackedWidget(central);
    m_pageStack->addWidget(buildChatPage());   // index 0：对话页

    rootLayout->addWidget(m_sidebar);
    rootLayout->addWidget(m_pageStack, 1);
    setCentralWidget(central);

    buildMenu();

    if (m_playerView && m_playerVM) {
        m_playerView->setViewModel(m_playerVM);
    }
    if (m_chatView && m_chatVM) {
        m_chatView->setViewModel(m_chatVM);
    }
}

QWidget* MainWindow::buildChatPage()
{
    auto* page = new QWidget(this);
    page->setStyleSheet(QStringLiteral("background:#0D1117;"));  // Dark.Background
    auto* pageLayout = new QHBoxLayout(page);
    pageLayout->setContentsMargins(0, 0, 0, 0);
    pageLayout->setSpacing(0);

    auto* splitter = new QSplitter(Qt::Horizontal, page);

    // ---- 左侧：播放器 + 下方面板占位 ----
    auto* left = new QWidget(splitter);
    left->setStyleSheet(QStringLiteral("background:#0D1117;"));
    auto* leftLayout = new QVBoxLayout(left);
    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(0);

    m_playerView = new PlayerView(left);
    m_playerView->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);

    auto* analysisTabs = new QTabWidget(left);
    analysisTabs->setStyleSheet(QStringLiteral(
        "QTabWidget { background:#1E1E2E; border-top:1px solid #2D2D3D; }"
        "QTabBar::tab { background:#1E1E2E; color:#8B8B8B; padding:8px 16px; }"
        "QTabBar::tab:selected { color:#2979FF; border-bottom:2px solid #2979FF; }"));
    analysisTabs->addTab(new QWidget(analysisTabs), tr("时间线"));
    analysisTabs->addTab(new QWidget(analysisTabs), tr("检测"));
    analysisTabs->addTab(new QWidget(analysisTabs), tr("字幕"));
    analysisTabs->setMaximumHeight(180);

    leftLayout->addWidget(m_playerView, 1);
    leftLayout->addWidget(analysisTabs, 0);

    // ---- 右侧：ChatView ----
    m_chatView = new ChatView(splitter);
    m_chatView->setMinimumWidth(320);
    m_chatView->setMaximumWidth(500);  // 限制最大宽度
    m_chatView->setStyleSheet(QStringLiteral("background:#0D1117;"));

    splitter->addWidget(left);
    splitter->addWidget(m_chatView);
    splitter->setStretchFactor(0, 3);  // 左侧占 3 份
    splitter->setStretchFactor(1, 2);  // 右侧占 2 份
    splitter->setSizes({ 750, 500 });
    splitter->setChildrenCollapsible(false);

    pageLayout->addWidget(splitter);
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
}

void MainWindow::onOpenVideo()
{
    const QString dir =
        QStandardPaths::writableLocation(QStandardPaths::MoviesLocation);
    const QString path = QFileDialog::getOpenFileName(
        this, tr("打开视频"), dir,
        tr("视频文件 (*.mp4 *.mkv *.avi *.mov *.flv *.ts *.webm);;所有文件 (*.*)"));
    if (path.isEmpty()) return;

    if (m_playerVM) m_playerVM->openFile(path);
    setWindowTitle(QStringLiteral("Frame Mind - ") + QFileInfo(path).fileName());
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
