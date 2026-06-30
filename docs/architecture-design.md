# 视频分析 AI Agent 客户端架构设计

## 一、概述

### 1.1 项目定位

基于 Qt6.9 + SmartPlayer SDK 构建的视频分析 AI 智能体客户端，支持视频播放、多模态 AI 对话、视频内容理解与交互式分析。

### 1.2 技术栈

| 层面 | 技术选型 |
|------|---------|
| UI 框架 | Qt 6.9.0 (Widgets + 部分 QML) |
| 架构模式 | MVVM (Model-View-ViewModel) |
| 视频播放 | SmartPlayer SDK (FFmpeg + SDL2) |
| AI 通信 | HTTP/WebSocket (OpenAI Compatible API) |
| 数据持久化 | SQLite (Qt SQL) |
| 配置管理 | QSettings + JSON |
| 构建系统 | CMake 3.20+ |
| 语言标准 | C++17 |

### 1.3 设计原则

- **关注点分离**：View 不包含业务逻辑，ViewModel 不依赖具体 View
- **可测试性**：ViewModel 和 Model 可脱离 UI 独立单元测试
- **响应式绑定**：通过 Qt 信号槽 + Q_PROPERTY 实现数据驱动 UI
- **插件化**：AI 后端、视频分析能力可热插拔
- **跨平台**：Windows / macOS / Linux

---

## 二、整体架构图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                            Application Layer                             │
│  main.cpp → App 初始化 → 依赖注入容器 → 主窗口启动                         │
├─────────────────────────────────────────────────────────────────────────┤
│                                                                         │
│  ┌─── View Layer ───────────────────────────────────────────────────┐   │
│  │                                                                   │   │
│  │  MainWindow                                                       │   │
│  │  ├── SidebarView (导航栏)                                         │   │
│  │  ├── PlayerView (视频播放器 + 控制栏)                              │   │
│  │  ├── AnalysisPanelView (时间线 / 检测 / 字幕)                     │   │
│  │  ├── ChatView (AI 对话面板)                                       │   │
│  │  ├── FileListView (本地文件管理)                                   │   │
│  │  └── SettingsView (设置页)                                        │   │
│  │                                                                   │   │
│  └───────────────── ▲ binds (Q_PROPERTY / signal-slot) ──────────────┘   │
│                     │                                                     │
│  ┌─── ViewModel Layer ──────────────────────────────────────────────┐   │
│  │                     │                                             │   │
│  │  ├── PlayerViewModel        ← 播放状态/进度/控制命令               │   │
│  │  ├── ChatViewModel          ← 消息列表/发送/流式接收               │   │
│  │  ├── AnalysisViewModel      ← 分析结果/时间线/检测数据             │   │
│  │  ├── FileListViewModel      ← 文件列表/搜索/排序                  │   │
│  │  ├── SettingsViewModel      ← 配置项/主题切换                     │   │
│  │  └── NavigationViewModel    ← 页面路由/导航状态                   │   │
│  │                                                                   │   │
│  └───────────────── ▲ calls / observes ──────────────────────────────┘   │
│                     │                                                     │
│  ┌─── Model Layer (Domain + Service) ───────────────────────────────┐   │
│  │                                                                   │   │
│  │  ┌─ Domain Models ─┐  ┌─ Services ──────────────────────────┐    │   │
│  │  │ ChatMessage      │  │ AgentService (AI 交互核心)          │    │   │
│  │  │ VideoInfo        │  │ PlayerService (SmartPlayer 封装)    │    │   │
│  │  │ AnalysisResult   │  │ VideoAnalysisService (帧分析调度)   │    │   │
│  │  │ TimelineEvent    │  │ FileManagerService (文件管理)       │    │   │
│  │  │ DetectionItem    │  │ ThemeService (主题管理)             │    │   │
│  │  │ UserSettings     │  │ SettingsService (配置持久化)        │    │   │
│  │  │ Conversation     │  │ ConversationService (对话持久化)    │    │   │
│  │  └─────────────────┘  └─────────────────────────────────────┘    │   │
│  │                                                                   │   │
│  └───────────────── ▲ depends ───────────────────────────────────────┘   │
│                     │                                                     │
│  ┌─── Infrastructure Layer ─────────────────────────────────────────┐   │
│  │                                                                   │   │
│  │  ├── NetworkClient (HTTP/WebSocket/SSE 通信)                      │   │
│  │  ├── DatabaseManager (SQLite 数据库)                              │   │
│  │  ├── SmartPlayer SDK (视频播放引擎)                               │   │
│  │  ├── ImageProcessor (帧数据 → QImage 转换)                       │   │
│  │  ├── Logger (日志系统)                                            │   │
│  │  └── EventBus (跨模块事件总线)                                    │   │
│  │                                                                   │   │
│  └───────────────────────────────────────────────────────────────────┘   │
│                                                                         │
└─────────────────────────────────────────────────────────────────────────┘
```

---

## 三、MVVM 各层详细设计

### 3.1 View Layer（视图层）

**职责**：UI 呈现 + 用户交互事件转发，**不含任何业务逻辑**。

#### 类设计

```cpp
// === MainWindow ===
class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget* parent = nullptr);
private:
    SidebarView*       m_sidebar;
    QStackedWidget*    m_pageStack;      // 页面容器（对话页/文件页/设置页）
    // 对话页内部
    PlayerView*        m_playerView;
    AnalysisPanelView* m_analysisPanel;
    ChatView*          m_chatView;
};

