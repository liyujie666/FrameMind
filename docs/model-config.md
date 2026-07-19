# FrameMind 视频 RAG — 模型配置方案

> 本文是 FrameMind 视频 RAG 系统的**模型配置单一事实来源（SSoT）**。
> 与现有代码契约冲突时，以代码契约（`clip_service.h` / `embedding_service.h` /
> `whisper_service.h` / `scene_detector.h` / `dicontainer.cpp` / `CMakeLists.txt`）为准。
>
> 配套文档：
> - 范围/里程碑 → [`development-plan.md`](./development-plan.md)
> - RAG 整体方案 → [`video-rag-plan.md`](./video-rag-plan.md)
> - Agent 决策 → [`agent-core-design.md`](./agent-core-design.md)

---

## 一、配置总览

FrameMind 采用 **大小模型协作** 架构。本地小模型负责感知与编码（零网络延迟、离线可用），
云端大模型负责理解与推理（能力上限高）。装配层（`DIContainer`）负责把两者注入到
`VideoAgent` 的五阶段决策循环。

| 层 | 模型 / 组件 | 部署位置 | 维度 / 规格 | 文件大小 | 加载入口 |
|----|------------|---------|------------|---------|---------|
| 感知 | CLIP ViT-B/32（视觉） | 本地 ONNX | 512d · 224×224 | ~350MB | `ClipService::initialize` |
| 感知 | CLIP ViT-B/32（文本） | 本地 ONNX | 512d · 77 tok | ~250MB | `ClipService::initialize` |
| 感知 | BGE-small-zh-v1.5 | 本地 ONNX | 512d · 512 tok | ~100MB | `EmbeddingService::initialize` |
| 感知 | Whisper small | 本地 ggml | 16kHz mono f32 | ~466MB | `WhisperService::initialize` |
| 感知 | TransNetV2（可选） | 本地 ONNX | — | ~30MB | `SceneDetector::loadTransNetV2` |
| 推理 | VLM 多模态 | 云端 API | 厂商相关 | — | `LLMProviderService` |
| 推理 | LLM 文本 | 云端 API | 厂商相关 | — | `LLMProviderService` |
| 装配 | ONNX Runtime ≥ 1.18 | `third_party/onnxruntime/` | — | ~50MB | CMake `FRAMEMIND_ENABLE_ONNX` |
| 装配 | whisper.cpp | `third_party/whisper.cpp/` | — | 源码子模块 | CMake `FRAMEMIND_ENABLE_WHISPER` |
| 存储 | FAISS（规划中） | `third_party/faiss/` | CPU 版 | — | CMake `FRAMEMIND_ENABLE_FAISS` |

> 当前 `third_party/` 仅有 `smartplayer_sdk/`。**ONNX Runtime / whisper.cpp 需先下载放置到位**，
> 详见 §三与 §四。

---

## 二、本地小模型配置

### 2.1 ONNX Runtime（推理框架）

**用途**：CLIP / BGE / TransNetV2 三个模型的统一推理后端，由 `OnnxRuntimeEngine` 封装。

**版本**：`onnxruntime-win-x64-1.18.1`（与 `onnx_runtime_engine.h` 注释一致）。

**目录结构**（解压后放置）：

```
third_party/onnxruntime/
├── include/                  # onnxruntime_cxx_api.h 等头文件
│   └── onnxruntime_cxx_api.h
├── lib/
│   └── onnxruntime.lib       # 链接库
└── bin/
    ├── onnxruntime.dll       # 主运行时（POST_BUILD 自动拷贝到 exe 目录）
    ├── onnxruntime_providers_cuda.dll   # 可选：CUDA EP
    └── onnxruntime_providers_shared.dll
```

**CMake 开关**：

```powershell
-DFRAMEMIND_ENABLE_ONNX=ON
```

启用后会触发 `target_compile_definitions(FrameMind PRIVATE FRAMEMIND_HAS_ONNXRUNTIME)`，
`dicontainer.cpp` 中 `#ifdef FRAMEMIND_HAS_ONNXRUNTIME` 块即生效。

