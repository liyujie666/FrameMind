# FrameMind

基于 **Qt 6.9 + SmartPlayer SDK** 的视频分析 AI Agent 客户端。

> 设计文档见 [`docs/`](./docs)；开发任务卡见 [`docs/dev-tasks.md`](./docs/dev-tasks.md)。

## 更新记录

| 日期 | 描述 |
|------|------|
| 2026-08-01 | Agent 核心优化：索引流程重构、知识库 UI 完善、播放器与分析联动增强 |
| 2026-07-30 | 优化 RAG 场景描述质量：全量场景描述、多帧采样、上下文连贯性增强 |
| 2026-07-29 | 实现视频 RAG pipeline、知识库 UI 与播放器功能修复 |
| 2026-07-28 | 搭建视频分析 Agent 骨架：五阶段决策循环 + RAG + 6 Tool 编排 |
| 2026-07-27 | 添加 Video RAG 小模型服务框架（CLIP/BGE/Whisper/场景检测）|
| 2026-07-26 | 解除帧率 30fps 限制 |
| 2026-07-25 | UI 优化与大模型配置功能 |
| 2026-07-24 | 更新 README 加入 Video RAG 方案说明，完善 UI 与播放器交互 |

## 当前进度

- **M1 — 骨架**：能打开本地 mp4，流畅播放、可拖动 seek、可调音量/倍速；三栏布局 + 空对话占位。
- **M2 — 单帧 AI 问答**：SSE 流式对话、Markdown 渲染、对话历史持久化（SQLite）、
  「📷 当前帧」截帧提问、多会话切换。首次使用需在「文件 → AI 设置」中填写
  Endpoint / 模型 / API Key（Key 经 Windows DPAPI 加密存于系统密钥库，**不入数据库/日志**）。
- **M3 / M4 — Video Agent 骨架代码（本次提交）**：按照 `agent-core-design.md` 的
  **五阶段决策循环**（PERCEIVE → REPRESENT → REASON → ACT → REFLECT）搭建了完整的
  Agent 框架代码，含 RAG 存储 / 多路检索 / 6 个 Tool / Tool 编排 / 反思引擎 / 顶层协调器。
  当前仅完成骨架与 DI 装配，尚未与 ChatViewModel 联调。

## Video Analysis Agent 框架

### 分层架构

```
┌───────────────────── ChatViewModel / UI ────────────────────┐
                              │
                    ┌─────────▼─────────┐
                    │    VideoAgent     │  ← 顶层协调器（五阶段主循环）
                    └───┬────────────┬──┘
                        │            │
              PERCEIVE  │            │  REFLECT
      ┌─────────────────▼──┐    ┌────▼─────────────────┐
      │ PerceptionStrategy │    │  ReflectionEngine    │
      │ 问题分类/采样计划   │    │  一致性/证据/时间/幻觉 │
      └─────────────────┬──┘    └──────────────────────┘
                        │
                REASON  │  ACT
              ┌─────────▼──────────┐
              │  ToolOrchestrator  │  ← 多轮 Tool Calling (≤5 轮)
              └─────────┬──────────┘
                        │
      ┌─────────────────┼─────────────────┐
      │                 │                 │
┌─────▼──────┐  ┌───────▼────────┐  ┌────▼──────────┐
│ AgentSvc   │  │  ToolRegistry  │  │ 6 Tools       │
│ +Tools API │  │                │  │ seek/analyze… │
└─────┬──────┘  └────────────────┘  └───────┬───────┘
      │                                     │
      │       REPRESENT                     │
      │       ┌─────────────────────────────▼──┐
      │       │  VideoAnalysisService (Level2) │
      │       │  ┌─── VideoIndexer (L0/L1) ──┐ │
      │       │  │  scene / CLIP / whisper   │ │
      │       │  └───────────────────────────┘ │
      │       └────────────┬───────────────────┘
      │                    │
      ▼                    ▼
┌─────────────────────────────────────────────────────┐
│              VideoRAGStore  (SQLite)                │
│   visual_frames │ text_segments │ entities │ QA     │
└─────────────────────────────────────────────────────┘
         ▲                        ▲
    QACacheManager        VideoRAGRetriever（多路+RRF）
```