// === PlayerView ===
// 负责视频渲染 + 控制栏 UI
class PlayerView : public QWidget {
    Q_OBJECT
public:
    void setViewModel(PlayerViewModel* vm);
private:
    VideoRenderWidget* m_renderWidget;   // 自定义绘制 widget
    PlayerControlBar*  m_controlBar;     // 进度条/按钮/音量/倍速
    PlayerViewModel*   m_vm = nullptr;
    void bindViewModel();                // 建立双向绑定
};

// === ChatView ===
// 负责对话 UI 展示
class ChatView : public QWidget {
    Q_OBJECT
public:
    void setViewModel(ChatViewModel* vm);
private:
    ChatMessageList*  m_messageList;     // 可滚动消息列表
    ChatInputWidget*  m_inputWidget;     // 输入框+快捷按钮
    QToolButton*      m_collapseBtn;     // 折叠按钮
    ChatViewModel*    m_vm = nullptr;
};
```

#### View 绑定原则

```cpp
void PlayerView::bindViewModel() {
    // ViewModel → View（数据驱动UI）
    connect(m_vm, &PlayerViewModel::positionChanged,
            m_controlBar, &PlayerControlBar::setPosition);
    connect(m_vm, &PlayerViewModel::durationChanged,
            m_controlBar, &PlayerControlBar::setDuration);
    connect(m_vm, &PlayerViewModel::stateChanged,
            m_controlBar, &PlayerControlBar::setPlayState);
    connect(m_vm, &PlayerViewModel::frameReady,
            m_renderWidget, &VideoRenderWidget::updateFrame);

    // View → ViewModel（用户操作转命令）
    connect(m_controlBar, &PlayerControlBar::seekRequested,
            m_vm, &PlayerViewModel::seek);
    connect(m_controlBar, &PlayerControlBar::playClicked,
            m_vm, &PlayerViewModel::togglePlay);
    connect(m_controlBar, &PlayerControlBar::volumeChanged,
            m_vm, &PlayerViewModel::setVolume);
}
```

---

### 3.2 ViewModel Layer（视图模型层）

**职责**：持有 UI 状态，协调 Service 调用，暴露可绑定属性和命令。

#### PlayerViewModel

```cpp
class PlayerViewModel : public QObject {
    Q_OBJECT
    // 可绑定属性（数据驱动UI）
    Q_PROPERTY(int64_t position READ position NOTIFY positionChanged)
    Q_PROPERTY(int64_t duration READ duration NOTIFY durationChanged)
    Q_PROPERTY(PlayerState state READ state NOTIFY stateChanged)
    Q_PROPERTY(int volume READ volume WRITE setVolume NOTIFY volumeChanged)
    Q_PROPERTY(float speed READ speed WRITE setSpeed NOTIFY speedChanged)
    Q_PROPERTY(bool muted READ muted WRITE setMute NOTIFY mutedChanged)
    Q_PROPERTY(QString mediaTitle READ mediaTitle NOTIFY mediaTitleChanged)

public:
    // eventBus: 监听跨 VM 事件（如 ChatVM 发出的 seekToPosition / captureFrameForAI）
    explicit PlayerViewModel(PlayerService* playerService,
                             VideoAnalysisService* analysisService,
                             EventBus* eventBus,
                             QObject* parent = nullptr);

    // 查询属性
    int64_t position() const;
    int64_t duration() const;
    PlayerState state() const;
    int volume() const;
    float speed() const;
    bool muted() const;
    QString mediaTitle() const;

public slots:
    // 命令（View 调用）
    void openFile(const QString& filePath);
    void togglePlay();
    void seek(int64_t posMs);
    void setVolume(int vol);
    void setSpeed(float speed);
    void setMute(bool mute);
    void takeScreenshot();                    // 截图到文件
    void captureFrameForAI();                // 截取当前帧（用 PlayerService::lastDecodedFrame）发给 AI
    void captureFrameAtForAI(int64_t posMs); // AI 工具 seek_and_analyze：异步截指定时间点帧
    void seekToTimestamp(int64_t posMs);     // AI 回复中点击时间戳跳转

signals:
    void positionChanged(int64_t posMs);
    void durationChanged(int64_t durationMs);
    void stateChanged(PlayerState state);
    void volumeChanged(int vol);
    void speedChanged(float speed);
    void mutedChanged(bool muted);
    void mediaTitleChanged(const QString& title);
    void frameReady(const QImage& frame);    // 渲染帧就绪
    void screenshotTaken(const QString& path);
    void errorOccurred(const QString& msg);

private:
    PlayerService* m_playerService;
    VideoAnalysisService* m_analysisService;
};
```

#### ChatViewModel

```cpp
class ChatViewModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isStreaming READ isStreaming NOTIFY streamingChanged)
    Q_PROPERTY(bool isCollapsed READ isCollapsed WRITE setCollapsed NOTIFY collapsedChanged)
    Q_PROPERTY(QString currentConversationId READ currentConversationId NOTIFY conversationChanged)