**CPU / GPU 选择**：

`OnnxRuntimeEngine` 构造参数 `useGpu`：
- `false`（默认）：CPU EP。零依赖、跨机一致、推荐起步。
- `true`：CUDA EP。需要 `onnxruntime_providers_cuda.dll` + 本机 NVIDIA 驱动 + CUDA。
  不可用时**自动回退 CPU**（见 `onnx_runtime_engine.cpp` 兜底逻辑）。

> **建议**：开发期一律用 CPU。GPU 只在 CLIP 批量编码出现瓶颈（>2s/批）时再开。

---

### 2.2 CLIP ViT-B/32（视觉 + 文本 Embedding）

**用途**：
- `encodeImage()`：关键帧 → 512 维视觉向量，写入 `VideoRAGStore::VisualFrames`。
- `encodeText()`：用户查询 → 512 维文本向量，做图文检索（CLIP text→image）。

**契约常量**（来自 `clip_service.h`，**不可改**）：

| 常量 | 值 | 说明 |
|------|----|----|
| `EMBEDDING_DIM` | 512 | 输出向量维度 |
| `IMAGE_SIZE` | 224 | 输入图像分辨率 |
| `TEXT_MAX_LEN` | 77 | 文本最大 token 数 |

**模型文件**（放置到项目根 `models/`，详见 §4.1）：

| 文件名 | 大小 | 来源 |
|--------|------|------|
| `clip_visual.onnx` | ~350MB | HuggingFace `openai/clip-vit-base-patch32` 的社区 ONNX 导出 |
| `clip_text.onnx` | ~250MB | 同上 |

**下载命令**（任选一种）：

```powershell
# 方式 A：用 Python 导出（精度可控，推荐）
pip install transformers onnxruntime torch
python -c "from transformers import CLIPModel; m = CLIPModel.from_pretrained('openai/clip-vit-base-patch32')"

# 方式 B：直接下载社区已导出的 ONNX
# https://huggingface.co/openai/clip-vit-base-patch32  → onnx/ 子目录
```

**加载位置**：`dicontainer.cpp:97`

```cpp
m_clipService->initialize(
    modelsDir + "/clip_visual.onnx",
    modelsDir + "/clip_text.onnx");
```

**待办**（README §下一步计划）：`ClipService::tokenizeText` 目前是占位实现，
需接入完整的 CLIP BPE tokenizer（可移植 `clip/simple_tokenizer.py` 为 C++）。

---

### 2.3 BGE-small-zh-v1.5（中文文本 Embedding）

**用途**：
- `embed()` / `embedBatch()`：转写文本、场景描述、实体描述 → 512 维向量。
- 写入 `VideoRAGStore::TextSegments` 与 `EntityProfiles`。
- 检索阶段在 `VideoRAGRetriever::textPathSearch` 中编码用户查询。

**与 CLIP 文本编码器的分工**（来自 `embedding_service.h`）：
- CLIP text：偏视觉语义（"红衣服的人" → 匹配画面中的人）。
- BGE：偏自然语言语义（场景描述、转写文本的精确语义搜索）。

**契约常量**：

| 常量 | 值 |
|------|----|
| `EMBEDDING_DIM` | 512 |
| `MAX_SEQ_LEN` | 512 |

**模型文件**：

| 文件名 | 大小 | 来源 |
|--------|------|------|
| `bge-small-zh.onnx` | ~100MB | HuggingFace `BAAI/bge-small-zh-v1.5` |

**加载位置**：`dicontainer.cpp:102`

```cpp
m_embeddingService->initialize(modelsDir + "/bge-small-zh.onnx");
```

**待办**：`EmbeddingService::tokenize` 目前占位，需接入 BERT WordPiece tokenizer
（中文场景下还需要 BasicTokenizer 分词）。

---

### 2.4 Whisper.cpp（语音转写）

**用途**：把视频音轨（16kHz mono float32 PCM）转写为带时间戳的分段文本，
写入 `VideoRepresentation::speechSegments` 与 `VideoRAGStore::TextSegments`。