### 核心组件

| 阶段 / 层 | 组件 | 职责 |
|-----------|------|------|
| PERCEIVE | `PerceptionStrategy` | 问题分类（13 种 `QuestionType`）+ 采样计划（时间区间/密度/预算）|
| REPRESENT | `VideoIndexer` | 渐进式索引 L0/L1（元信息 + 场景分割 + 关键帧 CLIP + Whisper 转写）|
| REPRESENT | `VideoAnalysisService` | Level 2 VLM 场景描述 + 全视频摘要；单帧/区间深度分析 |
| REPRESENT | `EntityTracker` | 实体注册、共指消解（"那个人"→ 具体实体档案）|
| REASON | `AgentService::sendMessageWithTools` | 携带 `tools` 字段发起 SSE，解析 `delta.tool_calls` 增量 |
| ACT | `ToolOrchestrator` | 多轮 Tool 循环（≤5 轮，≤3 次工具调用/答）；回填 `role=tool` 消息 |
| ACT | 6 个 Tool | `seek_and_analyze` / `analyze_time_range` / `search_video_content` / `get_transcript` / `get_scene_info` / `control_player` |
| REFLECT | `ReflectionEngine` | 四项校验：事实一致性 / 证据支撑 / 时间合理性 / 幻觉检测 |
| RAG 存储 | `VideoRAGStore` | 4 集合统一存储（visual_frames / text_segments / entity_profiles / qa_cache）+ SQLite 持久化 |
| RAG 检索 | `VideoRAGRetriever` | 多路召回（text/visual/entity）+ RRF 融合排序 |
| 记忆 | `QACacheManager` | QA 缓存复用（阈值 0.88），命中直接返回历史结论 |
| 顶层 | `VideoAgent` | 五阶段编排：`ask()` 一站式入口 |

### 领域模型

| 模型 | 说明 |
|------|------|
| `VideoChunk` / `RetrievalResult` | RAG 检索单元（双 embedding，携带时间定位与元数据）|
| `VideoRepresentation` | 视频三层表示（感知/结构/语义）+ 索引级别 L0~L2 |
| `EntityProfile` | 实体档案（类型/别名/出现记录/描述向量）|
| `SamplingPlan` / `SufficiencyCheck` | 感知采样计划与信息充分性判定 |
| `ReasoningResult` / `ReflectionResult` | 推理结果与反思校验产物 |
| `AgentAnswer` | 单轮 Agent 最终回答（含置信度、证据、工具轨迹）|
| `ToolCall` / `ToolResult` | Tool Calling 结构 |

## 下一步计划

- [ ] **接线到 ChatViewModel**：把 `AgentService::sendMessage` 调用替换为
      `DIContainer::videoAgent()->ask()`，UI 层即可用完整 Agent 能力。
- [ ] **视频打开钩子**：`PlayerViewModel::openFile` 完成后调用
      `VideoAgent::setActiveVideo(path, VideoIndexer::computeVideoId(path))`
      并触发 `VideoAnalysisService::onVideoOpened(path)` 启动渐进式索引。
- [ ] **索引进度 UI**：`ChatView` 顶部加状态条，绑定 `VideoIndexer::progress` 信号。
- [ ] **接入真实小模型**：编译带 `-DFRAMEMIND_ENABLE_ONNX=ON` / `-DFRAMEMIND_ENABLE_WHISPER=ON`，
      在 `AppData/models/` 放置 `clip_visual.onnx` / `clip_text.onnx` / `bge-small-zh.onnx` / `ggml-small.bin`。
- [ ] **VLM oneShot 独立通道**：`VideoAnalysisService::oneShotVLM` 目前复用
      `AgentService` 与用户对话共用状态，需迁移到独立 `AgentService` 实例或
      直连 `NetworkClient`，避免与用户流式响应互相干扰。