public:
    // 通信原则：ChatViewModel 不直接持有 PlayerViewModel，所有跨 VM 通信走 EventBus。
    // 例如点击 AI 回复中的时间戳 → emit EventBus::seekToPosition(ms)，PlayerVM 监听响应。
    explicit ChatViewModel(AgentService* agentService,
                           ConversationService* convService,
                           EventBus* eventBus,
                           QObject* parent = nullptr);

    // 消息模型（供 View 的 ListView 使用）
    ChatMessageListModel* messageModel() const;

    bool isStreaming() const;
    bool isCollapsed() const;
    QString currentConversationId() const;

public slots:
    // 命令
    void sendMessage(const QString& text);
    void sendMessageWithFrame(const QString& text, const QImage& frame);
    void sendMessageWithTimeRange(const QString& text, int64_t startMs, int64_t endMs);
    void stopGeneration();                   // 中止生成
    void regenerateLastResponse();           // 重新生成
    void setCollapsed(bool collapsed);

    // 对话管理
    void createNewConversation();
    void switchConversation(const QString& convId);
    void deleteConversation(const QString& convId);

    // AI 回复中的交互动作
    void onTimestampClicked(int64_t posMs);  // 点击时间戳→跳转播放器

signals:
    void streamingChanged(bool streaming);
    void collapsedChanged(bool collapsed);
    void conversationChanged(const QString& convId);
    void messageAppended(int index);         // 新消息到达
    void messageUpdated(int index);          // 流式更新
    void errorOccurred(const QString& msg);

private:
    AgentService* m_agentService;
    ConversationService* m_convService;
    EventBus* m_eventBus;                    // 跨 VM 通信通道（替代直接持有 PlayerVM）
    ChatMessageListModel* m_messageModel;
    bool m_streaming = false;
    bool m_collapsed = false;
};
```

#### AnalysisViewModel

```cpp
class AnalysisViewModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(AnalysisTab currentTab READ currentTab WRITE setCurrentTab NOTIFY tabChanged)
    Q_PROPERTY(bool isAnalyzing READ isAnalyzing NOTIFY analyzingChanged)

public:
    enum AnalysisTab { Timeline, Detection, Subtitle };
    Q_ENUM(AnalysisTab)

    // 同 ChatViewModel，跨 VM 通信走 EventBus
    explicit AnalysisViewModel(VideoAnalysisService* analysisService,
                               EventBus* eventBus,
                               QObject* parent = nullptr);

    // 子模型（QAbstractListModel 派生，供 QML/QListView 绑定）
    // 详细字段映射在 docs 的 model 章节描述，这里给出最小契约：
    //   TimelineModel  : rows = TimelineEvent，roles = {startMs, endMs, label, type}
    //   DetectionModel : rows = DetectionItem，roles = {timestampMs, bbox, label, confidence}
    //   SubtitleModel  : rows = SubtitleEntry，roles = {startMs, endMs, text, speaker}
    TimelineModel* timelineModel() const;
    DetectionModel* detectionModel() const;
    SubtitleModel* subtitleModel() const;

    AnalysisTab currentTab() const;
    bool isAnalyzing() const;

public slots:
    void setCurrentTab(AnalysisTab tab);
    void startFullAnalysis();                  // 全量分析视频
    void analyzeCurrentFrame();               // 分析当前帧
    void onTimelineItemClicked(int index);    // 点击时间线项→跳转
    void onSubtitleItemClicked(int index);    // 点击字幕→跳转

signals:
    void tabChanged(AnalysisTab tab);
    void analyzingChanged(bool analyzing);
    void analysisProgress(int percent, const QString& stage);

private:
    VideoAnalysisService* m_analysisService;
    EventBus* m_eventBus;
};
```

#### NavigationViewModel

```cpp
class NavigationViewModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(PageType currentPage READ currentPage WRITE setCurrentPage NOTIFY pageChanged)

public:
    enum PageType { ChatPage, FilePage, SettingsPage };
    Q_ENUM(PageType)

public slots:
    void setCurrentPage(PageType page);

signals:
    void pageChanged(PageType page);
};
```

#### SettingsViewModel

```cpp
class SettingsViewModel : public QObject {
    Q_OBJECT
    Q_PROPERTY(ThemeMode themeMode READ themeMode WRITE setThemeMode NOTIFY themeModeChanged)
    Q_PROPERTY(QString apiEndpoint READ apiEndpoint WRITE setApiEndpoint NOTIFY apiEndpointChanged)
    Q_PROPERTY(QString modelName READ modelName WRITE setModelName NOTIFY modelNameChanged)
    Q_PROPERTY(bool hardwareDecode READ hardwareDecode WRITE setHardwareDecode NOTIFY hardwareDecodeChanged)

public:
    explicit SettingsViewModel(SettingsService* settingsService,
                               ThemeService* themeService,
                               QObject* parent = nullptr);

    enum ThemeMode { FollowSystem, Light, Dark };
    Q_ENUM(ThemeMode)

public slots:
    void setThemeMode(ThemeMode mode);
    void setApiEndpoint(const QString& endpoint);
    void setModelName(const QString& model);
    void setHardwareDecode(bool enable);
    void resetToDefaults();

signals:
    void themeModeChanged(ThemeMode mode);
    void apiEndpointChanged(const QString& endpoint);
    void modelNameChanged(const QString& model);
    void hardwareDecodeChanged(bool enable);
};
```

---

### 3.3 Model Layer（模型层）

#### 3.3.1 Domain Models（领域模型）

```cpp
// 聊天消息
struct ChatMessage {
    QString id;
    enum Role { User, Assistant, System };
    Role role;
    QString content;              // Markdown 文本
    QList<QImage> attachedFrames; // 附带的视频帧
    QDateTime timestamp;
    bool isStreaming = false;     // 正在流式生成中
};