**契约**（来自 `whisper_service.h`）：
- 输入：16kHz、单声道、float32 PCM。
- 默认语言：`zh`（可 `setLanguage("auto")` 自动检测）。
- 默认采样：贪心（快），可切 beam search（准）。
- 默认线程数：4。

**模型文件**（放置到项目根 `models/`，详见 §4.1）：

| 文件名 | 大小 | 语言覆盖 | 推荐场景 |
|--------|------|---------|---------|
| `ggml-tiny.bin` | ~75MB | 多语种 | 极速验证 / 低配机 |
| `ggml-base.bin` | ~142MB | 多语种 | 开发期默认 |
| `ggml-small.bin` | ~466MB | 多语种 | **生产推荐**（中文识别质量佳） |
| `ggml-medium.bin` | ~1.5GB | 多语种 | 高精度需求 |
| `ggml-large-v3.bin` | ~3GB | 多语种 | 极致精度（不推荐本地） |

> `dicontainer.cpp:109` 当前硬编码 `ggml-small.bin`，按需替换。

**whisper.cpp 库放置**：

```powershell
git clone https://github.com/ggerganov/whisper.cpp third_party/whisper.cpp
# CMake 中 add_subdirectory(third_party/whisper.cpp) 会自动编译
```

**CMake 开关**：

```powershell
-DFRAMEMIND_ENABLE_WHISPER=ON
```

启用后 `target_compile_definitions(FrameMind PRIVATE FRAMEMIND_HAS_WHISPER)`，
并链接 `whisper` 目标。

**音频抽取待办**（README §下一步计划）：`VideoIndexer::buildLevel1` 里 Whisper 转写
目前占位，需接入 FFmpeg 把视频音轨抽成 16kHz mono PCM（`ffmpeg -i a.mp4 -ar 16000
-ac 1 -f f32le a.pcm`）。

---

### 2.5 TransNetV2（可选场景分割）

**用途**：当 `SceneDetector` 的直方图差异算法精度不足时，升级到深度学习场景检测。

**默认模式**：直方图差异（纯 Qt/OpenCV 算法，无模型，速度极快，阈值默认 0.3）。

**升级路径**：调用 `SceneDetector::loadTransNetV2(modelPath)`，加载后自动切换到
TransNetV2 模式（`m_useTransNet = true`）。

**模型文件**：

| 文件名 | 大小 | 来源 |
|--------|------|------|
| `transnetv2.onnx` | ~30MB | `ohenrik/transnetv2-onnx` 或社区导出 |

**何时启用**：直方图差异在以下场景误检率高时再升级——
- 快速运动 / 镜头晃动
- 渐变转场（淡入淡出）
- 同一场景内大幅光照变化

> M3 阶段先用直方图差异跑通流水线，**不要一上来就上 TransNetV2**。

---

## 三、云端大模型配置

### 3.1 LLMProvider 抽象

所有云端模型走 `LLMProviderService`，配置项由 `LLMProvider` 结构体承载
（`model/llmprovider.h`）。每个 Provider 字段：

| 字段 | 含义 | 示例 |
|------|------|------|
| `id` | 唯一标识符 | `qwen-vl-max` |
| `name` | 显示名称 | `通义千问 VL Max` |
| `type` | 提供商类型枚举 | `Qianfan` |
| `endpoint` | API 端点 | `https://dashscope.aliyuncs.com/compatible-mode/v1` |
| `defaultModel` | 默认模型 | `qwen-vl-max` |
| `apiKeyName` | 密钥存储名（DPAPI） | `qwen_vl_apikey` |
| `supportsVision` | 是否支持多模态 | `true` |
| `requiresOrgId` | 是否需要组织 ID | `false` |
| `models` | 可切换模型列表 | `["qwen-vl-max", "qwen-vl-plus"]` |

**预设 Provider**（`LLMProviderPresets::allPresets()`）：
OpenAI / DeepSeek / Qianfan（阿里云百炼）/ Zhipu / Ollama / Custom

