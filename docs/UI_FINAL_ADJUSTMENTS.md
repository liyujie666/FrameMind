# 聊天气泡 UI 最终调整

## 🎨 布局优化

### 调整前的问题
- ❌ 气泡宽度不固定，鼠标悬停时会变化
- ❌ 时间戳和操作按钮在气泡内部
- ❌ 操作按钮使用文字 + emoji
- ❌ 整体显得拥挤

### 调整后的改进
- ✅ **气泡宽度固定**：400-600px，不随悬停变化
- ✅ **时间戳**：在气泡**上方**（仅 AI 消息显示）
- ✅ **操作栏**：在气泡**下方**（鼠标悬停显示）
- ✅ **图标按钮**：使用 SVG/PNG 图标，更简洁美观
- ✅ **主题适配**：深色主题用 light 图标，浅色主题用 dark 图标

## 📐 新的结构

```
ChatBubbleWidget (固定宽度 400-600px)
│
├── 时间戳 (在气泡外上方，仅 AI 消息)
│   └── "21:40:59" (11px, 灰色)
│
├── 气泡容器 (圆角背景)
│   ├── 附件缩略图
│   └── 内容 (QTextBrowser)
│
└── 操作栏 (在气泡外下方，悬停显示)
    ├── 📋 复制 (图标按钮)
    └── 🔄 重新生成 (图标按钮，仅 AI 消息)
```

## 🎯 关键代码变更

### 1. 固定宽度

```cpp
ChatBubbleWidget::ChatBubbleWidget(QWidget* parent) {
    setMinimumWidth(400);  // 最小宽度
    setMaximumWidth(600);  // 最大宽度
}
```

### 2. 三层结构

```cpp
m_mainLayout->setContentsMargins(0, 0, 0, 0);  // 外层无边距

// 头部（时间戳）
createHeader();

// 气泡容器（独立的 widget）
auto* bubbleContainer = new QWidget(this);
auto* bubbleLayout = new QVBoxLayout(bubbleContainer);
bubbleLayout->setContentsMargins(12, 10, 12, 10);  // 气泡内边距

// 操作栏
createActionBar();
```

### 3. paintEvent 只绘制气泡

```cpp
void ChatBubbleWidget::paintEvent(QPaintEvent*) {
    // 只绘制气泡容器的背景（跳过头部和操作栏）
    if (m_mainLayout->count() >= 2) {
        QWidget* bubbleContainer = qobject_cast<QWidget*>(
            m_mainLayout->itemAt(1)->widget());
        if (bubbleContainer) {
            const QRect bubbleRect = bubbleContainer->geometry();
            // 绘制圆角背景...
        }
    }
}
```

### 4. 图标按钮

```cpp
// 根据主题选择图标
const bool isDark = !m_theme || m_theme->isDark();
const QString iconSuffix = isDark 
    ? QStringLiteral("_light.png")  // 深色主题用亮色图标
    : QStringLiteral("_dark.png");  // 浅色主题用暗色图标

m_copyButton->setIcon(QIcon(":/icons/copy" + iconSuffix));
m_copyButton->setIconSize(QSize(16, 16));
m_copyButton->setFixedSize(24, 24);

m_regenerateButton->setIcon(QIcon(":/icons/replay" + iconSuffix));
m_regenerateButton->setIconSize(QSize(16, 16));
m_regenerateButton->setFixedSize(24, 24);
```

## 📦 需要的图标资源

确保项目中有以下图标：

```
resources/icons/
├── copy_light.png       # 深色主题复制图标
├── copy_dark.png        # 浅色主题复制图标
├── replay_light.png     # 深色主题重新生成图标
└── replay_dark.png      # 浅色主题重新生成图标
```

图标规格：
- 尺寸：16x16px 或 24x24px
- 格式：PNG（透明背景）
- 颜色：light 版本用白色/浅色，dark 版本用黑色/深色

## 🎨 视觉效果

### 用户消息（右侧）
```
                           [21:40:59]  ← 时间戳（如果需要显示）
                         ┌──────────┐
                         │ 你好！    │  ← 用户消息
                         │          │     16px 圆角
                         └──────────┘     蓝色背景
```

### AI 消息（左侧）
```
21:40:59  ← 时间戳
┌────────────────────────────┐
│ 你好！你想了解什么呢？        │  ← AI 消息
│                            │     12px 圆角
│ 我可以帮你分析视频内容。      │     灰色背景 + 边框
└────────────────────────────┘
[📋] [🔄]  ← 操作按钮（鼠标悬停显示）
```

## ✅ 改进总结

| 项目 | 改进前 | 改进后 |
|------|--------|--------|
| **气泡宽度** | 不固定，悬停变化 | 固定 400-600px |
| **时间戳位置** | 气泡内部 | 气泡上方（外部）|
| **操作栏位置** | 气泡内部 | 气泡下方（外部）|
| **操作按钮** | 文字 + emoji | 图标按钮 |
| **按钮尺寸** | 不固定 | 24x24px |
| **主题适配** | 单一样式 | 图标跟随主题 |
| **视觉层次** | 混乱 | 清晰分层 |

## 🚀 编译运行

所有修改已完成，现在可以编译：

```bash
cd d:\Qt\ffmpegProjects\FrameMind
cmake --build build --config Release
```

运行后你会看到：
- ✨ 气泡宽度稳定，不再跳动
- 📅 时间戳优雅地显示在气泡上方
- 🎯 操作按钮整齐地排列在气泡下方
- 🖼️ 图标按钮简洁专业

现在的 UI 更接近 Codex、WorkBuddy 等专业 Agent 应用了！
