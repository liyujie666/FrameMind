# 开发计划 — 视频分析 AI Agent

> 个人开发，单线推进。本文档定义 MVP 范围、里程碑节奏、技术栈与依赖版本。
> 原则：**先跑通主链路，再做深度优化**。

---

## 〇、项目关系说明

本项目即 **`FrameMind`**——基于 `player_sdk`（已有的 FFmpeg + SDL2 视频播放 SDK）构建的上层 Qt 客户端应用。当前仓库根目录为 `Frame_Mind/`，应用代号统一使用 **`FrameMind`**。

| 组成 | 角色 | 状态 |
|------|------|------|
| `FrameMind`（当前仓库） | 上层 Qt 客户端应用 + 文档 + 资源 | 待开发 |
| `player_sdk`（外部依赖） | 底层视频播放 SDK，作为预编译产物（include + lib + dll）引入 `third_party/smartplayer_sdk/` | 已有 |

本开发计划描述的所有里程碑、目录结构、模块均属于 **`FrameMind`** 客户端项目。

---

## 一、MVP 范围定义

### 1.1 必须做（P0，尚未开始）

- [ ] 基础视频播放（基于已有 SmartPlayer SDK）
  - 打开本地视频文件
  - 播放/暂停/进度条 seek
  - 音量调节、倍速调节
  - Qt 自定义 widget 渲染 `onVideoFrame` 帧
- [ ] AI 对话基础能力
  - 流式输出 (SSE)
  - Markdown 渲染
  - 多轮对话
  - 对话历史持久化 (SQLite)
- [ ] 视频 + AI 联动核心能力（**这是项目的灵魂**）
  - 一键截取当前帧发给 AI 提问
  - AI 回复中的时间戳可点击跳转播放器
  - 全视频摘要（视频加载后异步生成）
- [ ] 主题切换（暗色/亮色/跟随系统）
- [ ] 设置页（AI 模型配置、播放器配置）

### 1.2 第二步做（P1）

- [ ] Video RAG 系统
  - 场景分割 + 关键帧 embedding
  - 语音转文字 (Whisper)
  - 多路检索（文本 + 视觉）
- [ ] Agent Tool Calling
  - seek_and_analyze
  - analyze_time_range
  - search_video_content
- [ ] 下方面板（时间线 / 字幕 / 检测）
- [ ] 文件列表页（最近打开 + 收藏）

### 1.3 暂不做（P2，明确划出去）

- ❌ 跨视频知识关联
- ❌ 实时流分析（RTSP 监控场景）
- ❌ 视频剪辑/导出
- ❌ 多说话人分离
- ❌ 知识图谱可视化
- ❌ 多 Agent 协作
- ❌ 跨平台适配（先专注 Windows，macOS/Linux 后续）
- ❌ 自动化评估流水线
- ❌ 复杂的内容安全审核

> **重要**：每次想加新功能时，问自己：核心问答链路能跑了吗？如果还没跑，先不加。

---

## 二、里程碑规划

### M1: 骨架 (预计 1 周)

**目标**：跑出一个能播放视频 + 显示空白对话框的 Qt 应用。

**任务**：
- [ ] 新建 Qt 6.9 项目，CMake 配置
- [ ] 集成 SmartPlayer SDK（拷贝头文件 + lib + dll）
- [ ] 实现 `MainWindow` 三栏布局（侧边栏 + 视频区 + 对话区）
- [ ] 实现 `PlayerService` 封装 SmartPlayer，跨线程传递 QImage
- [ ] 实现 `VideoRenderWidget`（paintEvent 绘制 QImage）
- [ ] 实现 `PlayerControlBar`（播放/暂停/进度条/音量/倍速）
- [ ] 实现 `PlayerViewModel`，View 通过信号槽绑定
- [ ] 实现 `SidebarView`（头像 + 3 个图标按钮，先不实现切换）

**验收**：
- 能打开 mp4 文件并流畅播放
- 进度条可拖动 seek
- 暂停/恢复/调音量/调倍速正常
- 退出无崩溃

**风险点**：
- SDK 的 `onVideoFrame` 在解码线程，QImage 跨线程传输要用 `Qt::QueuedConnection`
- 帧率高时 UI 可能跟不上，可能要做帧丢弃