**密钥安全**：所有 API Key 经 `SettingsService::secretSet` 写入 Windows DPAPI
（密文落 `<AppData>/secrets/<name>.bin`），**不入 SQLite / 配置 / 日志**。

---

### 3.2 VLM 多模态推荐（用于场景描述 / 多帧推理）

VLM 用于 `VideoAnalysisService::oneShotVLM`（Level 2 场景描述）与
`seek_and_analyze` / `analyze_time_range` Tool（多帧联合推理）。

| Provider | 模型 | 端点 | 是否支持视觉 | 推荐场景 |
|----------|------|------|------------|---------|
| OpenAI | `gpt-4o` / `gpt-4o-mini` | `https://api.openai.com/v1` | ✅ | 综合最佳，价格较高 |
| Qianfan | `qwen-vl-max` | `https://dashscope.aliyuncs.com/compatible-mode/v1` | ✅ | 中文视频首选 |
| Zhipu | `glm-4v` | `https://open.bigmodel.cn/api/paas/v4` | ✅ | 国内备选 |
| DeepSeek | `deepseek-vl2` | `https://api.deepseek.com/v1` | ✅ | 性价比高 |
| Ollama | `llava` / `qwen2-vl` | `http://localhost:11434/v1` | ✅ | 本地离线 |

> **生产首选**：`qwen-vl-max`（中文视频场景描述质量最佳）。
> **预算敏感**：`gpt-4o-mini`（多帧理解够用，成本低一个数量级）。
> **离线/隐私**：本地 Ollama + `qwen2-vl:7b`。

---

### 3.3 LLM 文本推荐（用于对话回答 / Tool Calling）

LLM 用于 `AgentService::sendMessageWithTools`（SSE 流式 + Tool Calling），
不直接看图，负责问题分类、上下文组装、最终回答生成。

| Provider | 模型 | 推荐场景 |
|----------|------|---------|
| OpenAI | `gpt-4o-mini` / `gpt-4o` | Tool Calling 最稳，生态成熟 |
| DeepSeek | `deepseek-chat`（V3） | 国内首选，Tool Calling 支持完整 |
| Qianfan | `qwen-plus` / `qwen-max` | 中文场景，长上下文 |
| Zhipu | `glm-4-plus` / `glm-4-flash` | 国产备选 |
| Ollama | `qwen2.5:7b` / `llama3.1:8b` | 本地离线 |

> **生产首选**：`deepseek-chat`（Tool Calling 稳定，价格极低）。
> **离线**：Ollama + `qwen2.5:7b-instruct`。

---

### 3.4 推荐双 Provider 组合

FrameMind 的 Agent 决策循环同时需要 VLM 和 LLM。推荐三种组合方案：

| 方案 | VLM | LLM | 月成本估算 | 适用 |
|------|-----|-----|----------|------|
| **A · 全云端** | `gpt-4o` | `gpt-4o-mini` | 中-高 | 综合质量最佳 |
| **B · 国内云** | `qwen-vl-max` | `deepseek-chat` | 低 | 中文视频、国内网络 |
| **C · 混合离线** | Ollama `qwen2-vl:7b` | Ollama `qwen2.5:7b` | 0（电费） | 隐私 / 离线 |

> **当前实现状态**：`LLMProviderService` 只维护一个 `activeProviderId`，
> **尚未做 VLM/LLM 双通道分离**。README §下一步计划已标记
> "VLM oneShot 独立通道" 待办：需为 `VideoAnalysisService::oneShotVLM` 单独指定 VLM Provider，
> 与用户对话用的 LLM Provider 解耦。建议在 `SettingsService` 新增
> `activeVlmProviderId` 与 `activeLlmProviderId` 两个 key。

---

## 四、模型文件目录结构

### 4.1 模型权重目录（项目自包含）

**模型权重放在项目根的 `models/` 目录**（不入 git，由 `.gitignore` 忽略），
便于项目自包含、便于团队协作、便于迁移。`dicontainer.cpp` 中的
`resolveModelsDir()` 按以下优先级查找：