// 对话
struct Conversation {
    QString id;
    QString title;
    QString videoFilePath;        // 关联的视频文件
    QDateTime createdAt;
    QDateTime updatedAt;
};

// 视频信息（从 SmartMediaInfo 映射）
struct VideoInfo {
    QString filePath;
    QString fileName;
    QString format;
    int64_t durationMs;
    int64_t bitRate;
    double frameRate;
    int width;
    int height;
    bool hasAudio;
};

// 时间线事件
struct TimelineEvent {
    int64_t startMs;
    int64_t endMs;
    QString label;
    QString description;
    enum Type { SceneChange, Action, Speech, Anomaly };
    Type type;
};

// 检测结果
struct DetectionItem {
    int64_t timestampMs;
    QRectF boundingBox;           // 归一化坐标 [0,1]
    QString label;
    float confidence;
};

// 字幕条目
struct SubtitleEntry {
    int64_t startMs;
    int64_t endMs;
    QString text;
    QString speaker;              // 说话人（可选）
};
```

#### 3.3.2 Services（服务层）

```cpp
// ==================== PlayerService ====================
// 封装 SmartPlayer SDK，屏蔽底层线程回调细节。
//
// 注意：SmartPlayer 通过 `setCallback(SmartPlayerCallback*)` 注册回调，
// 因此 PlayerService 内部组合一个 `CallbackBridge`（实现 SmartPlayerCallback）
// 并把事件转发为 Qt 信号；PlayerService 自身只继承 QObject，避免多重继承。
//
// 真实 SDK 的接口签名以 player_sdk/include/smartplayer*.h 为准：
//   - SmartPlayer::seek(int64_t posMs) 异步
//   - SmartPlayer::takeScreenshot(const std::string& path) 异步，结果走 onScreenshot
//   - 不提供同步截取当前帧的 API；本服务通过缓存最近一帧 + 等待新帧来实现
class PlayerService : public QObject {
    Q_OBJECT
public:
    explicit PlayerService(QObject* parent = nullptr);
    ~PlayerService() override;

    void open(const QString& filePath);
    void play();
    void pause();
    void stop();
    void seek(int64_t posMs);                 // 异步，定位完成后会有 onPositionChanged
    void setVolume(int vol);
    void setSpeed(float speed);
    void setMute(bool mute);
    void setHardwareDecode(bool enable);
    void takeScreenshot(const QString& savePath);  // 异步，完成走 screenshotReady

    /**
     * 异步截取指定时间点的帧（AI 工具 seek_and_analyze 用）。
     *
     * 行为：内部先 seek(posMs)，等待解码线程产出与目标时间戳邻近的新帧，
     * 通过 future 回传 QImage。timeoutMs 超时则 future 抛出错误。
     *
     * 为什么不是同步：SDK 的 seek 是异步的，立即取 m_lastFrame 拿到的几乎一定是旧帧。
     */
    QFuture<QImage> captureFrameAt(int64_t posMs, int timeoutMs = 2000);

    /// 同步获取当前已缓存的最近一帧（不触发 seek），可能为空 QImage
    QImage lastDecodedFrame() const;

    // 状态查询
    int64_t duration() const;
    int64_t position() const;
    PlayerState state() const;
    VideoInfo videoInfo() const;

signals:
    void positionChanged(int64_t posMs);
    void durationChanged(int64_t durationMs);          // 与 SDK onDurationChanged 对齐
    void stateChanged(PlayerState state);
    void frameDecoded(const QImage& frame);            // 跨线程信号（QueuedConnection）
    void openResult(bool success, const QString& error);
    void mediaInfoReady(const VideoInfo& info);
    void playFinished();
    void screenshotReady(const QString& path, bool success);
    void errorOccurred(const QString& msg);

private:
    // CallbackBridge 在 SDK 解码线程触发，转发为 PlayerService 的 Qt 信号
    class CallbackBridge;
    std::unique_ptr<SmartPlayer>     m_player;
    std::unique_ptr<CallbackBridge>  m_bridge;
    mutable std::mutex               m_frameMutex;
    QImage                           m_lastFrame;       // 缓存最近一帧，受 m_frameMutex 保护
};

// ==================== AgentService ====================
// AI 智能体交互核心
class AgentService : public QObject {
    Q_OBJECT
public:
    explicit AgentService(NetworkClient* network, QObject* parent = nullptr);

    // 发送对话（支持多模态：文本 + 图片帧）
    void sendMessage(const QString& conversationId,
                     const QString& text,
                     const QList<QImage>& frames = {},
                     const VideoContext& videoCtx = {});

    void stopGeneration();
    void setModel(const QString& modelName);
    void setEndpoint(const QString& endpoint);

signals:
    void responseChunk(const QString& convId, const QString& delta); // 流式片段
    void responseFinished(const QString& convId, const ChatMessage& fullMsg);
    void responseError(const QString& convId, const QString& error);
    void toolCallRequested(const QString& toolName, const QJsonObject& args);

private:
    NetworkClient* m_network;
    QString m_model;
    QString m_endpoint;
    // 构建 Agent prompt（包含视频上下文、工具定义等）
    QJsonObject buildRequestPayload(const QString& text,
                                     const QList<QImage>& frames,
                                     const VideoContext& videoCtx);
};

