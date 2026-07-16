#ifndef FRAMEMIND_MAINWINDOW_H
#define FRAMEMIND_MAINWINDOW_H

#include <QMainWindow>
#include <QColor>
#include <QToolButton>

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
class LLMProviderService;
class QStackedWidget;
class QStackedLayout;
class QSplitter;
class QActionGroup;

/**
 * 主窗口：顶部自定义标题栏 + 左侧导航栏 + 页面容器（QStackedWidget）。
 * 页面路由：
 *   0 → 对话页（播放器 + Analysis 面板 + ChatView）
 *   1 → 文件列表页（缩略图网格）
 *   2 → 知识库页（占位）
 *
 * 无边框窗口，通过 mouseMoveEvent / nativeEvent 实现边缘拖拽 resize。
 * 左右面板通过 QSplitter 分隔，支持鼠标拖动调整 ChatView 宽度。
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
               LLMProviderService* providers,
               QWidget* parent = nullptr);

private slots:
    void onOpenVideo();
    void onOpenVideoPath(const QString& path);
    void onNavRequested(int index);
    void onThemeChanged(bool isDark);
    void onOpenSettings();
    void onPlayerFullscreenChanged(bool fullscreen);
    void onCollapseChatPanel();
    void onExpandChatPanel();
    void onChatPanelToggled(bool visible);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;

private:
    QWidget* buildChatPage();
    QWidget* buildFilePage();
    QWidget* buildKnowledgePage();
    ThemedPanel* buildAnalysisPanel(QWidget* parent);
    void applyPageBackground();
    void updateSplitterStyle();

    // Edge resize helpers
    enum ResizeEdge { None = 0, Left=1, Right=2, Top=4, Bottom=8,
                      TopLeft=5, TopRight=6, BottomLeft=9, BottomRight=10 };
    ResizeEdge edgeAt(const QPoint& globalPos) const;
    static Qt::CursorShape cursorForEdge(ResizeEdge edge);

    static constexpr int kResizeBorder = 6;

    CustomTitleBar*   m_titleBar = nullptr;
    SidebarView*      m_sidebar = nullptr;
    QStackedWidget*   m_pageStack = nullptr;
    PlayerView*       m_playerView = nullptr;
    ChatView*         m_chatView = nullptr;
    FileListView*     m_fileListView = nullptr;

    // 对话页容器
    QSplitter*        m_mainSplitter = nullptr;
    ThemedPanel*      m_playerPanel = nullptr;
    ThemedPanel*      m_analysisPanel = nullptr;
    SegmentedControl* m_analysisTabs = nullptr;
    QStackedLayout*   m_analysisStack = nullptr;
    QWidget*          m_chatPage = nullptr;
    QWidget*          m_leftContainer = nullptr;
    QSplitter*        m_leftSplitter = nullptr;

    // Edge resize state
    bool        m_resizing = false;
    ResizeEdge  m_resizeEdge = None;
    QPoint      m_resizeStartGlobal;
    QRect       m_resizeStartGeometry;

    PlayerViewModel*    m_playerVM = nullptr;
    ChatViewModel*      m_chatVM = nullptr;
    FileListViewModel*  m_fileListVM = nullptr;
    SettingsService*    m_settings = nullptr;
    AgentService*       m_agent = nullptr;
    FileManagerService* m_fileService = nullptr;
    ThemeService*       m_theme = nullptr;
    LLMProviderService* m_providers = nullptr;
};

#endif // FRAMEMIND_MAINWINDOW_H
