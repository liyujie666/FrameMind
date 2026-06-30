# 开发任务拆解 — 视频分析 AI Agent

> 本文档是 [`development-plan.md`](./development-plan.md) 的**执行视图**，把 M1~M5 五个里程碑拆解为
> 「有序、可独立勾选完成」的任务卡片。
>
> - 顺序即推荐执行顺序，原则上**自上而下推进**，每张卡片完成后再开下一张。
> - 每张卡片显式给出 **目标 / 输入 / 输出 / 关键步骤 / 验收 / 依赖卡片**。
> - 不含工时估算（按"做完一张算一张"的方式推进）。
> - 跨里程碑的通用规则集中在文末「通用约束」一节，所有任务卡默认遵守，不再每卡重复。
>
> SSoT（与本文档冲突时以这些为准）：
> - 范围/里程碑 → `development-plan.md`
> - 模块/类设计/目录/Schema → `architecture-design.md`
> - Agent 决策/Prompt/Tool 语义 → `agent-core-design.md`
> - 工具列表/HTTP/SSE 协议 → `api-protocol.md`
> - UI 布局/主题色值 → `client_ui_design.md`

---

## 〇、通用约束（所有任务卡都要遵守）

| 维度 | 约束 |
|------|------|
| 架构 | 严格 MVVM：View 不写业务逻辑；ViewModel 只通过 Service 拿数据；跨 VM 通信走 `EventBus`，**禁止 VM 互相持有引用** |
| 依赖注入 | 所有 Service / ViewModel 通过 `DIContainer` 装配；不引入新的全局单例（既有 `EventBus::instance()` 例外） |
| 线程 | UI 操作只能在主线程；SDK 解码线程 → 主线程必须用 `Qt::QueuedConnection`；`QImage` 隐式共享，避免深拷贝 |
| 安全 | API Key / Token **不入 SQLite / 配置文件 / 日志**，统一走 `SettingsService::secretGet/secretSet`（Windows DPAPI / macOS Keychain / Linux libsecret） |
| 错误 | 任何外部调用（SDK / 网络 / 文件 / 模型）都要给降级或可见错误提示，禁止吞异常 |
| 命名 | 头文件 / 实现文件全小写下划线（与现有目录约定一致），类名 UpperCamelCase |
| Git | 每完成一张任务卡 commit 一次；每个里程碑结尾打 tag（见 `development-plan.md` §六） |

---

## M1 — 骨架（"能播视频 + 有空对话框"）

> 出口标准：能打开本地 mp4，流畅播放、可拖动 seek、可调音量/倍速；右侧对话区为空 UI 占位；退出无崩溃。

---

### M1-T1　初始化 `FrameMind` 工程骨架与 CMake

**目标**：在当前仓库（`Frame_Mind/`）下拉起一个空的 Qt 6.9 工程，应用代号 `FrameMind`，能编出一个 800×600 的空 `MainWindow`。

**输入**：本机 Qt 6.9 / MSVC 2019+ / CMake ≥3.20。

**输出**：
- 仓库根 `CMakeLists.txt`（`project(FrameMind LANGUAGES CXX)`）。
- `src/main.cpp`、`src/app/application.{h,cpp}`、`src/view/mainwindow.{h,cpp}` 初始版本。
- 工程可被 Qt Creator / VS / CLion 任一打开并编译，可执行文件命名 `FrameMind(.exe)`。

**关键步骤**：
1. 按 `architecture-design.md` §五 创建目录骨架（暂可只建 `view/`、`app/`、`utils/`，其余空目录加 `.gitkeep`）。
2. CMakeLists：`find_package(Qt6 6.9 COMPONENTS Core Gui Widgets Network Sql Concurrent Svg)`，启用 `CMAKE_AUTOMOC / AUTOUIC / AUTORCC`。
3. 编一个空 `MainWindow`（仅 `setWindowTitle("Frame Mind") + resize`），`main.cpp` 启动。
4. 写最简 README（指明如何编译）。

**验收**：
- `cmake --build` 通过；运行后看到空白窗口、关闭无报错。
- `git init` + 首个 commit。

**依赖**：无。

---

### M1-T2　集成 SmartPlayer SDK

**目标**：把当前仓库的 `player_sdk` 编译产物作为 `third_party/smartplayer_sdk/` 引入，链接通过。

**输入**：`player_sdk` 头文件、`.lib`、`.dll`。

**输出**：
- `third_party/smartplayer_sdk/{include,lib,bin}/`。
- CMake 中新增 `INTERFACE` target `smartplayer`，主程序链接成功。
- `.dll` 在运行目录可被找到（`add_custom_command(... copy_if_different ...)` 或部署脚本）。

**关键步骤**：
1. 拷贝头文件 / 库；编写 `cmake/FindSmartPlayer.cmake` 或直接在主 CMakeLists 用 `add_library(smartplayer INTERFACE IMPORTED)`。
2. 在 `main.cpp` 临时 `#include <smartplayer.h>` 并实例化一个对象（仅验证链接），后续删除。
3. 安全：不要把 `.lib/.dll` 单独 commit 到主分支，可放 `third_party` 或走 Git LFS（视团队规范）。

**验收**：
- Release/Debug 都能链接通过。
- 程序启动时不因找不到 dll 而崩溃。

**依赖**：M1-T1。

---

### M1-T3　`MainWindow` 三栏布局骨架

