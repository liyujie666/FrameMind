
---

## Video RAG 框架方案 — FrameMind 视频理解系统

基于你现有项目架构（MVVM + DIContainer + EventBus）和已完成的 M2 能力（SSE 流式对话、多模态图片发送、PlayerService），我为你设计如下 RAG 方案：

---

### 一、整体架构（大小模型协作）

```
┌─────────────────────────────────────────────────────────────────────────┐
│                         Video RAG Pipeline                               │
│                                                                         │
│  ┌──── 感知层 (小模型/本地推理) ────┐   ┌──── 推理层 (大模型/云端) ────┐  │
│  │                                  │   │                              │  │
│  │  CLIP (ONNX) → 视觉 Embedding   │   │  GPT-4o / Qwen-VL-Max      │  │
│  │  Whisper.cpp → 语音转文字         │   │  → 场景描述生成             │  │
│  │  BGE-small (ONNX) → 文本 Embed   │   │  → 多帧联合推理             │  │
│  │  TransNetV2 → 场景分割            │   │  → 对话回答生成             │  │
│  │  直方图差异 → 场景边界检测        │   │  → Tool Calling 决策       │  │
│  │                                  │   │                              │  │
│  └──────────────────────────────────┘   └──────────────────────────────┘  │
│                                                                         │
│  ┌──── 存储层 ──────────────────────────────────────────────────────┐   │
│  │  FAISS (本地向量库)  │  SQLite (结构化数据/对话/缓存)              │   │
│  └──────────────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────────────┘
```

**大小模型分工原则**：

| 任务 | 小模型（本地ONNX） | 大模型（云端API） |
|------|-------------------|------------------|
| 场景分割 | ✅ 直方图差异 / TransNetV2 | — |
| 关键帧 Embedding | ✅ CLIP ViT-B/32 | — |
| 语音转文字 | ✅ Whisper-base/small | — |
| 文本 Embedding | ✅ BGE-small-zh | — |
| 场景描述生成 | — | ✅ VLM (多帧理解) |
| 视频摘要 | — | ✅ LLM (汇总) |
| 问题分类/路由 | — | ✅ LLM |
| 复杂推理/回答 | — | ✅ VLM + Tool Calling |

---

### 二、视频源适配层设计（本地 + 拉流统一）

由于你的视频来源包含**本地文件**和**拉流（RTSP/RTMP/HLS）**，需要一个统一的视频源抽象：

```cpp
// src/service/video_source.h
class IVideoSource {
public:
    enum class SourceType { LocalFile, LiveStream };
    
    virtual SourceType type() const = 0;
    virtual int64_t durationMs() const = 0;         // 拉流返回-1(未知)
    virtual bool isSeekable() const = 0;            // 拉流=false
    virtual bool isLive() const = 0;
    virtual QString sourceUri() const = 0;
    
    // 帧获取接口（统一）
    virtual QImage captureFrameAt(int64_t timestampMs) = 0;   // 本地可seek截帧
    virtual QImage captureCurrentFrame() = 0;                  // 拉流只能截当前帧
    virtual std::vector<QImage> sampleFrames(int count) = 0;  // 均匀/自适应采样
};

class LocalVideoSource : public IVideoSource { ... };    // 基于现有 PlayerService
class StreamVideoSource : public IVideoSource { ... };   // 拉流适配
```

**本地 vs 拉流的 RAG 策略差异**：

| 能力 | 本地视频 | 拉流 |
|------|---------|------|
| 全量索引 | ✅ 加载时完成场景分割 + embedding | ❌ 只能渐进式 |
| Seek截帧 | ✅ 任意时间点 | ❌ 只有当前帧/缓冲区 |
| 场景分割 | 全量预处理 | 滑动窗口实时检测 |
| Whisper | 全量离线转写 | 流式增量转写 |
| CLIP索引 | 关键帧批量编码 | 每N秒增量编码 |
| 回看分析 | 可回溯任意片段 | 依赖环形缓冲区 |

---

### 三、RAG 核心流水线

#### 3.1 索引阶段（Indexing Pipeline）

