现在我已经读完了所有关键代码，可以给你一个完整准确的回答。

---

## 一、打开视频后 RAG 是怎么构建的

### 触发入口

```
PlayerViewModel::videoOpened 信号
  → ChatViewModel::setPlayerViewModel 的 lambda
  → VideoAnalysisService::onVideoOpened(filePath)
  → VideoAgent::setActiveVideo(filePath, videoId)
```

### Level 0 — 元信息 + 场景分割（< 1s，后台线程）

**入口**：`VideoIndexer::startIndex(videoPath)`，立即扔进线程池执行 `buildLevel0`。

**步骤 1：采样帧**

`sampleFrames(videoPath, durationMs, 20, &timestamps)`

- 均匀采 20 帧：`ts = (i + 0.5) / 20 * durationMs`
- 调用 `PlayerService::captureFrameAt(ts, 2000)` 截帧，同步等待
- 输出：`QVector<QImage>` + `QVector<int64_t>` 时间戳

**步骤 2：场景分割**

`SceneDetector::detectScenes(frames, timestamps)` 逐帧计算差异：

- 默认走 **直方图差异**：两帧都缩到 64×64，计算 RGB 三通道各 16 bins直方图，用 Bhattacharyya 距离（`1 - Σ√(pᵢqᵢ)`），差异 > threshold → 切换边界
- 若 `FRAMEMIND_HAS_ONNXRUNTIME` 且 TransNetV2 模型已加载：改走 **TransNetV2 批量推理**，输入 `[1, 100, 27, 48, 3]`，每帧以中心取 27 帧滑动窗口，输出每帧切换概率

**输出**：`repr->scenes`，每个 `Scene` 包含 `{id, startMs, endMs, keyframe}`

完成后 `emit levelReady(0, repr)`，repr 存入 `m_repr[videoPath]`。

---

### Level 1 — Embedding + ASR（秒级，同一后台线程）

紧接 Level 0 执行 `buildLevel1`，分两条支路，都由编译宏控制。

**支路 A：CLIP 关键帧 Embedding**（需 `FRAMEMIND_HAS_ONNXRUNTIME` + ClipService 就绪）

`ClipService::encodeImages(keyframes)` 对每个场景的关键帧：
1. resize 到 224×224，转 RGB888
2. CHW 排布 + ImageNet 均值方差归一化
3. 送入 `clip_visual.onnx`，输入 `float32[1,3,224,224]`，输出 `float32[1,512]`
4. L2 归一化

每个场景产生一个 512 维向量，写入 `VideoRAGStore::VisualFrames`：

```
VideoChunk {
  chunkId, videoId,
  startMs/endMs,        ← 场景时间范围
  chunkType = FrameDesc,
  textContent = "场景 N 关键帧",
  frameEmbedding = [512 floats]   ← CLIP 视觉向量
}
```

同时持久化到 SQLite `rag_chunks` 表，下次打开同一视频直接 `loadVideo` 读缓存。

**支路 B：Whisper ASR**（需 `FRAMEMIND_HAS_WHISPER` + WhisperService 就绪）

`AudioDecoder::decodeToFloat32(videoPath)` 解码全段音频为 16kHz PCM float32，然后：

`WhisperService::transcribe(pcmSamples)`
- 调用 `whisper_full()`，参数：beam search（或 greedy），语言 auto/zh，多线程
- 输出 `QVector<SpeechSegment>` 每段含 `{startMs, endMs, text}`，时间戳单位 10ms（centisecond × 10）

每个语音段写入 `VideoRAGStore::TextSegments`，若 EmbeddingService 就绪还顺带生成文本向量：

`EmbeddingService::embed(seg.text)` → BGE-small-zh：
1. 加前缀 `"为这个句子生成表示以用于检索相关文章：" + text`
2. BertTokenizer 分词，padding 到 512
3. 送入 `bge-small-zh.onnx`，输入 `input_ids/attention_mask/token_type_ids [1,512]`，输出 `float32[1,512]`
4. 取 `[CLS]` token，L2 归一化

完成后 `emit levelReady(1, repr)`。

---

### Level 2 — VLM 场景描述 + 全视频摘要（异步，按需）