**目标**：搭出 `client_ui_design.md` 描述的整体布局：左侧 64px 导航 / 中间播放器 + 下方 Tab 占位 / 右侧对话占位，可拖拽 splitter 调宽度。

**输入**：`client_ui_design.md`、`architecture-design.md` §3.1。

**输出**：
- `view/sidebar/sidebarview.{h,cpp}`：64px 固定宽，顶部头像占位、中间 3 个按钮占位、底部设置按钮占位。
- `view/mainwindow.{h,cpp}`：`SidebarView` + `QStackedWidget`（含「对话页」一个页面），对话页内部用 `QSplitter` 横向分割左右两块占位。
- 主题切换暂不接，先固定走亮色样式。

**关键步骤**：
1. `MainWindow` 顶层 `QHBoxLayout` 装侧边栏 + 页面容器。
2. 对话页内部：左侧 `QWidget` 留作播放器 + 下方面板，右侧 `QWidget` 留作 ChatView，先用纯色背景标记区域。
3. Splitter 设置最小宽度（右侧 ≥320px，最大 50%）。

**验收**：
- 看到三栏占位、可拖拽中间分割条；最小宽度生效。

**依赖**：M1-T1。

---

### M1-T4　`PlayerService` 封装 SmartPlayer

**目标**：用 `PlayerService` 屏蔽 SDK 回调细节，向上暴露 Qt 信号；实现 open/play/pause/seek/volume/speed/mute 命令。

**输入**：`architecture-design.md` §3.3.2 `PlayerService` 接口定义、SDK 头文件签名。

**输出**：
- `service/playerservice.{h,cpp}`，内部 `CallbackBridge` 实现 `SmartPlayerCallback`，把帧/位置/状态事件转 Qt 信号。
- 关键信号：`positionChanged / durationChanged / stateChanged / frameDecoded / mediaInfoReady / errorOccurred / openResult`。
- 关键缓存：`m_lastFrame`（加锁），用于 `lastDecodedFrame()`。
- `captureFrameAt(posMs, timeoutMs)` 留接口骨架但暂不实现（M2 用），返回失败 future。

**关键步骤**：
1. `CallbackBridge` 持有 `PlayerService*` 弱引用，回调里 `QMetaObject::invokeMethod` 或直接 `emit`（连接用 `QueuedConnection`）。
2. `onVideoFrame` 中：调用 `ImageProcessor::fromVideoFrame`（M1-T5 一起做）→ 更新 `m_lastFrame` → emit `frameDecoded`。
3. open/play/pause/seek 等命令做空指针保护。

**验收**：
- 在临时按钮里调用 `open(path)` + `play()`，能观察到 `positionChanged` 信号按预期触发（先用 qDebug 打印）。

**依赖**：M1-T2。

---

### M1-T5　`ImageProcessor::fromVideoFrame` + `VideoRenderWidget`

**目标**：把 SDK 给的帧数据转成 `QImage`，用自绘 widget 画出来。

**输入**：SDK 实际帧格式（YUV / RGB / SmartPixelFormat 枚举）。

**输出**：
- `infrastructure/imageprocessor.{h,cpp}`：至少实现 `fromVideoFrame()`，按 SDK 格式分支转 `QImage::Format_RGB32` 或 `RGBA8888`。
- `view/player/videorenderwidget.{h,cpp}`：保存当前帧、重写 `paintEvent` 用 `QPainter::drawImage` 按 KeepAspectRatio 缩放绘制。
- `PlayerService::frameDecoded` → `VideoRenderWidget::updateFrame`（通过 `PlayerViewModel::frameReady` 中转，先临时直连也可）。

**关键步骤**：
1. 若 SDK 是 YUV，先简单走 `sws_scale` 或借用 SDK 现成的转 RGB 接口；不追求最优性能。
2. 渲染层做帧节流：UI 刷新最高 30fps（用 `QElapsedTimer` 节流即可）。

**验收**：
- 调用 `open + play` 后能在 widget 上看到流畅画面。
- 窗口缩放时画面按比例缩放，不变形。

**依赖**：M1-T4。

---

### M1-T6　`PlayerControlBar`（控制栏）

**目标**：实现进度条 + 播放/暂停 + 时长显示 + 音量 + 倍速 + 全屏占位。

**输入**：`client_ui_design.md`。

**输出**：
- `view/player/playercontrolbar.{h,cpp}`，浮在视频底部（先用普通子 widget 即可，浮层效果放后面）。
- 控件：`QSlider`（位置）、`QToolButton`（播放/暂停切换）、`QLabel`（mm:ss/mm:ss）、`QSlider`（音量）、`QComboBox`（倍速 0.5/1/1.25/1.5/2）。
- 信号：`seekRequested / playClicked / volumeChanged / speedChanged / muteClicked`。

**关键步骤**：
1. seek 用 `sliderReleased` + `valueChanged` 联合实现：拖动时只更新显示，松手才真正 seek，避免风暴。
2. 位置/时长由 `PlayerViewModel` 推送，禁止控制栏直接调 `PlayerService`。

**验收**：
- 播放中进度条会前进；拖动后能跳到目标时间；暂停/恢复正常；音量/倍速生效。

**依赖**：M1-T5。

---

### M1-T7　`PlayerViewModel` 接入

**目标**：把 `PlayerView`（即 `VideoRenderWidget + PlayerControlBar`）与 `PlayerService` 通过 ViewModel 绑定，View 不再直接持有 Service。

**输入**：`architecture-design.md` §3.2 `PlayerViewModel`。