// ==================== VideoAnalysisService ====================
// 视频分析调度（帧采样、批量分析、结果缓存）。
// 依赖：
//   - AgentService    用于 VLM 调用（场景描述、帧描述）
//   - PlayerService   用于按时间点截帧（captureFrameAt）
//   - VideoRAGStore*  M4 加入：写入/检索向量索引（M3 阶段允许为 nullptr）
class VideoAnalysisService : public QObject {
    Q_OBJECT
public:
    explicit VideoAnalysisService(AgentService* agentService,
                                  PlayerService* playerService,
                                  VideoRAGStore* ragStore = nullptr,
                                  QObject* parent = nullptr);

    // 全视频分析（异步，后台线程执行）
    void analyzeVideo(const QString& filePath);

    // 单帧分析
    void analyzeFrame(const QImage& frame, int64_t timestampMs);

    // 获取缓存的分析结果
    QList<TimelineEvent> timelineEvents() const;
    QList<SubtitleEntry> subtitles() const;
    QList<DetectionItem> detectionsAt(int64_t timestampMs) const;

signals:
    void analysisProgress(int percent, const QString& stage);
    void analysisCompleted();
    void frameAnalysisReady(int64_t timestampMs, const AnalysisResult& result);

private:
    AgentService*   m_agentService;
    PlayerService*  m_playerService;
    VideoRAGStore*  m_ragStore;
    // 关键帧采样策略
    QList<int64_t> computeSamplingTimestamps(int64_t durationMs);
};

// ==================== ConversationService ====================
// 对话持久化
class ConversationService : public QObject {
    Q_OBJECT
public:
    QList<Conversation> getAllConversations();
    Conversation createConversation(const QString& videoPath);
    void deleteConversation(const QString& convId);
    void updateTitle(const QString& convId, const QString& title);
    QList<ChatMessage> getMessages(const QString& convId);
    void saveMessage(const QString& convId, const ChatMessage& msg);
    void updateMessage(const QString& msgId, const QString& content);

private:
    DatabaseManager* m_db;
};

// ==================== ThemeService ====================
// 主题管理统一走 ThemeService（DI 注入），不再使用早期文档里的 ThemeManager 单例。
// 这样可在测试中替换为 mock，避免全局状态。
class ThemeService : public QObject {
    Q_OBJECT
public:
    enum ThemeMode { FollowSystem, Light, Dark };
    Q_ENUM(ThemeMode)

    explicit ThemeService(SettingsService* settings, QObject* parent = nullptr);

    void setThemeMode(ThemeMode mode);
    ThemeMode themeMode() const;
    bool isDark() const;                 // 当前实际是否为暗色
    void applyTheme();                   // 加载并应用对应 QSS

    // 主题色查询（供自绘组件使用）
    QColor color(const QString& token) const;  // 如 "primary" / "textPrimary"

signals:
    void themeChanged(bool isDark);

private:
    void onSystemThemeChanged();
    SettingsService* m_settings;
    ThemeMode m_mode = FollowSystem;
};

// ==================== FileManagerService ====================
class FileManagerService : public QObject {
    Q_OBJECT
public:
    QList<VideoFileInfo> scanDirectory(const QString& dir);
    QList<VideoFileInfo> recentFiles();
    void addToRecent(const QString& filePath);
    void removeFile(const QString& filePath);
    VideoFileInfo getFileInfo(const QString& filePath);
};
```

---

### 3.4 Infrastructure Layer（基础设施层）

```cpp
// ==================== NetworkClient ====================
// HTTP + SSE 流式通信
class NetworkClient : public QObject {
    Q_OBJECT
public:
    // 普通 POST
    QNetworkReply* post(const QUrl& url, const QJsonObject& body,
                        const QMap<QString, QString>& headers = {});

    // SSE 流式请求（用于大模型流式回复）
    void streamPost(const QUrl& url, const QJsonObject& body,
                    std::function<void(const QString& chunk)> onChunk,
                    std::function<void()> onDone,
                    std::function<void(const QString& error)> onError);

    void cancelStream();

private:
    QNetworkAccessManager* m_nam;
    QNetworkReply* m_activeStream = nullptr;
};

// ==================== DatabaseManager ====================
class DatabaseManager {
public:
    static DatabaseManager* instance();
    bool initialize(const QString& dbPath);

    // 通用执行
    bool exec(const QString& sql, const QVariantList& bindings = {});
    QList<QVariantMap> query(const QString& sql, const QVariantList& bindings = {});

private:
    QSqlDatabase m_db;
    void createTables();
};

// ==================== ImageProcessor ====================
// SDK 帧数据 → QImage 转换（高性能）
class ImageProcessor {
public:
    static QImage fromVideoFrame(const uint8_t* data, int width, int height,
                                 SmartPixelFormat format);
    static QImage scaleToFit(const QImage& img, const QSize& targetSize);
    static QByteArray toBase64Jpeg(const QImage& img, int quality = 85);
    static QImage cropRegion(const QImage& img, const QRectF& normalizedRect);
};

