# FrameMind

基于 **Qt 6.9 + SmartPlayer SDK** 的视频分析 AI Agent 客户端。

> 设计文档见 [`docs/`](./docs)；开发任务卡见 [`docs/dev-tasks.md`](./docs/dev-tasks.md)。

## 当前进度

- **M1 — 骨架**：能打开本地 mp4，流畅播放、可拖动 seek、可调音量/倍速；三栏布局 + 空对话占位。
- **M2 — 单帧 AI 问答**：SSE 流式对话、Markdown 渲染、对话历史持久化（SQLite）、
  「📷 当前帧」截帧提问、多会话切换。首次使用需在「文件 → AI 设置」中填写
  Endpoint / 模型 / API Key（Key 经 Windows DPAPI 加密存于系统密钥库，**不入数据库/日志**）。
- **M3 — 视频整体理解**（规划中）：场景分割、全视频摘要、时间戳点击跳转。
- **M4 — RAG + Tool Calling**（规划中）：CLIP / Whisper / FAISS 多路检索 + Agent 多步推理。

## Video RAG 框架方案

项目核心目标是构建**视频理解 RAG 系统**，支持本地视频文件与拉流（RTSP/RTMP/HLS）两种来源，
采用**大小模型协作**架构：

- **小模型（本地 ONNX Runtime）**：负责感知与编码 —— CLIP 视觉 Embedding、Whisper 语音转写、
  BGE-small 文本 Embedding、直方图差异场景分割。零网络延迟，离线可用。
- **大模型（云端 VLM/LLM）**：负责理解与推理 —— 场景描述生成、多帧联合推理、对话回答、
  Tool Calling 决策。能力上限高，通过已有的 `LLMProviderService` 多提供商切换。

### 核心设计

| 能力 | 方案 |
|------|------|
| 视频源抽象 | `IVideoSource` 统一接口，`LocalVideoSource` / `StreamVideoSource` 分别适配本地与拉流 |
| 渐进式索引 | Level 0 元信息+场景分割（秒级）→ Level 1 关键帧 Embedding+转写（后台异步）→ Level 2 按需 VLM 场景描述 |
| 多路检索融合 | 视觉（CLIP text→image）+ 文本（BGE 语义）+ 时间（规则），RRF 融合排序 |
| 拉流增量索引 | 滑动窗口 + 环形缓冲，保留最近 N 分钟可检索数据，FIFO 淘汰 |
| Agent 决策循环 | PERCEIVE → REPRESENT → REASON → ACT → REFLECT，最多 5 轮 Tool Calling |
| 上下文管理 | 分层组装 + U 形注意力布局 + 对话历史压缩，控制在 token 预算内 |

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
| ONNX Runtime | ≥ 1.16（M4 起，CLIP / Whisper / BGE 本地推理）|
| FAISS | 1.7.4（M4 起，本地向量检索，CPU 版）|
| whisper.cpp | 预编译库（M4 起，语音转写）|

> M1 ~ M3 阶段仅需 Qt + SmartPlayer SDK；ONNX Runtime / FAISS / whisper.cpp 在 M4 阶段引入。

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
├── main.cpp              # 入口
├── app/                  # Application + DIContainer（依赖注入）
├── view/                 # View 层（MainWindow / Sidebar / Player / Chat ...）
├── viewmodel/            # ViewModel 层
├── model/                # Domain Models（ChatMessage / VideoContext ...）
├── service/              # Service 层
│   ├── playerservice.*        # SmartPlayer 封装（已有）
│   ├── agentservice.*         # AI 对话 + SSE 流式（已有）
│   ├── llmproviderservice.*   # 多 LLM 提供商管理（已有）
│   ├── video_analysis_service.*  # 视频分析主服务（M3/M4）
│   ├── scene_detector.*       # 场景分割（M3）
│   ├── clip_service.*         # CLIP 视觉 Embedding（M4）
│   ├── whisper_service.*      # 语音转写（M4）
│   ├── embedding_service.*    # 文本 Embedding BGE（M4）
│   ├── video_rag_retriever.*  # 多路检索 + RRF 融合（M4）
│   ├── stream_indexer.*       # 拉流增量索引（M4）
│   └── tool_orchestrator.*    # Agent Tool Calling 编排（M4）
├── infrastructure/       # 基础设施（ImageProcessor / EventBus / NetworkClient ...）
└── utils/
```
