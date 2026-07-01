#ifndef FRAMEMIND_MAINWINDOW_H
#define FRAMEMIND_MAINWINDOW_H

#include <QMainWindow>
#include <QColor>

class SidebarView;
class PlayerView;
class ChatView;
class FileListView;
class ThemedPanel;
class SegmentedControl;
class CustomTitleBar;
class SettingsDialog;
class PlayerViewModel;
class ChatViewModel;
class FileListViewModel;
class SettingsService;
class AgentService;
class FileManagerService;
class ThemeService;
class QStackedWidget;
class QStackedLayout;
class QActionGroup;

/**
 * 主窗口：顶部自定义标题栏 + 左侧导航栏 + 页面容器（QStackedWidget）。
 * 页面路由：
 *   0 → 对话页（播放器 + Analysis 面板 + ChatView）
 *   1 → 文件列表页（缩略图网格）
 *   2 → 知识库页（占位）
 */
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    MainWindow(PlayerViewModel* playerVM,
               ChatViewModel* chatVM,
               FileListViewModel* fileListVM,
               SettingsService* settings,
               AgentService* agent,
               FileManagerService* fileService,
               ThemeService* theme,
               QWidget* parent = nullptr);

private slots:
    void onOpenVideo();
    void onOpenVideoPath(const QString& path);
    void onNavRequested(int index);
    void onThemeChanged(bool isDark);
    void onOpenSettings();

private:
    QWidget* buildChatPage();
    QWidget* buildFilePage();
    QWidget* buildKnowledgePage();
    ThemedPanel* buildAnalysisPanel(QWidget* parent);
    void applyPageBackground();
    void resizeEvent(QResizeEvent* event) override;

    int  m_resizeGuard = 0;  // 嵌套 resizeEvent 递归保护

    CustomTitleBar*   m_titleBar = nullptr;
    SidebarView*      m_sidebar = nullptr;
    QStackedWidget*   m_pageStack = nullptr;
    PlayerView*       m_playerView = nullptr;
    ChatView*         m_chatView = nullptr;
    FileListView*     m_fileListView = nullptr;

    // 对话页容器
    ThemedPanel*      m_playerPanel = nullptr;
    ThemedPanel*      m_analysisPanel = nullptr;
    SegmentedControl* m_analysisTabs = nullptr;
    QStackedLayout*   m_analysisStack = nullptr;
    QWidget*          m_chatPage = nullptr;
    QWidget*          m_leftContainer = nullptr;

    // 主题菜单
    QActionGroup*    m_themeGroup = nullptr;

    PlayerViewModel*    m_playerVM = nullptr;
    ChatViewModel*      m_chatVM = nullptr;
    FileListViewModel*  m_fileListVM = nullptr;
    SettingsService*     m_settings = nullptr;
    AgentService*       m_agent = nullptr;
    FileManagerService* m_fileService = nullptr;
    ThemeService*       m_theme = nullptr;
};

#endif // FRAMEMIND_MAINWINDOW_H