**输出**：
- `viewmodel/playerviewmodel.{h,cpp}`：实现属性 `position/duration/state/volume/speed/muted/mediaTitle`，命令 `openFile/togglePlay/seek/setVolume/setSpeed/setMute`。
- `view/player/playerview.{h,cpp}`：组合 `VideoRenderWidget + PlayerControlBar`，`setViewModel(PlayerViewModel*)` 中完成所有 connect（参考架构文档 §3.1 `bindViewModel` 示例）。
- `MainWindow` 接收 DI 容器注入的 VM，调用 `setViewModel`。

**关键步骤**：
1. 先实现一个**最小** `DIContainer`：仅装 `PlayerService + PlayerViewModel`，其他后续补。
2. VM 暂不监听 `EventBus`（M2 才会有跨 VM 通信需求）。
3. 临时菜单项「打开文件」走 `QFileDialog::getOpenFileName` → `vm->openFile(path)`。

**验收**：
- 通过菜单打开 mp4，能完整完成 M1 出口标准（播放/暂停/seek/音量/倍速/退出无崩溃）。

**依赖**：M1-T3、M1-T6。

---

### M1-T8　`SidebarView` 完成视觉

**目标**：完成左侧导航 UI（暂不接路由切换，纯视觉 + hover）。

**输入**：`client_ui_design.md` §1。

**输出**：
- 头像（占位圆）、3 个 24px 图标按钮、底部设置按钮；激活态左侧 3px 蓝条。
- 引入 `resources/icons/` 与 `resources.qrc`，至少含 chat / files / settings / avatar 4 个 SVG。

**关键步骤**：
1. 用 `QToolButton + setCheckable(true)` + `QButtonGroup` 互斥。
2. 激活态指示条用 `paintEvent` 在按钮左侧画 3px 矩形，避免改 QSS。

**验收**：
- hover / pressed / checked 视觉正确；点击不同按钮指示条切换。

**依赖**：M1-T3。

---

### M1 出口检查清单

- [ ] 打开 mp4 流畅播放，进度/音量/倍速控制全部可用
- [ ] 三栏布局可拖拽，最小宽度生效
- [ ] 退出无崩溃，无明显内存泄漏（任务管理器观察）
- [ ] git tag `v0.1.0-M1`

---

## M2 — 单帧 AI 问答（核心闭环）

> 出口标准：暂停在某一帧 → 点击「📷 当前帧」 → 输入问题 → AI 流式回复 Markdown；重启应用历史对话仍在。

---

### M2-T1　`NetworkClient` + SSE 流式封装

**目标**：封装 HTTP POST 与 SSE 流式 POST 两种调用，处理 chunk 不完整、`[DONE]` 终止符、SSE 注释行。

**输入**：`api-protocol.md` §2.4 / §7.1 / §7.2。

**输出**：
- `infrastructure/networkclient.{h,cpp}`：`post()` + `streamPost(url, body, onChunk, onDone, onError) + cancelStream()`。
- 流式实现要点：监听 `QNetworkReply::readyRead`；维护内部 buffer 处理跨 chunk 的 `\n\n` 分割；忽略 `:` 开头注释；解析 `data:` 后 JSON 或 `[DONE]`。
- 60s 传输超时；`Authorization: Bearer <key.trimmed()>`。

**关键步骤**：
1. 注意 Qt 的 `QNetworkAccessManager` 必须在创建线程使用（这里就是主线程）。
2. `cancelStream()` 必须能立即终止——`reply->abort()` + 主动断开信号。

**验收**：
- 写一个临时 demo：直接 POST OpenAI Compatible 接口，能逐字打印增量。
- 中断按钮可以立刻停止。

**依赖**：M1 完成。

---

### M2-T2　`AgentService::sendMessage`（无 Tool 版本）

**目标**：根据 conversation_id / text / frames 拼出 OpenAI Compatible 请求体并通过 `NetworkClient` 发出；通过信号把 chunk / 完成 / 错误广播出去。

**输入**：`api-protocol.md` §二、§四 System Prompt 模板（M2 阶段先用最小版，不带视频元信息）。

**输出**：
- `service/agentservice.{h,cpp}`：`sendMessage(convId, text, frames, videoCtx={})` / `stopGeneration()` / `setModel / setEndpoint`。
- 信号：`responseChunk(convId, delta) / responseFinished(convId, fullMsg) / responseError(convId, err)`。
- `buildRequestPayload`：构造 `messages` 数组、对 `frames` 调 `ImageProcessor::toBase64Jpeg`（≤1024 边长、quality 80）。
- 暂不带 `tools` 字段。

**关键步骤**：
1. **API Key 从 `SettingsService::secretGet("secret.llm.api_key")` 取**，禁止 hardcode、禁止落库、禁止打印。
2. 单次请求图片数硬上限 ≤10（架构防御）。
3. `stopGeneration()` 直接转 `NetworkClient::cancelStream()`。

**验收**：
- 调用 `sendMessage("conv-1", "你好")` 能收到流式回复。
- 调用 `sendMessage(..., frames=[QImage])` 后多模态回复正常。

**依赖**：M2-T1。

---

### M2-T3　`DatabaseManager` + Schema 初始化

**目标**：建立 SQLite，初始化 `conversations / messages / settings / recent_files / analysis_cache` 表（与 `architecture-design.md` §八 一致）。

