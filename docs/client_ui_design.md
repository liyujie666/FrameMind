## UI 设计方案
### 整体布局
```plain
┌──┬─────────────────────────────────────────────────────────────┐
│  │                                                             │
│头│     ┌─────────────────────────┬──────────────────────┐      │
│像│     │                         │ ◀ AI 助手             │      │
│  │     │      视频播放器区域      │                      │      │
│──│     │      (16:9 比例)         │   AI: 这段视频展示了… │      │
│💬│     │                         │                      │      │
│  │     │  ▶  ──●────── 3:24/12:00 🔊── 1.0x ⛶│   You: 视频里有几个人？│      │
│──│     ├─────────────────────────┤                      │      │
│📁│     │                         │   AI: 画面中可以看到…  │      │
│  │     │   自定义显示区域          │                      │      │
│──│     │   [时间线][检测][字幕]    │                      │      │
│  │     │                         │ ┌──────────────────┐ │      │
│  │     │                         │ │ 输入问题...  [📷][发送]│ │      │
│⚙│     └─────────────────────────┴──────────────────────┘      │
└──┴─────────────────────────────────────────────────────────────┘
```

### 预期效果：
<!-- 这是一张图片，ocr 内容为： -->
![](https://cdn.nlark.com/yuque/0/2026/png/29086946/1782803797322-4f24fc47-d38b-401c-8375-d78f35503e96.png)<!-- 这是一张图片，ocr 内容为： -->
![](https://cdn.nlark.com/yuque/0/2026/png/29086946/1782803797488-637a2dc0-f5e2-4afd-b90b-2de0fc24ea1e.png)

### 设计优化建议
#### 1. 左侧导航栏（64px 宽）
| 元素 | 设计细节 |
| --- | --- |
| 头像 | 40px 圆形，hover 显示用户名 tooltip |
| 图标 | 24px，间距 20px，激活态左侧 3px 蓝色指示条 |
| 排布 | 头像顶部，功能图标居中区域，设置固定底部 |
| 扩展 | 可加"知识库"图标（后续扩展 RAG） |


#### 2. 视频播放器（优化建议）
+ **控制栏浮层**：鼠标移入视频区显示，移出自动隐藏（不占用空间）
+ **右键菜单**：截取当前帧发给 AI、标记时间点、逐帧步进
+ **AI 联动按钮**：控制栏增加一个"问 AI"按钮，点击自动截取当前帧发送到对话框

#### 3. 下方自定义区域（建议功能）
设计为**可切换 Tab 面板**：

| Tab | 内容 |
| --- | --- |
| 时间线 | 场景分割可视化 + 事件标记点（可点击跳转） |
| 检测结果 | 当前帧的目标检测结果列表（物体、人物、文字） |
| 字幕 | ASR 生成的逐句字幕（点击跳转对应时间） |
| 知识图谱 | 视频实体关系可视化（后期） |


#### 4. 右侧对话面板（核心优化）
**折叠/展开**：

+ 左边缘有拖拽条，可调整宽度（最小 320px，最大 50%）
+ 完全折叠时只显示一个 `▶` 展开按钮

**对话增强**：

+ 输入框上方加快捷操作按钮：
    - `📷 当前帧` — 一键截取当前画面作为问题上下文
    - `🎬 选片段` — 选择时间区间作为分析对象
    - `📎 附件` — 上传参考图片
+ AI 回复支持富文本：时间戳可点击跳转、检测框可高亮到视频上
+ 消息支持"引用帧"展示（消息中内嵌视频截图缩略图）

#### 5. 交互联动（Agent 特色）
这是区别于普通播放器的关键：

```plain
视频播放器 ←→ AI对话 双向联动:

用户在对话中问 "3分钟处那个人是谁"
  → Agent 自动 seek 到 3:00，截帧分析，回复并高亮画面区域

用户在视频上右键 "分析此区域"  
  → 自动裁剪 ROI 区域发到对话框，触发 Agent 分析

AI 回复 "在 [2:30] 处检测到异常"
  → 时间戳可点击，自动跳转播放器到 2:30
```

---

### 主题系统设计
<!-- 这是一张图片，ocr 内容为： -->
![](https://cdn.nlark.com/yuque/0/2026/png/29086946/1782804951231-85d6f924-1fd6-47e7-823e-e360fa24e039.png)

#### 三种模式
| 模式 | 行为 |
| --- | --- |
| **跟随系统** | 监听系统主题切换信号，实时响应 |
| **亮色** | 强制使用 Light 主题 |
| **暗色** | 强制使用 Dark 主题 |


---

#### 完整色值定义
##### Dark Theme（暗色）
```plain
Background:      #0D1117    主背景
Surface:         #1E1E2E    卡片/面板/弹窗
SurfaceVariant:  #252538    二级面板（如输入框）
Sidebar:         #161622    导航栏
Overlay:         #000000CC  遮罩层（80%透明黑）

Primary:         #2979FF    按钮/链接/激活态
PrimaryHover:    #448AFF    按钮悬停
PrimaryPressed:  #1565C0    按钮按下
PrimaryDisabled: #2979FF66  禁用态（40%透明度）

TextPrimary:     #E0E0E0    主要文字
TextSecondary:   #8B8B8B    次要/辅助文字
TextDisabled:    #5A5A5A    禁用文字
TextOnPrimary:   #FFFFFF    按钮上的文字

Border:          #2D2D3D    常规边框
BorderFocused:   #2979FF    聚焦边框
Divider:         #2D2D3D40  分割线（25%透明）

UserBubble:      #2979FF    用户消息气泡
UserBubbleText:  #FFFFFF    用户消息文字
AIBubble:        #252536    AI 消息气泡
AIBubbleText:    #E0E0E0    AI 消息文字

InputBg:         #1A1A2A    输入框背景
ScrollThumb:     #3A3A4A    滚动条
ScrollHover:     #4A4A5A    滚动条悬停

Success:         #4CAF50
Warning:         #FF9800
Error:           #F44336
Info:            #29B6F6
```

##### Light Theme（亮色）
```plain
Background:      #F8F9FA    主背景
Surface:         #FFFFFF    卡片/面板/弹窗
SurfaceVariant:  #F5F5F5    二级面板
Sidebar:         #F0F1F3    导航栏
Overlay:         #00000066  遮罩层（40%透明黑）

Primary:         #1565C0    按钮/链接/激活态
PrimaryHover:    #1976D2    按钮悬停
PrimaryPressed:  #0D47A1    按钮按下
PrimaryDisabled: #1565C066  禁用态

TextPrimary:     #1A1A1A    主要文字
TextSecondary:   #6B6B6B    次要文字
TextDisabled:    #BDBDBD    禁用文字
TextOnPrimary:   #FFFFFF    按钮上的文字

Border:          #E0E0E0    常规边框
BorderFocused:   #1565C0    聚焦边框
Divider:         #0000001A  分割线

UserBubble:      #1565C0    用户消息气泡
UserBubbleText:  #FFFFFF    用户消息文字
AIBubble:        #F0F2F5    AI 消息气泡
AIBubbleText:    #1A1A1A    AI 消息文字

InputBg:         #FFFFFF    输入框背景
ScrollThumb:     #C0C0C0    滚动条
ScrollHover:     #A0A0A0    滚动条悬停

Success:         #2E7D32
Warning:         #E65100
Error:           #C62828
Info:            #0277BD
```

---

#### Qt6.9 实现方案
##### 1. 主题服务（DI 注入，非单例）

> 与 `architecture-design.md` §3.3.2 保持一致：不使用全局单例 `ThemeManager`，而是通过 DI 容器注入 `ThemeService`，便于测试中替换为 mock。

```cpp
// themeservice.h
enum class ThemeMode { FollowSystem, Light, Dark };

class ThemeService : public QObject {
    Q_OBJECT
public:
    explicit ThemeService(SettingsService* settings, QObject* parent = nullptr);

    void setThemeMode(ThemeMode mode);
    ThemeMode themeMode() const;
    bool isDark() const;                 // 当前实际是否暗色
    void applyTheme();                   // 加载并应用对应 QSS

    // 获取当前主题色值（token 见 #完整色值定义 章节）
    QColor color(const QString& token) const;   // 如 "primary" / "textPrimary"

signals:
    void themeChanged(bool isDark);

private:
    void onSystemThemeChanged();
    SettingsService* m_settings;
    ThemeMode        m_mode = ThemeMode::FollowSystem;
};
```

##### 2. 设置页面切换 UI
```plain
┌─ 外观设置 ─────────────────────────┐
│                                     │
│  主题模式                            │
│  ┌─────┐  ┌─────┐  ┌─────┐        │
│  │ 🌓  │  │ ☀️  │  │ 🌙  │        │
│  │跟随  │  │亮色  │  │暗色  │        │
│  │系统  │  │     │  │     │        │
│  └──✓──┘  └─────┘  └─────┘        │
│                                     │
└─────────────────────────────────────┘
```

---

#### 设计原则
1. **亮色不是简单反转** — 亮色模式的 Primary 用了更深的蓝 (`#1565C0`)，保证在白背景上有足够对比度（WCAG AA 标准）
2. **阴影策略不同** — 暗色用微亮边框区分层级，亮色用 `box-shadow` 投影区分
3. **动画过渡** — 切换主题时加 200ms 渐变过渡，避免闪烁
4. **持久化** — 用户选择存入 SQLite `settings` 表（key: `ui.theme_mode`），由 `SettingsService` 统一管理；下次启动恢复

---

#### Qt6.9 实现要点
```plain
QMainWindow
├── QWidget (侧边导航栏, fixedWidth=64)
│   └── QVBoxLayout: [头像, Spacer, 图标按钮组, Spacer, 设置按钮]
│
└── QSplitter (水平, 主内容区)
    ├── QWidget (左面板)
    │   └── QVBoxLayout
    │       ├── VideoWidget (自定义QWidget, 重写paintEvent渲染onVideoFrame)
    │       │   └── PlayerControlBar (浮层, 进度条+控制按钮)
    │       └── QTabWidget (时间线/检测/字幕)
    │
    └── QWidget (右面板, 可折叠)
        └── QVBoxLayout
            ├── ChatHeader (标题 + 折叠按钮)
            ├── QScrollArea → ChatMessageList (气泡消息列表)
            └── ChatInputWidget (输入框 + 快捷按钮 + 发送)
```

视频渲染关键：在 `onVideoFrame` 回调中将帧数据转为 `QImage`，通过信号槽跨线程传递到 `VideoWidget::paintEvent` 用 `QPainter` 绘制。