```
视频打开
   │
   ├─[Level 0] 即时 (< 1s)
   │   ├── 提取元信息 (PlayerService → 时长/分辨率/FPS/编码)
   │   └── 场景边界检测 (直方图差异, 本地轻量算法)
   │
   ├─[Level 1] 快速索引 (后台线程, 3~10s)
   │   ├── 关键帧提取 (每场景1-2帧代表帧)
   │   ├── CLIP Embedding (ONNX Runtime, batch推理)
   │   ├── Whisper 转写 (whisper.cpp, 音频流)
   │   ├── 文本 Embedding (BGE-small, 对转写文本分段编码)
   │   └── 写入 FAISS + SQLite
   │
   └─[Level 2] 按需深化 (用户提问时触发)
       ├── VLM 场景描述 (选择性, 只描述相关场景)
       ├── 实体识别与追踪
       └── 视频摘要生成
```

#### 3.2 检索阶段（Retrieval Pipeline）

```cpp
// src/service/video_rag_retriever.h
class VideoRAGRetriever {
public:
    struct RetrievalResult {
        int64_t timestampMs;
        int sceneId;
        float score;
        QString content;       // 场景描述/转写文本
        QImage keyframe;       // 关键帧（按需加载）
    };

    // 多路检索 + RRF 融合
    std::vector<RetrievalResult> retrieve(const QString& query, int topK = 5);

private:
    // 路线1: 视觉语义检索 (CLIP text→image)
    std::vector<RetrievalResult> visualSearch(const QString& query, int topK);
    
    // 路线2: 文本语义检索 (BGE embedding, 在转写文本和场景描述上检索)
    std::vector<RetrievalResult> textSearch(const QString& query, int topK);
    
    // 路线3: 时间窗口检索 (基于用户提到的时间线索)
    std::vector<RetrievalResult> temporalSearch(const QString& query);
    
    // RRF 融合排序
    std::vector<RetrievalResult> reciprocalRankFusion(
        const std::vector<std::vector<RetrievalResult>>& resultSets, int topK);
};
```

**RRF (Reciprocal Rank Fusion) 公式**：

$$\text{RRF}(d) = \sum_{r \in \text{ranklists}} \frac{1}{k + \text{rank}_r(d)}$$

其中 $k=60$ 为平滑常数。

#### 3.3 生成阶段（Generation with RAG Context）

```
用户提问 "视频里那个穿红衣服的人什么时候出现的？"
         │
         ▼
┌─ 问题理解（大模型）──────────────────────────────────────┐
│  分类: TEMPORAL_LOCALIZATION + ENTITY_QUERY              │
│  关键词: "红衣服的人", "什么时候"                           │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─ 多路检索（小模型）─────────────────────────────────────┐
│  CLIP检索: encode("穿红衣服的人") → cosine search       │
│  文本检索: BGE("红衣服") → 在场景描述中搜索              │
│  时间检索: 无明确时间线索, 跳过                           │
│  RRF融合: Top-5 候选片段                                 │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─ 验证+深化（大模型 VLM）──────────────────────────────┐
│  Tool: seek_and_analyze(候选时间点)                     │
│  → VLM确认画面中是否真有"红衣服的人"                    │
│  → 去除误检, 返回准确时间戳                             │
└─────────────────────────────────────────────────────────┘
         │
         ▼
┌─ 生成回答（大模型）─────────────────────────────────────┐
│  "穿红衣服的人在 [02:15] 首次出现, 位于画面右侧…"       │
│  "[02:15]" 可点击跳转                                    │
└─────────────────────────────────────────────────────────┘
```

---

### 四、拉流场景的增量 RAG 方案

拉流（直播/监控）不能预处理全量视频，必须用**滑动窗口 + 环形缓冲**：