---

### M2: 单帧 AI 问答 (预计 1 周)

**目标**：用户点击"📷 当前帧"按钮，AI 流式回复关于当前画面的问题。

**任务**：
- [ ] 实现 `NetworkClient`（QNetworkAccessManager 封装）
- [ ] 实现 SSE 流式接收（QNetworkReply::readyRead 增量解析）
- [ ] 实现 `AgentService::sendMessage`（OpenAI Compatible 格式）
- [ ] 实现 `ChatViewModel` + `ChatView` + `ChatMessageList` + `ChatBubbleWidget`
- [ ] 集成 Markdown 渲染（推荐 `cmark-gfm` 或 `QTextDocument` 自带）
- [ ] 实现 `ImageProcessor::fromVideoFrame` 帧数据 → QImage 转换
- [ ] 实现"📷 当前帧"按钮，截取当前帧 + base64 编码送 AI
- [ ] 实现对话持久化（SQLite，conversations + messages 表）
- [ ] 简单的对话历史列表（左上下拉显示历史会话）

**验收**：
- 暂停在某一帧后，问 AI "画面里有什么"，能得到流式回复
- 回复用 Markdown 正确渲染
- 重启应用后历史对话仍可见

**风险点**：
- SSE 解析（处理 `data: ...\n\n` 格式 + chunk 不完整问题）
- 大图片 base64 编码可能很长，超过单次 HTTP body 默认限制
- 流式更新 UI 频率过高时的性能问题（节流到 30fps）

---

### M3: 视频整体理解 (预计 1.5 周)

**目标**：视频加载后异步生成全视频摘要，AI 回复中的时间戳可跳转。

**任务**：
- [ ] 集成场景分割（先用简单方案：每 N 秒一帧 + 直方图差异）
- [ ] 实现 `VideoIndexer`（后台线程，渐进式构建视频表示）
- [ ] 接入一个 VLM API（OpenAI / 通义千问 / Qwen-VL 等）做场景描述
- [ ] 生成全视频摘要并存入数据库
- [ ] 把视频摘要 + 场景列表注入 system prompt
- [ ] 实现 AI 回复中 `[mm:ss]` 时间戳的点击跳转
- [ ] 加上 loading 状态指示（"正在分析视频..."）

**验收**：
- 打开新视频后，几秒内能回答"视频讲了什么"
- AI 回答中提到具体时间点时，点击能跳转到对应位置播放

**风险点**：
- 场景分割算法准确度（先简单后优化）
- VLM API 成本与速度的平衡
- 跨线程的索引进度上报（避免阻塞 UI）

---

### M4: RAG + Tool Calling (预计 2 周)

**目标**：长视频的精准检索与多步推理。

**任务**：
- [ ] 选择向量库：FAISS（推荐，纯本地）
- [ ] 集成 CLIP（ONNX Runtime）做关键帧 embedding
- [ ] 集成文本 embedding 模型（BGE-small 或 OpenAI text-embedding-3-small）
- [ ] 集成 Whisper（ONNX 或 whisper.cpp）做语音转文字
- [ ] 实现 `VideoRAGRetriever`（多路检索 + RRF 融合）
- [ ] 实现 `ToolOrchestrator`（多步 Tool Calling 循环）
- [ ] 实现 6 个核心 Tool（与 `api-protocol.md` §3.1 / `agent-core-design.md` §7.1 完全一致）：
  - `seek_and_analyze`
  - `analyze_time_range`
  - `search_video_content`
  - `get_transcript`
  - `get_scene_info`
  - `control_player`
- [ ] 把 RAG 检索结果注入 Agent 上下文
- [ ] QA 缓存机制（相似问题复用历史回答）
- [ ] 实现下方 `AnalysisPanelView` 的 **字幕 Tab**（Whisper 转写结果，点击跳转）

**验收**：
- 问"什么时候出现了 XX"能定位到具体时间点
- 问"那个红衣服的人做了什么"能识别实体并分析其动作
- 长视频（30 分钟+）也能在合理时间内得到回答

**风险点**：
- ONNX 模型集成（CLIP/Whisper 在 C++ 端的部署）
- Tool Calling 循环失控（最大 5 轮硬限制）
- 向量库与 SQLite 的数据同步