`VideoAnalysisService` 监听 `levelReady(1)` 信号，触发 `startPrewarmAndSumarize(repr, 3)` 预热前 3 个场景。

**每个场景描述**：`doDescribeScene(sceneId, repr)`
- 取关键帧（或退化为截帧）
- 调 `oneShotVLM(kSceneDescPrompt, "场景[Xms-Yms]", frames, callback)`
  - 内部创建临时 convId，调 `AgentService::sendMessage`，走普通流式接口
  - 回调里把结果 JSON 写入 `repr->sceneDescriptions[sceneId]`
  - 再写一条 `VideoChunk{chunkType=SceneSummary, textContent=desc}` 到 `VideoRAGStore::TextSegments`
  - 若 EmbeddingService 就绪，同时生成描述的文本向量
  - `emit sceneDescribed(sceneId, desc)`

**三个场景全部描述完毕后**（`startPrewarmAndSummarize` 的计数器归零）：
- `QObject::disconnect(conn)` 断开监听
- 调用 `summarizeVideo(repr)`

**全视频摘要**：`summarizeVideo(repr)`
- 拼接已有的场景描述文本
- 调 `oneShotVLM(kVideoSummaryPrompt, scenesText, {}, callback)` — 纯文字，不带图
- 回调里写 `repr->videoSummary = summary`，`repr->level = Level2`
- `emit summaryReady(summary)`

---

### RAG 构建完成后的内存状态

```
VideoRAGStore::inMemory
  VisualFrames  → { chunkId: VideoChunk{frameEmbedding[512]} }  × 场景数
  TextSegments  → { chunkId: VideoChunk{textEmbedding[512]} }   × (语音段数 + 场景描述数)

VideoRepresentation
  scenes[]             ← 场景骨架
  speechSegments[]     ← 语音转写
  sceneDescriptions{}  ← sceneId → VLM 描述 JSON
  videoSummary         ← 全视频摘要
  level = Level2
```

SQLite `rag_chunks` 表也同步持久化，下次打开同视频直接 `loadVideo` 加载，跳过 Level 1。

---

## 二、用户提问后 Agent 如何运作

### 总入口

`ChatViewModel::doSend(text, frames)`

```
VideoContext ctx = getVideoContext()
  → m_videoAnalysis->representation(m_activeVideoPath)
  → buildContext(repr)
    输出：ctx.fileName / durationMs / sceneOverview / videoSummary

VideoAgent::ask(convId, question, frames, ctx, currentPosMs, ...)
```

---

### PERCEIVE 阶段

`VideoAgent::phasePerceive(question, repr, currentPosMs)`

调 `PerceptionStrategy::decideSampling(question, repr, currentPosMs)`，返回 `SamplingPlan`（帧密度、时间窗口），当前仅打日志，**不实际截帧**，具体感知留给 Tool层按需触发。

---

### REPRESENT 阶段

直接在 `ask()` 主函数里同步执行：

```cpp
VideoRAGRetriever::retrieve(question, {videoId}, topK=5)
```

**意图分析**（启发式正则）：
- 命中视觉关键词（颜色/画面/人/车）→ `needsVisualSearch = true`
- 命中文本关键词（说/讲/台词/字幕）→ `needsTextSearch = true`
- 都不命中 → 兜底走文本路
- 命中实体指代（那个/这个/视频里的人）→ 走实体路

**三路检索**（按意图决定走哪几路）：

**文本路**：`EmbeddingService::embed(query)` → 512 维向量 → `VideoRAGStore::search(TextSegments, ...)` 线性遍历，余弦相似度排序，topK×2 个候选

**视觉路**：`ClipService::encodeText(query)` → BPE tokenize → `clip_text.onnx` 推理 → 512 维向量 → `VideoRAGStore::search(VisualFrames, ...)` → topK×2 个候选

**实体路**：`EmbeddingService::embed(entityDesc)` → `VideoRAGStore::search(EntityProfiles, ...)` → 5 个候选

**RF 融合**：
```
score(doc) = Σ weight_i × 1/(60 + rank_i(doc))
```
按 `intent` 动态调权重（视觉偏 0.7/0.2/0.1，文本偏 0.2/0.7/0.1，均等偏 0.4/0.4/0.2），去重（时间重叠 > 70% 的丢后者），取 top 5 作为 `m_retrievedEvidence`。