// ==================== EventBus ====================
// 跨模块松耦合通信
class EventBus : public QObject {
    Q_OBJECT
public:
    static EventBus* instance();

    // 发布事件
    void publish(const QString& event, const QVariant& data = {});

signals:
    // 全局事件
    void videoOpened(const QString& filePath);
    void seekToPosition(int64_t posMs);       // AI 请求跳转
    void frameAnalysisRequested(int64_t posMs);
    void screenshotForAI(const QImage& frame);
};
```

---

## 四、模块交互流程

### 4.1 用户提问 + AI 回复流程

```
User Input (ChatView)
    │
    ▼
ChatView::onSendClicked()
    │  emit sendRequested(text)
    ▼
ChatViewModel::sendMessage(text)
    │  1. 判断是否附加当前帧（用户点击了📷按钮）
    │  2. 从 PlayerViewModel 获取当前帧 + 时间戳
    │  3. 构建 ChatMessage (User role)，加入 messageModel
    │  4. 调用 AgentService::sendMessage(convId, text, frames, videoCtx)
    ▼
AgentService::sendMessage()
    │  1. 构建 system prompt（含视频元信息 + 工具定义）
    │  2. 编码图片帧为 base64
    │  3. 通过 NetworkClient::streamPost() 发起 SSE 请求
    ▼
NetworkClient ──── SSE chunks ────▶ AgentService
    │                                    │
    │  onChunk(delta)                    │
    │  ◀────────────────────────────────┘
    ▼
AgentService::responseChunk(convId, delta)
    │
    ▼
ChatViewModel (收到 chunk)
    │  1. 追加到当前 AI 消息 content
    │  2. emit messageUpdated(index)
    ▼
ChatView::onMessageUpdated(index)
    │  刷新对应气泡的 Markdown 渲染
    ▼
[流式完成后]
AgentService::responseFinished(convId, fullMsg)
    │
    ▼
ChatViewModel
    │  1. 标记消息 isStreaming = false
    │  2. 保存到 ConversationService
    │  3. 解析回复中的时间戳/工具调用
    ▼
[如果 AI 请求跳转]
ChatBubbleWidget 内用正则 \[((\d{1,2}):)?\d{1,2}:\d{2}\] 匹配时间戳
    → 解析为毫秒，emit timestampClicked(posMs)
ChatViewModel::onTimestampClicked(posMs)
    → EventBus::seekToPosition(posMs)
PlayerViewModel（监听 EventBus::seekToPosition）
    → PlayerService::seek(posMs)
```

### 4.2 视频帧渲染流程

```
SmartPlayer SDK (内部解码线程)
    │  onVideoFrame(data, w, h, fmt)
    ▼
PlayerService::onVideoFrame()  [SDK 线程]
    │  1. ImageProcessor::fromVideoFrame() → QImage
    │  2. 缓存 m_lastFrame（加锁）
    │  3. emit frameDecoded(image)  [通过 Qt::QueuedConnection 跨线程]
    ▼
PlayerViewModel (主线程接收)
    │  emit frameReady(image)
    ▼
VideoRenderWidget::updateFrame(image)
    │  m_currentFrame = image
    │  update()  → 触发 paintEvent
    ▼
VideoRenderWidget::paintEvent()
    │  QPainter::drawImage() 绘制帧
```

### 4.3 主题切换流程

```
SettingsView: 用户选择 "暗色"
    │
    ▼
SettingsViewModel::setThemeMode(Dark)
    │  1. 更新属性
    │  2. 调用 ThemeService::setThemeMode(Dark)
    ▼
ThemeService::setThemeMode(Dark)
    │  1. m_mode = Dark
    │  2. applyTheme() → 加载对应 QSS
    │  3. qApp->setStyleSheet(qss)
    │  4. 持久化到 SettingsService
    │  5. emit themeChanged(true)
    ▼
各 View 收到 themeChanged 信号
    │  更新动态颜色（如自绘组件的画笔颜色）
