# 聊天气泡改造 - 快速参考

## 📋 改造概览

**目标**：从 QLabel 简单气泡 → 类似 Codex/WorkBuddy 的现代 Agent 界面

**核心改进**：
- ✅ QTextBrowser + HTML 富文本渲染
- ✅ 代码语法高亮（C++, Python, JS, JSON, Bash）
- ✅ 消息头部（时间戳）
- ✅ 操作栏（复制、重新生成）
- ✅ 优化的主题样式

## 🆕 新增文件（4个）

```
src/service/
├── markdownrenderer.h        # Markdown → HTML 转换服务
├── markdownrenderer.cpp
├── codehighlighter.h         # 代码语法高亮器
└── codehighlighter.cpp
```

## 📝 修改文件（6个）

```
src/view/chat/
├── chatbubblewidget.h         # QLabel → QTextBrowser
├── chatbubblewidget.cpp       # 添加头部、操作栏
├── chatmessagelist.h          # 传递 MarkdownRenderer
├── chatmessagelist.cpp
├── chatview.h                 # 创建并管理渲染器
└── chatview.cpp
```

## 🔑 关键 API

### MarkdownRenderer

```cpp
// 创建渲染器
auto* renderer = new MarkdownRenderer(themeService);

// Markdown 转 HTML（带语法高亮）
QString html = renderer->toHtml(markdown, isDarkTheme);
```

### CodeHighlighter

```cpp
// 创建高亮器
auto* highlighter = new CodeHighlighter(themeService);

// 高亮代码
QString highlighted = highlighter->highlight(code, "cpp", isDarkTheme);

// 支持的语言：cpp, python, javascript, json, bash
```

### ChatBubbleWidget

```cpp
// 设置渲染器
bubble->setMarkdownRenderer(renderer);

// 更新内容（自动转换为 HTML）
bubble->updateContent(markdown);

// 新增信号
connect(bubble, &ChatBubbleWidget::copyRequested, ...);
connect(bubble, &ChatBubbleWidget::regenerateRequested, ...);
```

## 🎨 样式特性

### 代码块
```markdown
\`\`\`cpp
int main() {
    return 0;  // 关键字紫色，注释绿色
}
\`\`\`
```
→ 深色背景 + 语法高亮 + 圆角

### 引用块
```markdown
> 这是一条引用
```
→ 左侧蓝色边框 + 浅色背景

### 表格
```markdown
| 列1 | 列2 |
|-----|-----|
| A   | B   |
```
→ 完整边框 + 表头加粗

## 🔧 集成步骤

### 1. 在 ChatView 中初始化

```cpp
ChatView::ChatView(QWidget* parent) {
    // 创建渲染器
    m_renderer = new MarkdownRenderer(this);
    
    // 传递给消息列表
    m_messageList->setMarkdownRenderer(m_renderer);
    
    // 主题切换时更新
    connect(m_theme, &ThemeService::themeChanged, [this]() {
        m_renderer->setThemeService(m_theme);
        m_messageList->refreshBubbleColors();
    });
}
```

### 2. 模型输出保持不变

```cpp
// 模型仍然返回 Markdown
QString response = "## 标题\n\n代码：\n\n```cpp\nint x = 10;\n```";

// ChatBubbleWidget 自动转换为 HTML
bubble->updateContent(response);
```

## ⚡ 性能优化

```cpp
// 1. 颜色缓存
m_bgColor = m_theme->color(...);  // 缓存，避免重复查询

// 2. 批量更新
m_container->setUpdatesEnabled(false);
for (auto* bubble : m_bubbles) {
    bubble->refreshColors();
}
m_container->setUpdatesEnabled(true);

// 3. 流式节流
constexpr int kFlushIntervalMs = 40;  // ~25fps
```

## 🧪 测试 Markdown 示例

```markdown
## 视频分析结果

这段视频展示了**重要场景**。

### 关键时间点
- **00:12**: 开始
- **00:28**: 结束

> 注意：画面较暗

### 代码片段
\`\`\`python
def analyze(video):
    return video.extract_frames()
\`\`\`

### 数据表格
| 时间 | 事件 |
|------|------|
| 00:12 | 开始 |
| 00:28 | 结束 |

[跳转到 00:12](ts://12000)
```

## 📊 效果对比

| 项目 | 改造前 | 改造后 |
|------|--------|--------|
| 渲染引擎 | QLabel | QTextBrowser |
| Markdown | Qt 内置 | Qt + 自定义 CSS |
| 代码高亮 | ❌ | ✅ (5种语言) |
| 操作按钮 | ❌ | ✅ (复制/重生成) |
| 时间戳 | ❌ | ✅ |
| 主题适配 | 基础 | 完整 |

## 🚀 编译运行

```bash
# 编译
cd d:\Qt\ffmpegProjects\FrameMind
cmake --build build --config Release

# 运行
cd build\Release
.\FrameMind.exe
```

## 🎯 验证清单

打开应用后，与 AI 对话，测试：
- [ ] 代码块显示语法高亮
- [ ] 引用块有左侧蓝色边框
- [ ] 鼠标悬停 AI 消息显示操作按钮
- [ ] 点击复制按钮成功复制
- [ ] 点击重新生成按钮触发重新生成
- [ ] 消息头部显示时间戳
- [ ] 主题切换时样式正确更新

## 💡 提示

### 无外部依赖
- ✅ 使用 Qt 内置 `QTextDocument::setMarkdown()`
- ✅ 自实现语法高亮
- ✅ 无需修改 CMakeLists.txt

### 向后兼容
- ✅ 模型输出格式不变（仍为 Markdown）
- ✅ 时间戳链接 `ts://12000` 保持不变
- ✅ 流式更新机制无需修改

### 扩展性强
- 未来可添加代码复制按钮
- 未来可添加 LaTeX 数学公式
- 未来可添加 Mermaid 图表
- 未来可添加图片预览

---

**完成状态**：✅ 全部完成，可以编译运行了！