- [ ] **Whisper 音频抽取**：`VideoIndexer::buildLevel1` 里 Whisper 转写目前占位，
      待接入 FFmpeg 提取 16kHz mono PCM 后打通。
- [ ] **场景关键帧持久化**：`Scene::keyframePath` 落盘到 `AppData/keyframes/<videoId>/`，
      释放内存占用；索引缓存命中时秒级复用。
- [ ] **单元测试起步**：`VideoRAGStore::cosineSimilarity`、
      `PerceptionStrategy::classifyQuestion`、`ReflectionEngine::extractTimestamps`
      等纯函数最适合起步。

## Video RAG 框架方案

项目核心目标是构建**视频理解 RAG 系统**，采用**大小模型协作**架构：

- **小模型（本地 ONNX Runtime）**：负责感知与编码 —— CLIP 视觉 Embedding、Whisper 语音转写、
  BGE-small 文本 Embedding、直方图差异场景分割。零网络延迟，离线可用。
- **大模型（云端 VLM/LLM）**：负责理解与推理 —— 场景描述生成、多帧联合推理、对话回答、
  Tool Calling 决策。通过已有的 `LLMProviderService` 多提供商切换。

### 关键设计

| 能力 | 方案 |
|------|------|
| 渐进式索引 | Level 0 元信息+场景分割（秒级）→ Level 1 关键帧 Embedding+转写（后台异步）→ Level 2 按需 VLM 场景描述 |
| 多路检索融合 | 视觉（CLIP text→image）+ 文本（BGE 语义）+ 实体，RRF 融合排序（k=60）|
| Agent 决策循环 | PERCEIVE → REPRESENT → REASON → ACT → REFLECT，最多 5 轮 Tool Calling |
| 上下文管理 | 分层组装 + U 形注意力布局 + 对话历史压缩 |
| 拉流增量索引 | 滑动窗口 + 环形缓冲，保留最近 N 分钟（规划中）|

### 三层视频表示金字塔

```
语义层  → VideoSummary / SceneDescriptions / EventChain / EntityProfiles
结构层  → SceneGraph / EntityRegistry / SpeechSegments / TemporalIndex
感知层  → 原始帧 / CLIP Embedding / 音频 / 光流
```

> 详细方案见 [`docs/video-rag-plan.md`](./docs/video-rag-plan.md)；Agent 决策与 Tool 语义见
> [`docs/agent-core-design.md`](./docs/agent-core-design.md)。

## 环境要求

| 依赖 | 版本 |
|------|------|
| Qt | 6.9（Core / Gui / Widgets / Network / Sql / Concurrent / Svg）|
| CMake | ≥ 3.20 |
| 编译器 | MSVC 2019 / 2022 (x64) |
| SmartPlayer SDK | 预编译产物，置于 `third_party/smartplayer_sdk/` |
| ONNX Runtime | ≥ 1.16（M4 起，CLIP / BGE 本地推理，可选）|
| FAISS | 1.7.4（规划中，本地向量检索，CPU 版）|
| whisper.cpp | 预编译库（M4 起，语音转写，可选）|

> M1 ~ M3 阶段仅需 Qt + SmartPlayer SDK；ONNX Runtime / whisper.cpp 在 M4 阶段引入。
> 当前 Agent 骨架允许未启用小模型时以 nullptr 兜底运行（仅缺失 embedding 检索能力）。

`third_party/smartplayer_sdk/` 结构：

```
third_party/smartplayer_sdk/
├── include/   # smartplayer.h / smartplayercallback.h / smartplayerdefs.h
├── lib/       # SmartPlayerSDK.lib
└── bin/       # SmartPlayerSDK.dll + FFmpeg/SDL2 运行时 dll
```

## 编译

```powershell
# 在仓库根目录
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_PREFIX_PATH="C:/Qt/6.9.0/msvc2019_64"

cmake --build build --config Debug
```