| 优先级 | 路径 | 适用场景 |
|--------|------|---------|
| 1 | 环境变量 `FRAMEMIND_MODELS_DIR` | 测试 / CI 覆盖 |
| 2 | 可执行文件同级 `./models/` | 发布版部署（exe + models/ 一起分发） |
| 3 | **项目根 `./models/`** | **开发期**（exe 在 `build/Debug/`，向上两级找 `<项目根>/models/`） |
| 4 | `<AppData>/FrameMind/FrameMind/models/` | 兜底（保持原行为） |

> 命中即用，不存在的级跳过。全部不存在时创建兜底目录。
> 启动时 `qDebug() << "modelsDir =" << ...` 会打印实际选中的路径，便于排查。

**推荐做法（开发期）**：把所有模型权重放到项目根 `models/`：

```
D:\Qt\ffmpegProjects\FrameMind\
├── models/                     ← 模型权重（项目自包含，不入 git）
│   ├── clip_visual.onnx        # CLIP 视觉编码器（必需，开 ONNX）    ~336MB
│   ├── clip_text.onnx          # CLIP 文本编码器（必需，开 ONNX）      ~243MB
│   ├── clip_vocab.json         # CLIP BPE 词表                          ~843KB
│   ├── clip_merges.txt         # CLIP BPE merge 规则                    ~513KB
│   ├── bge-small-zh.onnx       # BGE 文本编码器（必需，开 ONNX）        ~100MB
│   ├── bge_tokenizer.json      # BGE WordPiece tokenizer                  ~XXX
│   ├── ggml-small.bin          # Whisper 模型（必需，开 Whisper）        ~466MB
│   ├── ggml-base.bin           # 可选：低配机用 base 替代 small          ~142MB
│   └── transnetv2.onnx         # 可选：场景分割升级                      ~30MB
├── third_party/                ← 编译时依赖（CMake 硬编码）
│   ├── onnxruntime/            # ONNX Runtime 库
│   ├── whisper.cpp/            # whisper.cpp 源码
│   └── smartplayer_sdk/        # SmartPlayer SDK
├── scripts/
│   └── setup_models.py         # 一键配置模型权重
├── .venv/                      # 临时 Python 环境（BGE 导出用，不入 git）
├── src/
├── docs/
└── CMakeLists.txt
```

**用户数据仍走 `<AppData>`**（与模型权重分开）：

```
<AppData>/FrameMind/FrameMind/
├── agent.db                    # SQLite 主库（settings / conversations / rag_chunks / rag_entities）
├── secrets/                    # DPAPI 加密后的 API Key
│   ├── openai_apikey.bin
│   └── ...
├── keyframes/                  # 场景关键帧缓存（按 videoId 分目录）
└── thumbnails/                 # 视频列表缩略图缓存
```

> 这种分离的好处：模型权重是项目资源（团队共享、可重建），用户数据是
> 私有数据（每个人的对话历史/密钥不同），两者独立管理。

**模型文件清单（最小可运行集）**：

| 开关组合 | 必需文件 | 总大小 |
|---------|---------|--------|
| `FRAMEMIND_ENABLE_ONNX=ON` | clip_visual.onnx + clip_text.onnx + bge-small-zh.onnx | ~700MB |
| `FRAMEMIND_ENABLE_WHISPER=ON` | ggml-small.bin | ~466MB |
| 两个都开 | 以上全部 | ~1.2GB |
| 都关（兜底模式） | 无 | 0（仅失去 embedding 检索能力） |

---

## 五、CMake 构建开关

完整构建命令（启用全部小模型）：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 ^
      -DCMAKE_PREFIX_PATH="D:/Qt/6.9.1/msvc2022_64" ^
      -DFRAMEMIND_ENABLE_ONNX=ON ^
      -DFRAMEMIND_ENABLE_WHISPER=ON
