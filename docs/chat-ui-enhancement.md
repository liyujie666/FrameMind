# 聊天气泡 UI 增强方案

## 改造目标

将原有的基于 QLabel + Qt::MarkdownText 的简单气泡升级为：
- 使用 QTextBrowser + HTML 渲染
- 支持更丰富的 Markdown 样式
- 添加消息头部（时间戳）
- 添加操作栏（复制、重新生成）
- 更好的主题适配

## 架构变化

### 原有架构
```
ChatBubbleWidget
  └── QLabel (Qt::MarkdownText)
      └── 直接显示 Markdown
```

### 新架构
```
ChatBubbleWidget
  ├── 消息头部 (时间戳)
  ├── 附件缩略图
  ├── QTextBrowser (HTML)
  │   └── MarkdownRenderer 转换后的 HTML
  └── 操作栏 (鼠标悬停显示)
      ├── 复制按钮
      └── 重新生成按钮
```

## 新增组件

### 1. MarkdownRenderer 服务类
**位置**: `src/service/markdownrenderer.{h,cpp}`

**功能**:
- 将 Markdown 文本转换为带样式的 HTML
- 使用 Qt 的 QTextDocument::setMarkdown() 进行解析
- 生成统一的 CSS 样式（支持深色/浅色主题）
- 处理代码块、引用块、表格等格式

**关键方法**:
```cpp
QString toHtml(const QString& markdown, bool isDarkTheme = true) const;
```

### 2. 增强的 ChatBubbleWidget
**位置**: `src/view/chat/chatbubblewidget.{h,cpp}`

**新增功能**:
- 使用 QTextBrowser 替代 QLabel
- 消息头部显示时间戳
- 操作栏（鼠标悬停显示）
  - 复制按钮：复制 Markdown 原文到剪贴板
  - 重新生成按钮：仅 AI 消息显示
- 更大的圆角半径（用户消息 16px，AI 消息 12px）
- AI 消息带边框，用户消息无边框

**新增信号**:
```cpp
void copyRequested(const QString& content);
void regenerateRequested();
```

## 样式改进

### 代码块
- 深色背景 (#171721)
- 等宽字体 (Cascadia Mono / Consolas)
- 内边距和圆角
- 支持语法标记类 (language-cpp, language-python 等)

### 引用块
- 左侧 4px 主题色边框
- 浅色背景
- 内边距和圆角

### 列表
- 合适的缩进 (24px)
- 列表项间距 (4px)

### 表格
- 边框颜色跟随主题
- 表头加粗并带背景色
- 单元格内边距 (8px 12px)

### 段落和标题
- 标题层级明确 (h1:24px ~ h6:13px)
- 段落间距 (12px)
- 标题上边距 (16px)
- 行高 (1.6)

## 使用示例

### 在 ChatView 中初始化
```cpp
ChatView::ChatView(QWidget* parent) : QWidget(parent) {
    // 创建 Markdown 渲染器
    m_renderer = new MarkdownRenderer(this);
    
    // 创建消息列表并设置渲染器
    m_messageList = new ChatMessageList(this);
    m_messageList->setMarkdownRenderer(m_renderer);
    
    // 主题变更时更新渲染器
    connect(m_theme, &ThemeService::themeChanged, this, [this]() {
        m_renderer->setThemeService(m_theme);
        m_messageList->refreshBubbleColors();
    });
}
```

### 设置消息内容
```cpp
// ViewModel 仍然使用 Markdown
ChatMessage msg;
msg.content = "## 场景分析\n\n人物在 **00:12** 进入画面。\n\n> 画面亮度较低。";

// ChatBubbleWidget 自动转换为 HTML
bubble->setMessage(msg);
```

## 性能优化

### 流式渲染
- 保留原有的 40ms 节流机制
- 仅在 Markdown 内容变化时重新渲染
- 避免频繁的 HTML 重解析

### 颜色缓存
- ChatBubbleWidget 缓存主题颜色
- 避免每次 paintEvent 查询 ThemeService
- 仅在主题切换时更新

### 批量更新
```cpp
void ChatMessageList::refreshBubbleColors() {
    m_container->setUpdatesEnabled(false);
    for (auto* bubble : m_bubbles) {
        bubble->refreshColors();
    }
    m_container->setUpdatesEnabled(true);
}
```

## 兼容性

### 保持向后兼容
- 模型仍然返回 Markdown 格式
- 时间戳跳转链接保持不变 (`ts://12000`)
- 现有的流式更新机制无需修改

### Qt 版本要求
- Qt 6.0+ (QTextDocument::setMarkdown 在 Qt 5.14+ 可用)
- 不依赖外部 Markdown 解析库

## 未来扩展

### 代码语法高亮
可以在 MarkdownRenderer 中集成语法高亮器：
```cpp
QString highlightCode(const QString& code, const QString& language);
```

### 代码复制按钮
在代码块右上角添加复制按钮：
```html
<div class="code-block-wrapper">
  <button class="copy-code-btn">📋</button>
  <pre><code>...</code></pre>
</div>
```

### 折叠长代码块
对超过 20 行的代码块自动折叠：
```cpp
if (lineCount > 20) {
    html += "<button class='expand-btn'>展开全部</button>";
}
```

### LaTeX 数学公式
使用 MathJax 或 KaTeX 渲染数学公式：
```html
<span class="math-inline">\( E = mc^2 \)</span>
<div class="math-block">\[ \int_0^\infty e^{-x^2} dx = \frac{\sqrt{\pi}}{2} \]</div>
```

## 测试清单

- [x] Markdown 基本格式（粗体、斜体、链接）
- [x] 代码块（行内代码、多行代码块）
- [x] 列表（有序、无序、嵌套）
- [x] 引用块
- [x] 表格
- [x] 标题（h1-h6）
- [x] 时间戳链接 (`ts://12000`)
- [x] 主题切换（深色/浅色）
- [x] 流式更新
- [x] 操作按钮（复制、重新生成）
- [x] 鼠标悬停交互

## 变更文件清单

### 新增文件
- `src/service/markdownrenderer.h`
- `src/service/markdownrenderer.cpp`

### 修改文件
- `src/view/chat/chatbubblewidget.h`
- `src/view/chat/chatbubblewidget.cpp`
- `src/view/chat/chatmessagelist.h`
- `src/view/chat/chatmessagelist.cpp`
- `src/view/chat/chatview.h`
- `src/view/chat/chatview.cpp`

### 无需修改
- `CMakeLists.txt` (使用 Qt 内置功能，无需外部依赖)
- `src/viewmodel/chatviewmodel.cpp` (数据层保持不变)
- `src/service/agentservice.cpp` (模型接口保持不变)