**输出**：
- `infrastructure/databasemanager.{h,cpp}`：单例式 `instance()`、`initialize(path)`、`exec / query` 通用 API。
- DB 文件位置：`QStandardPaths::AppDataLocation + "/agent.db"`。
- 启动时建表幂等（`CREATE TABLE IF NOT EXISTS`）；同时建索引。

**关键步骤**：
1. 所有动态参数走绑定，**禁止字符串拼 SQL**（防注入）。
2. `messages.attached_frames` 存 JSON 数组（base64 缩略图，可选）。

**验收**：
- 首次启动生成 db；二次启动不重复建表；用 DB Browser 看到 5 张表。

**依赖**：M1 完成。

---

### M2-T4　`ConversationService` + 持久化

**目标**：会话与消息的 CRUD。

**输出**：
- `service/conversationservice.{h,cpp}`：`getAllConversations / createConversation / deleteConversation / updateTitle / getMessages / saveMessage / updateMessage`。
- 所有方法都走 `DatabaseManager` 的绑定接口。

**验收**：
- 单元手测：创建对话 → saveMessage → 重启 → getMessages 仍能拿到。

**依赖**：M2-T3。

---

### M2-T5　`ChatViewModel` + `ChatMessageListModel`

**目标**：把消息列表暴露给 View，处理流式片段累积与持久化。

**输出**：
- `viewmodel/chatviewmodel.{h,cpp}`：实现 `sendMessage(text)` / `sendMessageWithFrame(text, frame)` / `stopGeneration / regenerateLastResponse / createNewConversation / switchConversation / deleteConversation / onTimestampClicked`。
- 流式 chunk 累积：收到 `responseChunk` → 追加到当前 assistant 消息的 content → emit `messageUpdated(index)`；最多 30fps 节流。
- `responseFinished` → 标记非 streaming → 通过 `ConversationService::saveMessage` 落库。
- `ChatMessageListModel`（`QAbstractListModel`），roles 包含 `role / content / timestamp / isStreaming / attachedFrames`。

**验收**：
- 发送一条文本消息，能看到对应 assistant 气泡逐字增长。
- 中断按钮能停止生成；重启后历史消息仍在。

**依赖**：M2-T2、M2-T4。

---

### M2-T6　`ChatView` + 气泡 + 输入框

**目标**：UI 呈现对话列表与输入区，支持「📷 当前帧」按钮。

**输出**：
- `view/chat/chatview.{h,cpp}`：组合 `ChatMessageList + ChatInputWidget + 折叠按钮`。
- `chatmessagelist.{h,cpp}`：`QListView + 自定义 delegate` 渲染气泡；自动滚动到底（仅在已贴底时）。
- `chatbubblewidget.{h,cpp}`：根据 `role` 渲染左右两侧不同背景；附带帧缩略图横排展示。
- `chatinputwidget.{h,cpp}`：多行 `QTextEdit` + 「📷 当前帧」`QToolButton` + 「发送 / 停止」按钮（流式中切换为「停止」）。

**关键步骤**：
1. Markdown 渲染先用 `QTextDocument::setMarkdown`（Qt 自带，够用），后续可换 `cmark-gfm`。
2. 输入框 Enter=发送 / Shift+Enter=换行。

**验收**：
- 输入文本 → 发送 → 回复流畅显示 + Markdown 格式正确（标题、代码块、列表）。

**依赖**：M2-T5。

---

### M2-T7　`EventBus::frameForAIRequested` + 截帧上送链路

**目标**：实现「📷 当前帧」核心交互：ChatView 触发截帧请求 → PlayerVM 截帧 → ChatVM 拿到 QImage 发出。

**输入**：`architecture-design.md` §7.2 跨 VM 通信约定。

**输出**：
- `EventBus` 新增信号：`frameForAIRequested(int64_t posMs)` / `screenshotForAI(QImage img, int64_t ts)`。
- `ChatViewModel::sendMessageWithFrame` 流程：先 emit `frameForAIRequested(currentPos=-1 表示用当前)` → 监听 `screenshotForAI` 回包 → 拼帧后调 `AgentService::sendMessage`。
- `PlayerViewModel` 监听 `frameForAIRequested`：使用 `PlayerService::lastDecodedFrame()` 直接取；若空再退化为 `captureFrameAt`（本 M 先只支持当前帧路径）。

**验收**：
- 暂停在任意一帧，点击「📷 当前帧」并输入「这一帧画面里有什么？」，AI 能基于该帧回答。

**依赖**：M2-T2、M2-T6、M1-T7。

---

### M2-T8　对话历史侧边下拉

**目标**：在 ChatView 顶部加一个「会话切换」入口（下拉或弹层），列出最近会话；可新建 / 切换 / 删除。

**输出**：
- `ChatViewModel::createNewConversation / switchConversation / deleteConversation` 接入 UI。
- 切换会话时清空当前 listmodel，从 DB 重新载入消息。

**验收**：
- 新建对话 → 发几条消息 → 切换到旧对话 → 再切回来，内容一致。

**依赖**：M2-T5、M2-T6。

---

### M2 出口检查清单

- [ ] 单帧问答完整链路可用
- [ ] 流式过程 UI 不卡顿（节流生效）
- [ ] 停止按钮可立即中断生成
- [ ] 历史对话持久化、可切换
- [ ] git tag `v0.2.0-M2`

---

## M3 — 视频整体理解

> 出口标准：打开新视频几秒内能回答"这视频讲了什么"；AI 回复中的 `[mm:ss]` 时间戳可点击跳转。

---