启用小模型（需先在 `third_party/onnxruntime/` 放置 ONNX Runtime）：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_PREFIX_PATH="C:/Qt/6.9.0/msvc2019_64" ^
      -DFRAMEMIND_ENABLE_ONNX=ON ^
      -DFRAMEMIND_ENABLE_WHISPER=ON
```

> 把 `CMAKE_PREFIX_PATH` 改成你本机的 Qt 安装路径。

构建完成后，可执行文件位于 `build/Debug/FrameMind.exe`，运行所需的 SDK / FFmpeg / SDL2 dll 会在编译后自动拷贝到同目录。

## 运行

```powershell
./build/Debug/FrameMind.exe
```

菜单「文件 → 打开视频...」选择本地 mp4 即可播放。

## 目录结构

```
src/
├── main.cpp                  # 入口
├── app/                      # Application + DIContainer（依赖注入）
├── view/                     # View 层（MainWindow / Sidebar / Player / Chat ...）
├── viewmodel/                # ViewModel 层
├── model/                    # Domain Models
│   ├── chatmessage.h / conversation.h / videoinfo.h / videocontext.h
│   ├── scene.h / speech_segment.h
│   ├── retrieval_result.h        # RAG 检索单元（新）
│   ├── entity_profile.h          # 实体档案（新）
│   ├── video_representation.h    # 视频三层表示（新）
│   ├── agent_types.h             # Agent 决策/反思类型（新）
│   └── tool_types.h              # Tool Calling 类型（新）
├── service/                  # Service 层
│   ├── playerservice.*            # SmartPlayer 封装
│   ├── agentservice.*             # AI 对话 + SSE 流式 + Tool Calling
│   ├── llmproviderservice.*       # 多 LLM 提供商管理
│   ├── conversationservice.*      # 对话持久化
│   ├── filemanagerservice.*       # 文件管理
│   ├── settingsservice.* / themeservice.*
│   ├── scene_detector.*           # 场景分割
│   ├── clip_service.*             # CLIP 视觉+文本 Embedding（ONNX）
│   ├── embedding_service.*        # BGE-small 文本 Embedding（ONNX）
│   ├── whisper_service.*          # 语音转写（whisper.cpp）
│   ├── rag/                       # ── RAG 存储与检索（新）
│   │   ├── video_rag_store.*         #    4 集合统一存储
│   │   ├── qa_cache_manager.*        #    QA 缓存（阈值 0.88）
│   │   ├── video_rag_retriever.*     #    多路 + RRF 融合
│   │   └── entity_tracker.*          #    实体追踪与共指消解
│   └── agent/                     # ── Agent 核心（新）
│       ├── video_indexer.*           #    渐进式索引 L0/L1
│       ├── video_analysis_service.*  #    VLM 场景描述 + 摘要 (L2)
│       ├── perception_strategy.*     #    PERCEIVE：问题分类 + 采样规划
│       ├── reflection_engine.*       #    REFLECT：4 项校验
│       ├── tool_base.h               #    ITool 接口
│       ├── tool_registry.*           #    Tool 注册表
│       ├── tool_orchestrator.*       #    多轮 Tool 循环（≤5 轮）
│       ├── video_agent.*             #    顶层协调器（五阶段主循环）
│       └── tools/                    #    6 个 Tool 实现
│           ├── seek_and_analyze_tool.*
│           ├── analyze_time_range_tool.*
│           ├── search_video_content_tool.*
│           ├── get_transcript_tool.*
│           ├── get_scene_info_tool.*
│           └── control_player_tool.*
├── infrastructure/           # 基础设施
│   ├── networkclient.*            # HTTP + SSE（含 streamPostRaw 支持 tool_calls）
│   ├── databasemanager.*
│   ├── eventbus.* / imageprocessor.*
│   └── onnx_runtime_engine.*      # ONNX Runtime 统一封装
└── utils/
```