```cpp
// src/service/stream_indexer.h
class StreamIndexer : public QObject {
    Q_OBJECT
public:
    // 环形缓冲区：保留最近 N 分钟的索引数据
    static constexpr int BUFFER_MINUTES = 30;
    
    void onNewFrame(int64_t timestampMs, const QImage& frame);
    
private:
    // 每隔 interval_ms 取一帧做 CLIP embedding
    int m_samplingIntervalMs = 2000;  // 每2秒采一帧
    
    // 场景变化检测（滑动窗口直方图比较）
    bool detectSceneChange(const QImage& current, const QImage& previous);
    
    // 增量 Whisper（流式音频分段送入）
    void onAudioChunk(const QByteArray& pcmData, int64_t startMs);
    
    // 环形 FAISS 索引（定期淘汰老数据）
    void evictOldEntries(int64_t olderThanMs);
    
    // 环形缓冲区
    struct FrameEntry {
        int64_t timestampMs;
        QImage thumbnail;              // 缩略图
        std::vector<float> embedding;  // CLIP向量
        QString sceneDesc;             // 场景描述(按需生成)
    };
    QQueue<FrameEntry> m_frameBuffer;  // 环形队列, FIFO淘汰
};
```

**拉流 RAG 与本地 RAG 的统一抽象**：

```cpp
// src/service/video_analysis_service.h
class VideoAnalysisService : public QObject {
public:
    enum class AnalysisMode {
        Offline,    // 本地文件: 全量预处理
        Streaming   // 拉流: 增量实时索引
    };
    
    void startAnalysis(IVideoSource* source);
    
    // 统一检索接口（内部根据 mode 走不同策略）
    QFuture<std::vector<RetrievalResult>> retrieve(const QString& query, int topK);
    
    // 统一的视频上下文（供 AgentService 注入 prompt）
    VideoContext buildContext(const QString& question);
    
signals:
    void analysisProgress(int percent, const QString& stage);
    void sceneDetected(int sceneId, int64_t startMs, int64_t endMs);
    void transcriptReady(int64_t startMs, int64_t endMs, const QString& text);
};
```

---

### 五、模块依赖与集成架构（融入现有项目）

```
                          DIContainer
                              │
          ┌───────────────────┼────────────────────┐
          ▼                   ▼                    ▼
   ┌─────────────┐    ┌──────────────┐     ┌──────────────┐
   │PlayerService│    │AgentService  │     │VideoAnalysis │
   │(已有,M1)    │    │(已有,M2)     │     │Service (新)  │
   └──────┬──────┘    └──────┬───────┘     └──────┬───────┘
          │                  │                    │
          │                  │         ┌──────────┼──────────┐
          │                  │         ▼          ▼          ▼
          │                  │   ┌─────────┐ ┌────────┐ ┌─────────┐
          │                  │   │SceneSplit│ │Whisper │ │CLIP     │
          │                  │   │Detector  │ │Service │ │Service  │
          │                  │   └─────────┘ └────────┘ └─────────┘
          │                  │         小模型层 (ONNX Runtime)
          │                  │                    │
          │                  │                    ▼
          │                  │            ┌──────────────┐
          │                  │            │VideoRAG      │
          │                  │            │Retriever     │
          │                  │            └──────┬───────┘
          │                  │                   │
          │                  ▼                   ▼
          │         ┌────────────────────────────────────┐
          └────────▶│   ToolOrchestrator (新, M4)        │
                    │   管理 Agent 多步推理 + Tool调用     │
                    └────────────────────────────────────┘
```

---

### 六、新增文件/模块清单

```
src/
├── service/
│   ├── video_source.h/.cpp              # 视频源抽象接口
│   ├── video_analysis_service.h/.cpp    # 视频分析主服务
│   ├── scene_detector.h/.cpp            # 场景分割（直方图/TransNet）
│   ├── clip_service.h/.cpp              # CLIP推理封装
│   ├── whisper_service.h/.cpp           # Whisper转写封装
│   ├── embedding_service.h/.cpp         # 文本Embedding(BGE-small)
│   ├── video_rag_retriever.h/.cpp       # 多路检索+RRF融合
│   ├── stream_indexer.h/.cpp            # 拉流增量索引器
│   ├── tool_orchestrator.h/.cpp         # Tool Calling编排
│   └── entity_tracker.h/.cpp           # 实体追踪与共指消解
├── model/
│   ├── video_representation.h           # 视频三层表示数据结构
│   ├── scene.h                          # 场景结构体
│   ├── speech_segment.h                 # 语音段结构体
│   └── retrieval_result.h              # 检索结果结构体
└── infrastructure/
    ├── onnx_runtime_engine.h/.cpp       # ONNX Runtime统一封装
    ├── faiss_index.h/.cpp               # FAISS向量索引封装
    └── ring_buffer.h                    # 环形缓冲区模板
```