---

### M5: 打磨上线 (预计 1 周)

**目标**：完善设置、主题、错误处理，做一个可发布的版本。

**任务**：
- [ ] 实现 `ThemeService` + 亮色/暗色 QSS
- [ ] 实现 `SettingsView`（API 配置、主题选择、播放器选项）
- [ ] 实现 `FileListView`（最近文件 + 收藏）
- [ ] 完善下方 `AnalysisPanelView` 剩余两个 Tab：**时间线** / **检测**（字幕 Tab 已在 M4 完成）
- [ ] 错误降级（API 失败、视频加载失败、网络中断）
- [ ] 应用图标 + 打包脚本（windeployqt + NSIS）
- [ ] README + 截图

**验收**：
- 三种主题切换无闪烁
- API 配错时有清晰的错误提示
- 双击安装包后能正常使用

---

## 三、总周期估算

| 阶段 | 预计 | 累计 |
|------|------|------|
| M1: 骨架 | 1 周 | 1 周 |
| M2: 单帧问答 | 1 周 | 2 周 |
| M3: 视频整体理解 | 1.5 周 | 3.5 周 |
| M4: RAG + Tool Calling | 2 周 | 5.5 周 |
| M5: 打磨 | 1 周 | 6.5 周 |

> **个人开发现实预估**：每个里程碑实际可能延期 30~50%，所以心里预期 **8~10 周**完成 MVP 是合理的。

---

## 四、技术栈与依赖版本

### 4.1 核心技术栈

| 类别 | 选型 | 版本 |
|------|------|------|
| 语言 | C++ | 17 |
| UI 框架 | Qt | 6.9.0 |
| 构建工具 | CMake | ≥ 3.20 |
| 编译器 | MSVC | 2019/2022 |
| 视频播放 | SmartPlayer SDK | 当前项目内 |
| 数据库 | SQLite (Qt Sql) | Qt 自带 |
| 向量索引 | FAISS | 1.7.4 |
| HTTP/SSE | QNetworkAccessManager | Qt 自带 |
| Markdown | QTextDocument | Qt 自带（够用）|
| JSON | QJsonDocument | Qt 自带 |
| 推理框架 | ONNX Runtime | 1.16+ |
| 日志 | spdlog | 1.12+ |

### 4.2 Qt 模块清单

```cmake
find_package(Qt6 6.9 REQUIRED COMPONENTS
    Core
    Gui
    Widgets
    Network
    Sql
    Concurrent      # 异步任务
    Svg             # 图标
)
```

### 4.3 AI 模型与服务

#### 大模型 API（核心推理）

支持任意 OpenAI Compatible 格式：
- OpenAI GPT-4o（首选）
- 通义千问 Qwen-VL-Max / Qwen2.5-VL
- DeepSeek Vision
- 智谱 GLM-4V
- 本地 Ollama + 视觉模型

#### 本地模型（M4 阶段需要）

| 用途 | 模型 | 大小 | 部署方式 |
|------|------|------|---------|
| 视觉 embedding | CLIP-ViT-B/32 | ~150MB | ONNX Runtime |
| 文本 embedding | BGE-small-zh-v1.5 | ~95MB | ONNX Runtime |
| 语音转文字 | Whisper-base | ~140MB | whisper.cpp |
| 场景分割 | 直方图差异 (M3 简单方案) | - | 自实现 |
| 场景分割 (M4 优化) | TransNetV2 | ~10MB | ONNX Runtime |

### 4.4 依赖获取方式

```
项目根目录/
├── third_party/
│   ├── smartplayer_sdk/    # 当前项目编译产物
│   ├── faiss/              # 编译好的 FAISS lib
│   ├── onnxruntime/        # 官方 release 解压
│   └── spdlog/             # header-only
└── models/                  # AI 模型文件（gitignore）
    ├── clip-vit-b32.onnx
    ├── bge-small-zh.onnx
    └── whisper-base.bin
```

依赖管理：
- 简单依赖（spdlog）：直接拷贝头文件到 `third_party`
- 复杂依赖（FAISS / ONNX）：手动下载 release，不用 vcpkg/conan（个人开发，简单优先）