### M3-T1　简易场景分割（直方图差异）

**目标**：在不引入大模型的前提下，对视频做粗粒度场景分割，产出 `[{scene_id, start_ms, end_ms}]`。

**输出**：
- `service/videoanalysis/scenedetector.{h,cpp}`：输入 `videoPath`，输出场景列表。
- 实现：每 N 秒（如 1s）取一帧 → 计算 HSV 直方图 → 与前一帧比较，差异 > 阈值则视为场景切换。
- 工程上：通过 `PlayerService` 的 `captureFrameAt`（M3-T2 完整实现后接入）或单独走 FFmpeg 子模块解码（取决于 SDK 是否方便）。

**关键步骤**：
1. 先用最简方案：固定每秒采样 + 阈值；阈值开放为常量便于调。
2. 单元手测：用 1~2 段已知场景的视频对比结果。

**验收**：
- 一段含 3~5 个明显切换的视频能切出接近预期的场景数（±1 即可）。

**依赖**：M1 完成 + `PlayerService` 截帧能力（见 M3-T2）。

---

### M3-T2　`PlayerService::captureFrameAt` 实现

**目标**：完成异步「按时间点截帧」能力（SDK seek 是异步的，需等待目标时间附近的新帧）。

**输出**：
- `PlayerService::captureFrameAt(posMs, timeoutMs)` 返回 `QFuture<QImage>`。
- 内部状态机：发起 seek → 监听 `frameDecoded` → 比对帧时间戳与 `posMs` 容差（如 ±200ms）→ 满足则完成；超时则 future 抛错。

**关键步骤**：
1. 用 `QFutureInterface<QImage>` 手动管理。
2. 若用户正在播放，应保留播放状态；截完帧后恢复（或文档明示会暂停）。
3. 并发安全：同一时间允许多个 captureFrameAt 排队，串行执行。

**验收**：
- 顺序请求 5 个不同时间点 → 全部 future 在 timeout 内完成且帧时间戳接近目标。

**依赖**：M1-T4。

---

### M3-T3　`VideoIndexer`（后台渐进式索引）

**目标**：异步对新打开的视频做 Level 0/1 索引（元信息 + 场景分割 + 关键帧抽取）。

**输入**：`agent-core-design.md` §5.2 渐进式理解。

**输出**：
- `service/videoanalysis/videoindexer.{h,cpp}`：`startIndex(videoPath)`、`progress(int %, QString stage)` 信号、`indexReady` 信号（携带场景列表与关键帧路径）。
- 关键帧落盘到 `appData/keyframes/<videoHash>/scene_<id>.jpg`；元信息 + 场景写入 `analysis_cache` 表（`analysis_type='scene_split'`）。
- 视频 hash：基于文件大小 + 头 1MB hash（不读全文件，足够区分修改）。

**关键步骤**：
1. 跑在 `QThreadPool` 的工作任务里，所有信号回主线程。
2. 同一视频 hash 命中缓存时直接跳过重做。

**验收**：
- 打开 10 分钟视频，几秒后能在日志看到 "indexReady"；缓存命中后秒级返回。

**依赖**：M3-T1、M3-T2。

---

### M3-T4　VLM 场景描述 + 视频摘要

**目标**：对每个场景调用 VLM 生成结构化描述，再用 LLM 汇总成视频摘要；写入数据库。

**输入**：`agent-core-design.md` §3.2 SCENE_DESCRIPTION_PROMPT、VIDEO_SUMMARY_PROMPT。

**输出**：
- `service/videoanalysisservice.{h,cpp}`：基于 `AgentService` 复用 OpenAI Compatible 通道，新增 `describeScene(sceneId, frames)` / `summarizeVideo()`。
- 场景描述 JSON 落库（`analysis_cache` 中 `analysis_type='scene_desc'`）。
- 摘要文本落 `analysis_cache`（`analysis_type='video_summary'`）。
- 进度通过 `analysisProgress(percent, stage)` 信号上报。

**关键步骤**：
1. 每个场景送给 VLM 的关键帧 ≤3 张，base64 后 ≤300KB/张。
2. 失败一个场景不阻塞整体；记录 warning，可重试。
3. 安全：调用前确认有 API Key；否则提示用户去设置。

**验收**：
- 打开测试视频后，10~30s 内能在日志或临时 UI 看到完整摘要文本。
- 摘要质量主观合理（覆盖主要场景）。

**依赖**：M3-T3、M2-T2。

---

### M3-T5　把视频上下文注入 system prompt

**目标**：每次发起对话时，按 `api-protocol.md` §4.1 模板拼装 system prompt，包含元信息 + 场景概览 + 摘要。

**输出**：
- `AgentService::buildRequestPayload` 增加 `VideoContext` 参数（文件名/时长/分辨率/fps/has_audio/scene_overview/video_summary）。
- `ChatViewModel` 在每次 `sendMessage` 前从 `VideoAnalysisService` 拉最新 ctx 注入。

**关键步骤**：
1. 控制 system prompt 长度：场景描述总长 > 阈值时截断，保留前 N 个 + 末 M 个（首尾 U 形）。

**验收**：
- 打开视频后立刻问「这视频讲了什么」，回复明显引用了摘要中的内容。

**依赖**：M3-T4、M2-T2。

---

### M3-T6　AI 回复中 `[mm:ss]` 时间戳点击跳转