```

---

## 五、项目目录结构

```
FrameMind/                            # 客户端项目（仓库根目录 Frame_Mind/）
├── CMakeLists.txt
├── README.md
├── resources/
│   ├── icons/                     # SVG 图标
│   ├── styles/
│   │   ├── dark.qss              # 暗色主题样式
│   │   ├── light.qss            # 亮色主题样式
│   │   └── common.qss           # 公共样式
│   ├── fonts/                    # 字体文件
│   └── resources.qrc            # Qt 资源文件
│
├── src/
│   ├── main.cpp                  # 入口 + DI 容器初始化
│   ├── app/
│   │   ├── application.h/.cpp    # 应用生命周期管理
│   │   └── dicontainer.h/.cpp    # 依赖注入容器
│   │
│   ├── view/                     # View 层
│   │   ├── mainwindow.h/.cpp
│   │   ├── sidebar/
│   │   │   └── sidebarview.h/.cpp
│   │   ├── player/
│   │   │   ├── playerview.h/.cpp
│   │   │   ├── videorenderwidget.h/.cpp
│   │   │   └── playercontrolbar.h/.cpp
│   │   ├── chat/
│   │   │   ├── chatview.h/.cpp
│   │   │   ├── chatmessagelist.h/.cpp
│   │   │   ├── chatbubblewidget.h/.cpp
│   │   │   ├── chatinputwidget.h/.cpp
│   │   │   └── markdownrenderer.h/.cpp
│   │   ├── analysis/
│   │   │   ├── analysispanelview.h/.cpp
│   │   │   ├── timelinewidget.h/.cpp
│   │   │   ├── detectionlistwidget.h/.cpp
│   │   │   └── subtitlelistwidget.h/.cpp
│   │   ├── filelist/
│   │   │   └── filelistview.h/.cpp
│   │   └── settings/
│   │       └── settingsview.h/.cpp
│   │
│   ├── viewmodel/                # ViewModel 层
│   │   ├── playerviewmodel.h/.cpp
│   │   ├── chatviewmodel.h/.cpp
│   │   ├── analysisviewmodel.h/.cpp
│   │   ├── filelistviewmodel.h/.cpp
│   │   ├── settingsviewmodel.h/.cpp
│   │   └── navigationviewmodel.h/.cpp
│   │
│   ├── model/                    # Domain Models
│   │   ├── chatmessage.h
│   │   ├── conversation.h
│   │   ├── videoinfo.h
│   │   ├── timelineevent.h
│   │   ├── detectionitem.h
│   │   └── subtitleentry.h
│   │
│   ├── service/                  # Service 层
│   │   ├── playerservice.h/.cpp
│   │   ├── agentservice.h/.cpp
│   │   ├── videoanalysisservice.h/.cpp
│   │   ├── conversationservice.h/.cpp
│   │   ├── filemanagerservice.h/.cpp
│   │   ├── themeservice.h/.cpp
│   │   └── settingsservice.h/.cpp
│   │
│   ├── infrastructure/           # 基础设施
│   │   ├── networkclient.h/.cpp
│   │   ├── databasemanager.h/.cpp
│   │   ├── imageprocessor.h/.cpp
│   │   ├── eventbus.h/.cpp
│   │   └── logger.h/.cpp
│   │
│   └── utils/                    # 工具类
│       ├── singleton.h           # 单例模板
│       ├── asynctask.h           # 异步任务封装
│       └── constants.h           # 常量定义
│
├── libs/
│   └── smartplayer_sdk/          # SmartPlayer SDK (头文件 + 库)
│       ├── include/
│       └── lib/
│
└── tests/                        # 单元测试
    ├── test_chatviewmodel.cpp
    ├── test_playerservice.cpp
    └── test_agentservice.cpp
```

---

## 六、依赖注入与生命周期

```cpp
// dicontainer.h — 简易 DI 容器
class DIContainer {
public:
    void initialize() {
        // Infrastructure
        m_db = std::make_unique<DatabaseManager>();
        m_db->initialize(appDataPath() + "/agent.db");
        m_network = std::make_unique<NetworkClient>();
        m_eventBus = EventBus::instance();

        // Services
        m_settingsService = std::make_unique<SettingsService>(m_db.get());
        m_themeService = std::make_unique<ThemeService>(m_settingsService.get());
        m_playerService = std::make_unique<PlayerService>();
        m_agentService = std::make_unique<AgentService>(m_network.get());
        m_convService = std::make_unique<ConversationService>(m_db.get());
        // M3 阶段 ragStore 传 nullptr；M4 启用 RAG 后注入实例
        m_analysisService = std::make_unique<VideoAnalysisService>(
            m_agentService.get(), m_playerService.get(), /*ragStore*/ nullptr);
        m_fileService = std::make_unique<FileManagerService>();

        // ViewModels
        m_navVM = std::make_unique<NavigationViewModel>();
        m_playerVM = std::make_unique<PlayerViewModel>(m_playerService.get(),
                                                       m_analysisService.get(),
                                                       m_eventBus);   // 监听 seekToPosition
        m_chatVM = std::make_unique<ChatViewModel>(m_agentService.get(),
                                                    m_convService.get(),
                                                    m_eventBus);
        m_analysisVM = std::make_unique<AnalysisViewModel>(m_analysisService.get(),
                                                            m_eventBus);
        m_fileVM = std::make_unique<FileListViewModel>(m_fileService.get());
        m_settingsVM = std::make_unique<SettingsViewModel>(m_settingsService.get(),
                                                           m_themeService.get());
    }

    // getter 方法...
    PlayerViewModel* playerVM() { return m_playerVM.get(); }
    ChatViewModel* chatVM() { return m_chatVM.get(); }
    // ...
};
```

---

## 七、关键设计决策

### 7.1 MVVM 绑定机制选择

| 方案 | 优劣 | 本项目选择 |
|------|------|-----------|
| Q_PROPERTY + signal/slot | Qt 原生，类型安全，IDE 支持好 | ✅ 主要方案 |
| QML Binding | 声明式，但 Widgets 项目混用复杂 | ❌ 不采用 |
| 手动 connect | 灵活但繁琐 | 辅助使用 |

### 7.2 跨 ViewModel 通信

**统一规则：所有跨 ViewModel 通信走 `EventBus`，ViewModel 之间不互相持有引用。**
这样可以保证每个 VM 都能独立单元测试，也避免多个 VM 互相依赖形成耦合环。

| 场景 | 事件 |
|------|------|
| Chat → Player（时间戳跳转） | `EventBus::seekToPosition(int64_t posMs)` |
| Chat → Player（"📷 当前帧"按钮） | `EventBus::frameForAIRequested(int64_t posMs)`，PlayerVM 截帧后回 `EventBus::screenshotForAI(QImage, ts)` |
| Player → Analysis（帧更新） | `EventBus::frameDecoded(QImage, ts)` |
| Analysis → Player（点击时间线/字幕跳转） | `EventBus::seekToPosition(int64_t posMs)` |
| Settings → 全局（主题切换） | `ThemeService::themeChanged` 信号 → 各 View 直接监听（不经 VM） |

### 7.3 线程模型

```
主线程 (GUI Thread)
├── 所有 View 绑定与渲染
├── ViewModel 状态更新
└── 信号槽分发