---

## 五、目录结构

```
FrameMind/                       # 客户端项目（当前仓库根目录 Frame_Mind/）
├── CMakeLists.txt
├── README.md
├── docs/                        # 项目文档
│   ├── architecture-design.md
│   ├── agent-core-design.md
│   ├── development-plan.md     # 本文档
│   └── api-protocol.md
│
├── resources/
│   ├── icons/                   # SVG 图标
│   ├── styles/
│   │   ├── dark.qss
│   │   └── light.qss
│   └── resources.qrc
│
├── src/
│   ├── main.cpp
│   ├── app/                     # 应用初始化 + DI
│   ├── view/                    # View 层
│   ├── viewmodel/               # ViewModel 层
│   ├── model/                   # Domain Models
│   ├── service/                 # 业务服务
│   ├── infrastructure/          # 基础设施
│   ├── rag/                     # RAG 系统（M4 加入）
│   └── utils/
│
├── third_party/
│   ├── smartplayer_sdk/
│   ├── faiss/
│   ├── onnxruntime/
│   └── spdlog/
│
├── models/                      # AI 模型（gitignore）
└── tests/                       # 简单的手动测试用例
```

---

## 六、版本控制策略

个人开发，简化分支模型：

```
main         ← 始终保持可运行的状态
└── dev/*    ← 每个里程碑一个分支，完成后合回 main 并打 tag

Tag 规范:
v0.1.0-M1   骨架完成
v0.2.0-M2   单帧问答完成
v0.3.0-M3   视频整体理解完成
v0.4.0-M4   RAG + Tool Calling 完成
v1.0.0      MVP 发布
```

每个里程碑结束后强制要求：
1. 能在干净环境编译通过
2. 核心功能跑一遍无崩溃
3. 写一份简短的 CHANGELOG 备忘

---

## 七、关键技术风险与应对

| 风险 | 影响 | 应对 |
|------|------|------|
| Qt + SmartPlayer SDK 跨线程渲染卡顿 | 高 | M1 优先验证，必要时做帧丢弃 |
| SSE 流式协议解析复杂 | 中 | M2 找成熟参考实现（curl-cpp、OpenAI SDK 思路） |
| ONNX Runtime 在 Windows 上的部署 | 中 | M4 前预研，准备好 fallback（用 API） |
| FAISS 编译复杂 | 中 | 用预编译 release，不自己编译 |
| Whisper 转写耗时长 | 中 | 异步执行 + 进度上报，不阻塞主流程 |
| LLM API 成本失控 | 中 | 加 token 统计 + 单次问答上限 + QA 缓存 |
| Markdown 渲染特性不全 | 低 | QTextDocument 兜底，后续可换 cmark-gfm |
| 视频文件格式兼容性 | 低 | SmartPlayer 已基于 FFmpeg，问题不大 |

---

## 八、开发节奏建议（个人开发版）

1. **每天至少推进 1 个小任务**，哪怕只是修个 bug 或调通一个接口
2. **每个里程碑结束打 tag**，强制有一个稳定版本
3. **遇到选型纠结，先选最简单的**，能跑了再优化
4. **每周回顾一次**：上周完成了什么？这周计划什么？哪里卡住了？
5. **不要追求完美**：先 80 分能用，再迭代到 95 分
6. **API Mock 优先**：开发阶段用便宜的小模型甚至 mock 数据测试链路
7. **遇到长链路集成卡住时，先写一个最小可复现 demo**

---

## 九、立即可以开始的第一个任务

```
任务: 新建 Qt 6.9 项目骨架
预计耗时: 半天

步骤:
1. 在当前 Frame_Mind 仓库下初始化 FrameMind 工程目录结构（src/ resources/ third_party/ 等）
2. 写最简单的 CMakeLists.txt，能编出空 MainWindow
3. 把 SmartPlayer SDK 拷过来，CMake 链接通过
4. MainWindow 里加一个按钮，点击调用 SmartPlayer 打开测试视频
5. 用一个 QLabel 作为临时渲染目标，把帧数据显示出来

验收: 能播放一个 mp4 文件，画面正常显示在 QLabel 上

完成后: 提交 v0.0.1-skeleton tag
```