**目标**：在 `ChatBubbleWidget` 渲染 Markdown 后，识别 `[mm:ss]` / `[hh:mm:ss]` 模式，渲染为可点击链接 → emit `timestampClicked(posMs)` → ChatVM → `EventBus::seekToPosition` → PlayerVM → seek。

**输入**：`api-protocol.md` §4.2、`architecture-design.md` §4.1。

**输出**：
- `ChatBubbleWidget`：解析正则 `\[((\d{1,2}):)?\d{1,2}:\d{2}\]`，在 `QTextBrowser` / `QTextDocument` 里用 `<a href="ts://120000">` 包装；监听 `anchorClicked`。
- `EventBus` 新增/沿用 `seekToPosition(int64_t)`；`PlayerViewModel` 构造时 connect。

**关键步骤**：
1. 仅识别有效范围内的时间戳（不超过当前视频 duration），超出的不可点。

**验收**：
- AI 回复中点击 `[01:23]`，播放器跳转到 1:23 并继续播放/暂停状态保持。

**依赖**：M3-T5、M1-T7。

---

### M3-T7　索引进度 UI

**目标**：右上角或 ChatView 顶部显示"正在分析视频..."进度条，索引完成后消失；失败时显示重试按钮。

**输出**：
- ChatView 顶部加一个可隐藏的状态条，绑定 `VideoAnalysisService::analysisProgress / analysisCompleted / errorOccurred`。

**验收**：
- 索引中可见进度；完成后状态条收起；中途网络错误能给提示。

**依赖**：M3-T4。

---

### M3 出口检查清单

- [ ] 打开视频几秒后能回答全局问题
- [ ] 时间戳点击跳转链路稳定
- [ ] 缓存命中（再次打开同一视频）秒级生效
- [ ] git tag `v0.3.0-M3`

---

## M4 — RAG + Tool Calling

> 出口标准：长视频（30+ 分钟）能精确定位 "X 在什么时候"，能跟踪 "那个 X 的人做了什么"；6 个 Tool 全部可用。

---

### M4-T1　依赖：FAISS / ONNX Runtime / Whisper 预编译入仓

**目标**：把第三方依赖准备齐全，CMake 链接通。

**输出**：
- `third_party/faiss/`、`third_party/onnxruntime/`、`third_party/whisper.cpp/`（按 `development-plan.md` §4.4）。
- 对应 CMake 模块；运行所需 dll 拷到输出目录。
- `models/` 目录与 `.gitignore`；准备好 `clip-vit-b32.onnx` / `bge-small-zh.onnx` / `whisper-base.bin` 三个模型文件（不进 git）。

**验收**：
- 写一个临时 demo：CLIP 编码一张图、BGE 编码一句话、Whisper 转写 5 秒音频，分别成功。

**依赖**：M3 完成。

---

### M4-T2　`VideoRAGStore`（FAISS + SQLite metadata）

**目标**：实现 `agent-core-design.md` §9.2 描述的 4 个集合 `visual_frames / text_segments / entity_profiles / qa_cache`，提供 insert / search / delete。

**输出**：
- `rag/videoragstore.{h,cpp}`：每集合一个 `faiss::IndexFlatIP` 或 `IndexHNSWFlat`，payload 存 SQLite 新表 `rag_chunks(collection, chunk_id, video_id, start_ms, end_ms, text_content, metadata_json)`。
- 支持 metadata 过滤：`search(collection, vector, filter, topK)`。
- 持久化：FAISS 索引按 `video_id` 落盘 `appData/rag/<video_id>/<collection>.index`。

**关键步骤**：
1. 同一 collection 跨视频共享一个 index 或按视频分库——本项目按视频分库（数据小、便于失效清理）。
2. 视频文件 hash 变更 → `IndexLifecycleManager::invalidate_index`。

**验收**：
- 写入 100 条向量后能正确检索 topK，并支持 `start_ms` / `end_ms` 范围过滤。

**依赖**：M4-T1、M3-T3。

---

### M4-T3　集成 CLIP / BGE / Whisper 推理

**目标**：在 C++ 端调通三个本地模型推理。

**输出**：
- `rag/encoders/clipencoder.{h,cpp}`：`encodeImage(QImage)`、`encodeText(QString)` 返回 768-d。
- `rag/encoders/textencoder.{h,cpp}`：BGE，返回 1024-d（或所用模型实际维度）。
- `rag/encoders/whispertranscriber.{h,cpp}`：输入音频/视频文件路径 → 分段输出 `SubtitleEntry` 列表。
- 所有编码器线程安全，可被 `QThreadPool` 调用。

**关键步骤**：
1. 模型路径走 `SettingsService`，缺失时给清晰提示。
2. Whisper 走 whisper.cpp 的 C API 包一层即可，先用 base 模型。
3. 安全：模型文件路径校验，禁止外部用户传入未受信路径触发 SSRF/LFI。

**验收**：
- 三个 demo 都通过；编码耗时记录到日志。

**依赖**：M4-T1。

---

### M4-T4　扩展 `VideoIndexer` 到 Level 1（embedding + ASR）

**目标**：场景分割之后，自动跑关键帧 CLIP 编码 + Whisper 转写，结果写入 `VideoRAGStore`。

**输出**：
- `VideoIndexer` 在 Level 1 阶段：对每个关键帧调 CLIP → 写 `visual_frames`；对全视频调 Whisper → 按句子拆 chunk → 写 `text_segments`（同时把场景描述也写入）。
- 进度上报区分 stage：`"scene_split" / "encode_frames" / "transcribe" / "scene_desc"`。