SDK 解码线程 (SmartPlayer 内部)
├── onVideoFrame 回调
└── 通过 QueuedConnection 跨线程传递 QImage

网络线程 (QNetworkAccessManager)
├── HTTP/SSE 请求
└── 回调回主线程处理

分析线程 (QThreadPool)
├── 图像预处理
└── 采样帧批量编码
```

### 7.4 性能优化策略

| 问题 | 方案 |
|------|------|
| 帧回调高频 (30~60fps) | 渲染帧跳帧策略，UI 最多 30fps 刷新 |
| QImage 跨线程拷贝开销 | 使用 `QImage` 隐式共享 (COW)，避免深拷贝 |
| AI 请求大图片 | 发送前缩放到 720p + JPEG 压缩 (base64 体积可控) |
| 长对话历史 | ListView 虚拟化，仅渲染可见区域 |
| 主题切换闪烁 | QSS 预编译缓存 + 200ms opacity 动画过渡 |

---

## 八、数据库 Schema

```sql
-- 对话表
CREATE TABLE conversations (
    id TEXT PRIMARY KEY,
    title TEXT NOT NULL DEFAULT '新对话',
    video_path TEXT,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
);

-- 消息表
CREATE TABLE messages (
    id TEXT PRIMARY KEY,
    conversation_id TEXT NOT NULL,
    role TEXT NOT NULL CHECK(role IN ('user','assistant','system')),
    content TEXT NOT NULL,
    attached_frames TEXT,          -- JSON array of base64 thumbnails
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (conversation_id) REFERENCES conversations(id) ON DELETE CASCADE
);

-- 视频分析缓存
CREATE TABLE analysis_cache (
    video_path TEXT NOT NULL,
    analysis_type TEXT NOT NULL,   -- 'timeline' | 'subtitle' | 'detection'
    data TEXT NOT NULL,            -- JSON
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (video_path, analysis_type)
);

-- 设置表（仅存非敏感配置；API Key / Token 等机密走 OS 密钥管理服务，见下方说明）
CREATE TABLE settings (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

-- 密钥存储说明：
-- 不在 SQLite 中保存任何 API Key / Bearer Token / 密码等机密。
-- 平台对应的密钥管理服务：
--   Windows : DPAPI (CryptProtectData / CryptUnprotectData)，作用域 CRYPTPROTECT_LOCAL_MACHINE = false
--   macOS   : Keychain (SecItemAdd / SecItemCopyMatching)
--   Linux   : libsecret (Secret Service API，例如 GNOME Keyring / KWallet)
-- 客户端在 SettingsService 中封装 `secretGet(name) / secretSet(name, value)`，
-- 跨平台代码统一抽象，不让上层 ViewModel 感知具体实现。

-- 最近文件
CREATE TABLE recent_files (
    path TEXT PRIMARY KEY,
    last_opened DATETIME DEFAULT CURRENT_TIMESTAMP
);

CREATE INDEX idx_messages_conv ON messages(conversation_id, timestamp);
CREATE INDEX idx_recent_files_time ON recent_files(last_opened DESC);
```

---

## 九、扩展性设计

### 9.1 AI 后端可插拔

```cpp
// 抽象接口
class ILLMBackend {
public:
    virtual ~ILLMBackend() = default;
    virtual void sendChat(const ChatRequest& req,
                          std::function<void(const QString&)> onChunk,
                          std::function<void(const ChatMessage&)> onDone,
                          std::function<void(const QString&)> onError) = 0;
    virtual void cancel() = 0;
    virtual QString name() const = 0;
};

// 实现
class OpenAIBackend : public ILLMBackend { ... };
class OllamaBackend : public ILLMBackend { ... };
class QwenBackend   : public ILLMBackend { ... };
```

### 9.2 分析能力可插拔

```cpp
class IAnalysisPlugin {
public:
    virtual ~IAnalysisPlugin() = default;
    virtual QString pluginName() const = 0;
    virtual AnalysisResult analyzeFrame(const QImage& frame, int64_t tsMs) = 0;
    virtual bool supportsVideoLevel() const { return false; }
};
```

---

## 十、总结

本架构通过 **MVVM 分层** 实现：
- **View** 只负责 UI 渲染与事件转发
- **ViewModel** 持有可绑定状态，协调业务逻辑
- **Model/Service** 封装领域逻辑与外部依赖
- **Infrastructure** 提供底层能力（网络/数据库/SDK 封装）

核心优势：
1. **可测试** — ViewModel 可脱离 UI 独立测试
2. **可维护** — 职责清晰，改 UI 不动业务，改业务不动 UI
3. **可扩展** — AI 后端、分析插件可热插拔
4. **高性能** — 帧回调异步传递、渲染跳帧、网络流式处理