---

### REASON + ACT 阶段

`ToolOrchestrator::runQuery(convId, question, frames, ctx, ...)`

**第 0 轮**：调 `AgentService::sendMessageWithTools`

组装 messages：
```
[system prompt (含 videoSummary + sceneOverview)]
[历史对话]
[user: question + frames]
```

payload 中附带所有 Tool 的 JSON Schema 定义，`tool_choice: "auto"`，流式请求。

**LM 的两种响应**：

**情况 A — finish_reason = "stop"**：LM 认为已有足够信息直接回答（比如摘要已在 system prompt 里），直接返回文本，跳过 Tool 调用。

**情况 B — finish_reason = "tool_calls"**：LM 决定调用工具，`ToolOrchestrator::executeToolsThenContinue` 顺序执行每个 Tool：

| Tool | 做什么 | 输入 | 输出 |
|---|---|---|
| `get_scene_info` | 从 `repr->scenes` + `sceneDescriptions` 取场景信息 | scene_id 或 timestamp_ms | `{scene_id, start_ms, end_ms, description}` |
| `get_transcript` | 从 `repr->speechSegments` 取转写文本 | 可选时间范围 | 语音段列表 |
| `search_video_content` | 调 `VideoRAGRetriever::retrieve` 检索 | query 文本 | top-N 相关片段 |
| `seek_and_analyze` | `PlayerService::captureFrameAt` 截帧 → `VideoAnalysisService::describeFrame` → VLM 描述 | timestamp_ms + focus |单帧描述文本 |
| `analyze_time_range` | 均匀采 2~10 帧 → `oneShotVLM` 多帧联合分析 | startMs, endMs, sampleCount | 该段过程描述 |
| `control_player` | `EventBus::requestSek` 跳转播放器 | timestamp_ms | 操作结果 |

Tool 执行完后，把 `assistant tool_calls 消息 + tool result 消息` 追加到历史，发起下一轮 `continueWithToolResults`。

如此循环，最多 `MAX_ROUNDS` 轮、`MAX_TOOL_CALLS_PER_ANSWER` 次工具调用。

---

### REFLECT 阶段

LM 最终输出 stop 后，`ReflectionEngine::reflect(answer, evidence, repr)` 评估：
- `repr->videoSummary` 是否非空作为基础信息充足性指标
- 对比 evidence 与 answer 的一致性
- 输出置信度 `[0, 1]`

置信度 ≥ 0.6 时，把这次 `(question, answer, confidence)` 写入 `QACacheManager`（向量化存 `VideoRAGStore::QACache`），下次相似问题直接命中缓存（`ask()` 开头的快速路径）。

---

### 完整链路一张图

```
用户提问
    │
    ▼
getVideoContext → buildVideoContext(repr)
    → ctx.videoSummary (若 Level2 已完成则非空)
    │
    ▼
QACacheManager::tryAnswer  ── 命中 → 直接返回历史结论
    │ 未命中
    ▼
PERCEIVE: PerceptionStrategy::decideSampling (只规划，不执行)
    │
    ▼
REPRESENT: VideoRAGRetriever::retrieve(question)
    ├─ 文本路: BGE embed → TextSegments 余弦检索
    ├─ 视觉路: CLIP text encode → VisualFrames 余弦检索
    └─ 实体路: BGE embed → EntityProfiles 余弦检索
    → RF 融合 → top5 evidence
    │
    ▼
REASON+ACT: ToolOrchestrator
    第0轮: sendMessageWithTools
    (system=摘要+场景概览, user=问题, tools=6个工具定义)
        │
        ├─ stop →直接回答（摘要足够用）
        │
        └─ tool_calls → 执行工具 → continueWithToolResults → 再次LM推理
                (最多 MAX_ROUNDS 轮)
    │
    ▼
REFLECT: ReflectionEngine::reflect → 置信度
    │
    ▼
QACacheManager::cache (置信度≥0.6 时写缓存)
    │
    ▼
onDone(AgentAnswer) → ChatViewModel 更新 UI
```