**验收**：
- 索引完成后，`SELECT COUNT(*) FROM rag_chunks WHERE video_id=...` 数量合理。
- 用一句中文搜场景能召回正确时间段。

**依赖**：M4-T2、M4-T3、M3-T3。

---

### M4-T5　`VideoRAGRetriever`（多路检索 + RRF 融合）

**目标**：实现 `agent-core-design.md` §9.4 多路召回与 RRF 融合。

**输出**：
- `rag/videoragretriever.{h,cpp}`：`retrieve(query, videoId, constraints, topK)` 返回融合排序后的 chunk 列表。
- 多路：text（BGE）/ visual（CLIP text encoder）/ entity / qa_cache。
- 融合：RRF（默认 k=60），权重按 `analyze_query` 简单规则给定。

**验收**：
- 给定一个测试 query，能在合理 top-5 中包含人工标注的 ground truth chunk。

**依赖**：M4-T4。

---

### M4-T6　`ToolOrchestrator` + 6 个 Tool 实现

**目标**：实现 `api-protocol.md` §3 的 Tool Calling 解析与执行循环；6 个 Tool 全部接通底层 Service。

**输出**：
- `service/toolorchestrator.{h,cpp}`：最大 5 轮；逐轮：从 `AgentService` 收到 `finish_reason=tool_calls` → 解析 → 执行 → 把结果作为 `role=tool` 消息追加 → 再次发起 LLM 请求。
- 各 Tool 实现：
  - `seek_and_analyze` → `PlayerService::captureFrameAt` + VLM `describeFrame`
  - `analyze_time_range` → 区间多帧采样 + VLM 多帧联合推理
  - `search_video_content` → `VideoRAGRetriever`
  - `get_transcript` → 查 `rag_chunks where chunk_type='speech_segment' and overlap range`
  - `get_scene_info` → 查 `analysis_cache` 中场景描述
  - `control_player` → `EventBus::seekToPosition` 等（不阻塞 / 不分析）
- `AgentService::sendMessage` 在 M4 改为带 `tools` 字段，由 `ToolOrchestrator` 管理整个多轮循环。

**关键步骤**：
1. 工具调用的增量 `arguments` 需要拼接，注意 chunk 边界。
2. 单次回答最多 3 次工具调用（与 system prompt 中的约束一致）。
3. 失败工具结果用结构化错误回填，让模型有机会修正。

**验收**：
- 提问"什么时候出现 X"能命中 `search_video_content` 并跳转。
- 提问"那个穿蓝色衣服的人做了什么"能触发 search + analyze_time_range 组合。
- 工具循环不会失控（5 轮硬限）。

**依赖**：M4-T5、M3-T4。

---

### M4-T7　QA 缓存

**目标**：每次成功问答后写入 `qa_cache`；下一次相似问题（>0.88 阈值）直接复用回答（标记"历史结论"）。

**输出**：
- `rag/qacachemanager.{h,cpp}`：`cacheQAPair` / `tryAnswerFromCache`。
- `ChatViewModel` 在 `sendMessage` 入口先查缓存；命中则直接渲染历史回答 + 标签提示，仍允许用户「重新分析」覆盖。

**验收**：
- 同一个问题连问两次：第二次秒回 + "[历史分析结论]" 标记。

**依赖**：M4-T6。

---

### M4-T8　`AnalysisPanelView` 的「字幕 Tab」

**目标**：把 Whisper 转写结果以列表呈现，点击行 → 跳转。

**输出**：
- `view/analysis/subtitlelistwidget.{h,cpp}`、`viewmodel/analysisviewmodel.{h,cpp}` 的 SubtitleModel。
- 通过 `EventBus::seekToPosition` 完成跳转。

**验收**：
- 视频含语音时，字幕 Tab 列出条目；点击跳转正确。

**依赖**：M4-T4。

---

### M4 出口检查清单

- [ ] 长视频精准检索可用
- [ ] 6 个 Tool 全部能被 LLM 正确调用并返回
- [ ] QA 缓存命中率可观察（日志）
- [ ] 字幕 Tab 可用
- [ ] git tag `v0.4.0-M4`

---

## M5 — 打磨上线

> 出口标准：三种主题切换无闪烁；设置页可改 API / 主题 / 播放器选项；可打成安装包发布。

---

### M5-T1　`ThemeService` + QSS 加载

**目标**：实现 `client_ui_design.md` 的暗色/亮色/跟随系统三模式。

**输出**：
- `service/themeservice.{h,cpp}`：`setThemeMode / themeMode / isDark / applyTheme / color(token)`。
- `resources/styles/{dark.qss, light.qss, common.qss}`，色值对齐 `client_ui_design.md` 「完整色值定义」。
- 跟随系统：监听 `QGuiApplication::styleHints()->colorSchemeChanged`（Qt 6.5+）。

**关键步骤**：
1. QSS 用变量？Qt 不原生支持——可在加载时做简单 token 替换。
2. 切换时 200ms `QPropertyAnimation` 过渡（淡入淡出主窗口 opacity）。

**验收**：
- 切换三种模式无闪烁；持久化 + 重启恢复。

**依赖**：M2 完成。

---

### M5-T2　`SettingsService` + `SecretStore` 三平台实现

**目标**：完成设置项持久化与 OS 密钥管理抽象。