```

**开关矩阵**：

| 开关 | 默认 | 启用后效果 | 前置条件 |
|------|------|----------|---------|
| `FRAMEMIND_ENABLE_ONNX` | `OFF` | 编译 `onnx_runtime_engine.cpp` / `clip_service.cpp` / `embedding_service.cpp`；定义 `FRAMEMIND_HAS_ONNXRUNTIME`；链接 `onnxruntime` | `third_party/onnxruntime/` 已放置 |
| `FRAMEMIND_ENABLE_WHISPER` | `OFF` | 编译 `whisper_service.cpp`；定义 `FRAMEMIND_HAS_WHISPER`；`add_subdirectory(third_party/whisper.cpp)` | `third_party/whisper.cpp/` 已 clone |
| `FRAMEMIND_ENABLE_FAISS` | （规划中） | 替换 `VideoRAGStore` 内存向量为 FAISS `IndexFlatIP` | `third_party/faiss/` 已放置 |

**关闭时的兜底**（README §环境要求）：
> 当前 Agent 骨架允许未启用小模型时以 nullptr 兜底运行（仅缺失 embedding 检索能力）。
> 即 `ClipService*` / `EmbeddingService*` / `WhisperService*` 在 DIContainer 中为 `nullptr`，
> `VideoIndexer::setClipService(nullptr)` 等会跳过对应 stage。

---

## 六、运行时加载流程

```
main.cpp
   │
   ▼
Application::Application
   │
   ▼
DIContainer::initialize()                       ← dicontainer.cpp:64
   │
   ├─ 创建 <AppData> 目录 + <AppData>/models/
   ├─ DatabaseManager::initialize(<AppData>/agent.db)
   ├─ NetworkClient / SettingsService / ThemeService / PlayerService
   ├─ LLMProviderService（从 SQLite 加载预设与自定义 Provider）
   ├─ AgentService（绑定 NetworkClient + Settings + Provider）
   │
   ├─ SceneDetector（无模型，纯算法）
   │
   ├─ [若 FRAMEMIND_HAS_ONNXRUNTIME]
   │   ├─ ClipService::initialize(models/clip_visual.onnx, models/clip_text.onnx)
   │   └─ EmbeddingService::initialize(models/bge-small-zh.onnx)
   │       └─ 失败：日志告警，服务置 nullptr，继续启动（不致命）
   │
   ├─ [若 FRAMEMIND_HAS_WHISPER]
   │   └─ WhisperService::initialize(models/ggml-small.bin)
   │       └─ 失败：日志告警，服务置 nullptr，继续启动（不致命）
   │
   ├─ VideoRAGStore::initialize()（建表 rag_chunks / rag_entities）
   ├─ QACacheManager / VideoRAGRetriever（注入 ClipService / EmbeddingService）
   ├─ VideoIndexer / VideoAnalysisService
   ├─ ToolRegistry（注册 6 个 Tool）
   ├─ ToolOrchestrator / ReflectionEngine / PerceptionStrategy
   └─ VideoAgent（顶层协调器）
   │
   ▼
MainWindow 显示，等待用户打开视频
```

**用户打开视频后**：
```
PlayerViewModel::openFile(path)
   │
   ├─ PlayerService::open(path)                 ← 实际解码播放
   │
   └─ [待接线，README §下一步计划]
       ├─ VideoAgent::setActiveVideo(path, VideoIndexer::computeVideoId(path))
       └─ VideoAnalysisService::onVideoOpened(path)
              │
              ▼
       VideoIndexer::startIndex(path)
         │
         ├─ StageMetadata    : 提取时长/分辨率/FPS（< 1s）
         ├─ StageSceneSplit  : 直方图差异场景分割（秒级）
         ├─ StageKeyframeEncode: CLIP encodeImages 关键帧（若 m_clip != nullptr）
         ├─ StageTranscribe  : Whisper 转写（若 m_whisper != nullptr，需 FFmpeg 抽 PCM）
         └─ StageSceneDesc / StageSummarize: VLM 按需深化（用户提问时触发）