---

### 七、技术选型与依赖

| 组件 | 选型 | 理由 |
|------|------|------|
| 视觉Embedding | CLIP ViT-B/32 (ONNX, ~350MB) | 平衡速度与质量，C++ ONNX Runtime原生支持 |
| 语音转写 | whisper.cpp (base/small模型) | 纯C++，无Python依赖，支持流式 |
| 文本Embedding | BGE-small-zh-v1.5 (ONNX, ~100MB) | 中文效果好，体积小 |
| 向量检索 | FAISS (CPU版) | 单机够用，支持增量插入 |
| 场景分割 | OpenCV直方图差异 + 可选TransNetV2 | 先用轻量算法，精度不够再升级 |
| VLM | GPT-4o / Qwen-VL-Max / 通义千问 | 通过已有LLMProviderService切换 |
| 推理框架 | ONNX Runtime 1.16+ | 统一所有小模型推理，GPU加速可选 |

**CMake 新增依赖**：
```cmake
# ONNX Runtime
find_package(onnxruntime REQUIRED)  # 或手动指定路径

# FAISS (CPU)
find_package(faiss REQUIRED)

# whisper.cpp (作为子模块或预编译库)
add_subdirectory(third_party/whisper.cpp)
```

---

### 八、性能预算与优化策略

| 环节 | 目标耗时 | 优化策略 |
|------|---------|---------|
| 场景分割 (1min视频) | < 2s | 抽帧比较(每0.5s一帧)，OpenCV GPU加速 |
| CLIP编码 (20帧) | < 1s | batch推理，ONNX GPU EP |
| Whisper转写 (1min音频) | < 5s | small模型+INT8量化 |
| FAISS检索 (10K向量) | < 10ms | IVF索引，内存常驻 |
| VLM场景描述 (单场景) | 2~4s | 异步+优先级队列 |
| 端到端问答响应 | < 8s | 缓存+预计算+流式输出 |

---

### 九、渐进式开发建议（融入现有里程碑）

| 阶段 | 任务 | 产出 |
|------|------|------|
| **M3 第一步** | 场景分割 + VideoIndexer后台线程 + 关键帧提取 | 视频加载后能显示"共N个场景" |
| **M3 第二步** | VLM场景描述 + 全视频摘要 + system prompt注入 | 能回答"视频讲了什么" |
| **M3 第三步** | 时间戳点击跳转 | `[mm:ss]` 可交互 |
| **M4 第一步** | ONNX Runtime集成 + CLIP embedding + FAISS | 能搜索"有XX的画面" |
| **M4 第二步** | whisper.cpp + 文本embedding | 能回答语音相关问题 |
| **M4 第三步** | VideoRAGRetriever多路检索 + RRF | 综合检索能力 |
| **M4 第四步** | ToolOrchestrator + 6个Tool | Agent可多步推理 |
| **M4 第五步** | StreamIndexer(拉流增量) | 拉流场景可用 |

---

### 十、关键设计决策总结

1. **大小模型解耦**：感知/编码用本地小模型（零网络延迟），理解/推理用云端大模型（能力上限高）
2. **统一视频源抽象**：本地和拉流共用 `IVideoSource` 接口，上层无感知
3. **渐进式索引**：Level 0 → Level 1 → Level 2，不让用户等
4. **多路检索融合**：视觉(CLIP) + 文本(BGE) + 时间(规则)，RRF融合取长补短
5. **环形缓冲拉流**：拉流用滑动窗口保持最近N分钟可检索，老数据FIFO淘汰
6. **按需深化**：场景描述只在被问到时才生成，避免浪费VLM调用