**输出**：
- `service/settingsservice.{h,cpp}`：所有非敏感配置走 SQLite `settings` 表。
- `infrastructure/secretstore_{win,mac,linux}.cpp` 三平台实现，统一接口 `secretGet / secretSet / secretDelete`。
- API Key 等机密走 SecretStore，**禁止**进 SQLite / 日志（与 `api-protocol.md` §8.2、安全规则一致）。

**关键步骤**：
1. Windows DPAPI：`CryptProtectData`（`CRYPTPROTECT_LOCAL_MACHINE=false`）；macOS Keychain：`SecItemAdd/Copy`；Linux libsecret。
2. Mac/Linux 在本项目首版可只 stub（仅 Windows 实现），但接口必须留。

**验收**：
- 设置 API Key → 退出重启 → 仍可发起对话；用任何工具查看 SQLite 无明文 key。

**依赖**：M2-T3。

---

### M5-T3　`SettingsView`

**目标**：完整设置页 UI：外观（主题模式三选一）+ AI 配置（endpoint / model / api key / temperature / max_tokens）+ 播放器（硬解开关）+ 数据（清空对话 / 清空 RAG 索引）。

**输出**：
- `view/settings/settingsview.{h,cpp}` + `viewmodel/settingsviewmodel.{h,cpp}`。
- API Key 输入框 `setEchoMode(Password)`；保存时调 `SettingsService::secretSet`。

**验收**：
- 改 endpoint / model 后下一次问答生效。
- 清空对话 / RAG 索引功能可用。

**依赖**：M5-T1、M5-T2。

---

### M5-T4　`FileListView`（最近文件 + 收藏）

**输出**：
- `service/filemanagerservice.{h,cpp}`：`scanDirectory / recentFiles / addToRecent / removeFile / getFileInfo`。
- 表 `recent_files` 已在 M2-T3 建好；新增 `favorites` 表（可后续）。
- `view/filelist/filelistview.{h,cpp}` + `viewmodel/filelistviewmodel.{h,cpp}`：卡片式列出，双击打开。
- `SidebarView` 切换到该页时显示。

**验收**：
- 打开过的视频出现在列表；双击能切到对话页并自动 open。

**依赖**：M5-T3、M1-T8、`NavigationViewModel`（首次落地点）。

---

### M5-T5　`AnalysisPanelView` 时间线 + 检测 Tab

**目标**：补齐下方面板剩余两个 Tab。

**输出**：
- 时间线：可视化场景边界 + 关键事件标记点；点击跳转。
- 检测：当前帧的目标检测列表（M5 可暂用占位/简化版——只列 VLM 描述中的 "objects" 字段，等后续真正接 YOLO 再升级）。

**验收**：
- 时间线在打开新视频后能正确展示；点击跳转。

**依赖**：M3-T3、M4-T4（场景描述含 objects）。

---

### M5-T6　错误降级与可见性

**目标**：把 `agent-core-design.md` §10 的降级策略落到 UI 与日志。

**输出**：
- 统一的 `Toast` / `Banner` 组件，按 `api-protocol.md` §6.4 错误展示规范实现。
- 关键降级：API 失败重试 1 次；429 退避（最多 3 次）；流式中断显示「已中断 / 重新生成」；视频打开失败中央提示。
- `Logger`（spdlog 包装）+ 日志脱敏（API Key / Bearer）。

**验收**：
- 故意配错 API Key → 弹窗跳设置；网络断开 → 红色 banner；流式中途断网 → 提示中断 + 重生成。

**依赖**：M5-T2、M2-T1。

---

### M5-T7　打包与发布

**目标**：在 Windows 上能输出可分发的安装包。

**输出**：
- `scripts/deploy_windows.ps1`：调用 `windeployqt` 收集运行时；拷贝 SDK / ONNX dll / 模型；输出到 `dist/`。
- `installer/installer.nsi`（或 Inno Setup）：生成单 exe 安装包；含图标 / 卸载项 / 起始菜单快捷方式。
- 应用图标 `.ico`；`resources.qrc` 整理。

**验收**：
- 在一台**未装 Qt** 的干净 Windows 上双击安装、运行、能开视频问 AI。

**依赖**：M1~M5 其余全部完成。

---

### M5-T8　README + 截图 + CHANGELOG

**输出**：
- 顶层 README：项目简介 / 截图 / 编译说明 / 使用指南 / 已知限制。
- `CHANGELOG.md`：M1~M5 关键变更与版本号。

**验收**：
- README 截图清晰，能让陌生人 5 分钟内跑起来 demo。

**依赖**：M5-T7。

---

### M5 出口检查清单

- [ ] 主题三模式无闪烁切换
- [ ] 安装包能在干净环境运行
- [ ] 所有 P0 + P1 功能可用
- [ ] git tag `v1.0.0`

---

## 附录 A：每张卡完成后的"提交清单"

每完成一张卡，按以下步骤交付（避免欠债）：

1. 通过编译（Debug + Release）
2. 手动跑一遍验收项
3. 给该卡涉及的关键代码加 1~3 行注释/doxygen
4. `git commit -m "M{x}-T{y}: <简述>"`
5. 在本文件该卡前打勾 `[x]`

## 附录 B：什么时候允许偏离本计划

- 走通主链路被某张卡卡住 ≥3 天 → 允许跳过该卡走兜底（如 M3-T1 场景分割准度差 → 直接均匀采样）
- 发现新的 P0 缺陷 → 优先修，但要记录到本文件「插队任务」节
- 任何对 SSoT（架构 / 协议 / Agent 设计）的实际偏离 → 先回去改文档再写代码