```

---

## 七、降级与容错策略

| 故障点 | 检测方式 | 降级行为 |
|--------|---------|---------|
| ONNX 模型文件缺失 | `ClipService::initialize` 返回 `false` | 服务置 `nullptr`，`VideoIndexer::StageKeyframeEncode` 跳过，RAG 检索失去视觉路径 |
| Whisper 模型缺失 | `WhisperService::initialize` 返回 `false` | 服务置 `nullptr`，`StageTranscribe` 跳过，无法回答语音相关问题 |
| CUDA EP 不可用 | ONNX Runtime 抛异常 | `OnnxRuntimeEngine` 自动回退 CPU EP |
| VLM API Key 未填 | `LLMProviderService::getApiKey` 返回空 | `AgentService` 返回错误提示，引导用户进「文件 → AI 设置」 |
| VLM API 超时 / 限流 | `NetworkClient` 超时或 HTTP 429 | `VideoAnalysisService` 标记场景描述为空，Agent 在 REFLECT 阶段降级为"基于转写文本回答" |
| CLIP BPE tokenizer 未实现 | `ClipService::tokenizeText` TODO | 文本检索路径不可用，仅能用 BGE 文本检索 + 直方图场景 |
| FFmpeg 未集成 | `VideoIndexer::buildLevel1` 占位 | Whisper 转写无法运行，需先打通音频抽取 |
| SQLite 锁 / 损坏 | `DatabaseManager` 异常 | 整个 RAG 检索不可用，UI 仍可播放视频 |

**关键原则**（来自 `dev-tasks.md` 通用约束）：
> 任何外部调用（SDK / 网络 / 文件 / 模型）都要给降级或可见错误提示，禁止吞异常。

---

## 八、性能预算与硬件建议

**目标耗时**（来自 `video-rag-plan.md` §八）：

| 环节 | 目标耗时 | 优化策略 |
|------|---------|---------|
| 场景分割（1min 视频） | < 2s | 抽帧（每 0.5s 一帧），降采样到 64×64 |
| CLIP 编码（20 帧） | < 1s | batch 推理，ONNX GPU EP |
| Whisper 转写（1min 音频） | < 5s | small 模型 + INT8 量化 |
| FAISS 检索（10K 向量） | < 10ms | IVF 索引，内存常驻 |
| VLM 场景描述（单场景） | 2~4s | 异步 + 优先级队列 |
| 端到端问答响应 | < 8s | QA 缓存（阈值 0.88）+ 预计算 + 流式输出 |

**硬件建议**：

| 配置等级 | CPU | RAM | GPU | 可启用能力 |
|---------|-----|-----|-----|----------|
| 最低 | 4 核 i5 | 8GB | 无 | 直方图场景分割 + 兜底对话 |
| 推荐 | 8 核 i7 / Ryzen 7 | 16GB | 无 | 全部小模型 CPU 推理 + 云端 VLM |
| 高配 | 8 核+ | 32GB | RTX 3060+ | ONNX CUDA EP + 本地 Ollama VLM |
| 极致 | 16 核+ | 64GB | RTX 4090 | 全本地化（含 whisper-medium + qwen2-vl-13b） |

**内存占用估算**（推荐配置，加载完模型常驻）：
- CLIP 视觉 + 文本 ONNX：~1.2GB
- BGE-small ONNX：~200MB
- Whisper small：~600MB
- ONNX Runtime 本身：~100MB
- **合计常驻 ~2.1GB**（不含视频解码缓冲）

---

## 九、配置验收清单

完成以下全部步骤即视为模型配置就绪：

### 9.1 第三方库就位

- [ ] `third_party/onnxruntime/include/onnxruntime_cxx_api.h` 存在
- [ ] `third_party/onnxruntime/lib/onnxruntime.lib` 存在
- [ ] `third_party/onnxruntime/bin/onnxruntime.dll` 存在
- [ ] `third_party/whisper.cpp/CMakeLists.txt` 存在（git clone 完整）

### 9.2 模型权重就位

- [ ] `models/clip_visual.onnx` 存在且可加载
- [ ] `models/clip_text.onnx` 存在且可加载
- [ ] `models/bge-small-zh.onnx` 存在且可加载
- [ ] `models/ggml-small.bin` 存在且可加载

### 9.3 CMake 构建通过

- [ ] `cmake -D FRAMEMIND_ENABLE_ONNX=ON -D FRAMEMIND_ENABLE_WHISPER=ON ...` 配置成功
- [ ] `cmake --build build --config Debug` 编译通过
- [ ] `build/Debug/onnxruntime.dll` 已自动拷贝到 exe 同目录

### 9.4 运行时自检

- [ ] 程序启动无崩溃，日志无 "ClipService initialize failed"
- [ ] 「文件 → AI 设置」中至少配置了一个 `supportsVision=true` 的 Provider
- [ ] API Key 经 DPAPI 保存，重启后仍可读取
- [ ] 打开一个 mp4 后，索引进度条能跑完 StageMetadata + StageSceneSplit
- [ ] CLIP/Whisper 启用后，StageKeyframeEncode / StageTranscribe 不报错

### 9.5 端到端冒烟

- [ ] 打开视频后问 "视频讲了什么" → VLM 能生成摘要
- [ ] 问 "有 X 的画面在什么时候" → CLIP 检索能返回时间戳
- [ ] 问 "视频里说了什么" → Whisper 转写文本被检索到

---

## 十、附录：下载链接与版本号

### 10.1 第三方库

| 组件 | 版本 | 下载地址 |
|------|------|---------|
| ONNX Runtime | 1.18.1（win-x64） | https://github.com/microsoft/onnxruntime/releases/tag/v1.18.1 |
| whisper.cpp | master（HEAD） | https://github.com/ggerganov/whisper.cpp |
| FAISS | 1.7.4（规划中） | https://github.com/facebookresearch/faiss |

### 10.2 模型权重

| 模型 | HuggingFace 仓库 |
|------|-----------------|
| CLIP ViT-B/32 | https://huggingface.co/openai/clip-vit-base-patch32 |
| BGE-small-zh-v1.5 | https://huggingface.co/BAAI/bge-small-zh-v1.5 |
| Whisper ggml | https://huggingface.co/ggerganov/whisper.cpp |
| TransNetV2 ONNX | https://huggingface.co/ohenrik/transnetv2-onnx |

### 10.3 云端 API 申请入口

| Provider | 控制台 |
|----------|--------|
| OpenAI | https://platform.openai.com/api-keys |
| 阿里云百炼（Qianfan） | https://dashscope.console.aliyun.com/ |
| DeepSeek | https://platform.deepseek.com/ |
| 智谱 Zhipu | https://open.bigmodel.cn/usercenter/apikeys |
| Ollama（本地） | https://ollama.com/download |

---

## 十一、下一步行动项

按优先级排序，可直接转为 dev-tasks 卡片：

1. **【M4-0】下载并放置 ONNX Runtime 1.18.1** — 验证 `FRAMEMIND_ENABLE_ONNX=ON` 链接通过
2. **【M4-0】clone whisper.cpp 到 `third_party/`** — 验证 `FRAMEMIND_ENABLE_WHISPER=ON` 编译通过
3. **【M4-0】下载 4 个模型权重到项目根 `models/`** — CLIP×2 + BGE + Whisper small
4. **【M4-1】实现 CLIP BPE tokenizer（C++）** — 移植 `clip/simple_tokenizer.py`，解锁文本检索
5. **【M4-1】实现 BGE WordPiece tokenizer（C++）** — 含 BasicTokenizer（中文分词）
6. **【M4-1】FFmpeg 音频抽取集成** — 让 `StageTranscribe` 真正能跑
7. **【M4-2】VLM/LLM 双 Provider 通道** — `SettingsService` 加 `activeVlmProviderId` /
    `activeLlmProviderId`，`VideoAnalysisService::oneShotVLM` 用 VLM Provider
8. **【M4-2】`ChatViewModel` 接线 `VideoAgent::ask`** — 让 UI 真正用上 Agent 决策
9. **【M4-3】索引进度 UI** — `ChatView` 顶部状态条绑定 `VideoIndexer::progress`
10. **【M5】FAISS 集成（可选）** — 当单视频向量数 > 10K 时升级 `VideoRAGStore`
