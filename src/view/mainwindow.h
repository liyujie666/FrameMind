#ifndef FRAMEMIND_MAINWINDOW_H
#define FRAMEMIND_MAINWINDOW_H

#include <QMainWindow>

class SidebarView;
class PlayerView;
class ChatView;
class PlayerViewModel;
class ChatViewModel;
class SettingsService;
class AgentService;
class QStackedWidget;

/**
 * 主窗口：左侧导航栏 + 页面容器（QStackedWidget）。
 * 对话页内部用 QSplitter 横向分割：左播放器+下方面板，右 ChatView。
 */
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(PlayerViewModel* playerVM,
               ChatViewModel* chatVM,
               SettingsService* settings,
               AgentService* agent,
               QWidget* parent = nullptr);

private slots:
    void onOpenVideo();
    void onAiSettings();

private:
    QWidget* buildChatPage();
    void buildMenu();

    SidebarView*     m_sidebar = nullptr;
    QStackedWidget*  m_pageStack = nullptr;
    PlayerView*      m_playerView = nullptr;
    ChatView*        m_chatView = nullptr;

    PlayerViewModel* m_playerVM = nullptr;
    ChatViewModel*   m_chatVM = nullptr;
    SettingsService* m_settings = nullptr;
    AgentService*    m_agent = nullptr;
};

#endif // FRAMEMIND_MAINWINDOW_H
