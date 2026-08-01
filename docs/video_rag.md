我已了解设计文档，现在深入实际代码验证实现状态。
核心问题很可能在索引采样环节。并行读取关键实现。
核心问题已浮现。继续验证证据注入与截帧机制。
发现关键缺陷。再确认 system prompt 组装与截帧实现。
致命缺陷已确认。最后核实截帧质量与上下文传递。
已通读全部 RAG 链路代码。结论：**问题不在"模型能力"，而在于索引采样、证据注入两处存在架构性断链——你的 RAG 检索结果实际上从未进入 LLM 的 prompt**。

---

## 一、致命缺陷（P0，直接导致"帧理解很有限"）

### 1. RAG 检索到的证据被丢弃，从未注入 prompt

```99:106:src/service/agent/video_agent.cpp
    if (m_retriever && !m_activeVideoId.isEmpty()) {
        VideoRAGRetriever::Constraints c;
        c.videoId = m_activeVideoId;
        m_retrievedEvidence = m_retriever->retrieve(question, c, 5);
    }

    // === REASON + ACT: 通过 ToolOrchestrator 让 LLM 决定是否调工具 ===
    phaseReasonAndAct(conversationId, question, userFrames, videoCtx);
```

`m_retrievedEvidence` 只流向 `result.evidence`（给反思引擎和 UI），**没有任何一行代码把它拼进 messages**。三路检索 + RRF 融合的全部结果对模型是不可见的。这是整个系统"R"环节的空转。

### 2. `sceneOverview` 构建了但被 system prompt 忽略

```68:78:src/service/agentservice.cpp
    if (!ctx.isEmpty()) {
        prompt += QStringLiteral("\n# 当前视频信息\n");
        if (!ctx.fileName.isEmpty())
            prompt += QStringLiteral("- 文件名: %1\n").arg(ctx.fileName);
        if (ctx.durationMs > 0)
            prompt += QStringLiteral("- 总时长(ms): %1\n").arg(ctx.durationMs);
        if (ctx.width > 0 && ctx.height > 0)
            prompt += QStringLiteral("- 分辨率: %1x%2\n").arg(ctx.width).arg(ctx.height);
        if (!ctx.videoSummary.isEmpty())
            prompt += QStringLiteral("\n# 视频摘要\n%1\n").arg(ctx.videoSummary);
    }
```

`ctx.sceneOverview`（带时间戳的全场景清单，`buildVideoContext` 里辛苦拼好的）没有被使用。模型手上只有一段 200~500 字的全局摘要，**没有任何带时间戳的细粒度信息**，自然答不准"什么时候"。

### 3. 全程没有把画面喂给模型

`RetrievalResult` 只承载文本；检索命中的关键帧从未作为 `image_url` 回传 VLM。`Scene::keyframePath` 始终为空（无人赋值），重启后连图都取不回来。除了用户手动点「📷 当前帧」，**模型在回答时看不到任何一帧画面**——这就是"对视频帧理解很有限"的直接原因。

### 4. 采样密度低到无法支撑帧级理解

```33:42:src/service/agent/video_indexer.cpp
int computeSampleCount(int64_t durationMs, bool dense)
{
    if (durationMs <= 0) return kMinSampleCount;
    if (dense) {
        const int count = static_cast<int>(durationMs / 1000 * kDenseFps);
        return std::clamp(count, kMinSampleCount, kMaxDenseSample);
    }
    const int count = static_cast<int>(durationMs / 1000 / kSparseSamplePerSec);
    return std::clamp(count, kMinSampleCount, kMaxSparseSample);
}
```

实测密度：

| 视频时长 | 实际采样间隔 | 场景边界时间误差 |
|---|---|---|
| 10 min | 1 s | ±1 s |
| 30 min | 3 s（回退直方图）| ±3 s |
| 60 min | 18 s（上限 200 帧）| **±18 s** |

场景 `startMs/endMs` 直接取采样点时间戳，误差即上表末列。长视频的时间定位在物理上就不可能准。

### 5. 每个场景只用 1 帧代表，且是首帧

```138:141:src/service/scene_detector.cpp
            s.startMs  = timestampsMs[sceneStartIdx];
            s.endMs    = timestampsMs[i + 1];
            s.keyframe = frames[sceneStartIdx];
            scenes.append(s);
```

一个可能长达数十秒的场景，视觉索引只有首帧的 1 个 CLIP 向量。场景内的动作、进出画面的物体、文字变化全部丢失。视觉检索粒度 = 场景级，这是最粗的可能粒度。

### 6. 截帧走 `extractThumbnail`，每帧重开一次文件

```120:162:src/service/playerservice.cpp
QFuture<QImage> PlayerService::captureFrameAt(int64_t posMs, int timeoutMs)
{
    ...
    return QtConcurrent::run([filePath, posMs]() -> QImage {
        opts.positionMs  = posMs;
        opts.targetWidth = 0;      // 原始尺寸
        opts.jpegQuality = 2;

        bool ok = SmartPlayer::extractThumbnail(
            filePath.toUtf8().constData(),
            tmpFile.toUtf8().constData(),
            opts);
```

单帧代价 = 打开文件 + 建解码器 + seek + 解码 + JPEG 编码 + 写盘 + 读盘 + 删盘。而 `sampleFrames` 里逐帧 `future.waitForFinished()` 串行等待：

```333:346:src/service/agent/video_indexer.cpp
    for (int i = 0; i < count; ++i) {
        if (m_cancelRequested) break;
        const int64_t ts = static_cast<int64_t>(
            (static_cast<double>(i) + 0.5) / count * durationMs);
        auto future = m_player->captureFrameAt(ts, 2000);
        future.waitForFinished();
```

**这形成了死锁式的权衡**：因为截帧太慢，只能把采样数压到 200；因为采样数只有 200，帧理解必然很差。这是 P0 里的根因项——不解决它，其他优化都受限。

### 7. 中文查询走视觉检索 = 随机噪声

`ClipService` 加载的是 OpenAI CLIP ViT-B/32（`clip_vocab.json` + BPE merges），**纯英文模型**。`encodeText("穿红衣服的人")` 经英文 BPE 切成无意义字节片段，输出向量与图像空间毫无对齐关系。视觉路对中文提问的召回等价于随机排序。

---

## 二、架构级缺陷（P1）

| # | 问题 | 位置 / 证据 | 影响 |
|---|---|---|---|
| 8 | TransNetV2 被误用 | `detectScenes` 在 1 fps 采样帧上跑 TransNetV2 | 该模型要求 **连续原始帧**（25fps 邻接窗口），1fps 输入下相邻帧差异巨大 → 大量误检边界 |
| 9 | TransNetV2 窗口语义错 | `transnetBatchPredict` 把 100 帧当独立 batch 填充 | 官方需前后各 25 帧 padding、仅中间 50 帧输出有效；当前取全部 100 帧输出，边界帧预测不可信 |
| 10 | 阈值语义混用 | `score > m_threshold`，probs（sigmoid 概率）与直方图 Bhattacharyya 距离共用同一阈值 | 两者量纲完全不同；`refineBoundariesWithTransNet` 里另用 0.3，说明设计上已自相矛盾 |
| 11 | CLIP 与 BGE 向量靠维度区分 | `store.search` 中 `target.size() != queryVector.size()` 即跳过 | CLIP ViT-B/32 与 bge-small-zh 均为 **512 维**，无法区分，错配时静默返回垃圾分数 |
| 12 | 场景描述以原始 JSON 串入库并 embedding | `repr->sceneDescriptions.insert(sceneId, desc)`，desc 是含 ```json 包裹的整段 | 结构噪声（键名、括号）稀释语义，BGE 向量质量大幅下降；且未设 `response_format`，无解析 |
| 13 | 无 rerank / 无查询改写 / 无时间路 | `retrieve()` 仅 RRF 后直接 topK | 设计文档写了 `temporalSearch`，代码未实现；"开头讲了什么"无法走时间过滤 |
| 14 | 去重逻辑损失多模态互证 | `deduplicate` 按时间重叠 >0.7 丢弃后者 | 同一时段的"视觉命中 + 文本命中"被合并成一条，恰好丢掉了最有价值的交叉验证信号 |
| 15 | 意图分析为脆弱正则 | `visualCues` 中 `人[^名字]` 写法错误（字符类含"名""字"）；`needsTextSearch \|\| !needsVisualSearch` 兜底 | 绝大多数问题只走文本路，多路检索退化为单路 |
| 16 | `oneShotVLM` 与用户对话共用单例状态 | `m_currentConvId` / `m_accumulated` / `m_streaming` 为成员变量，`NetworkClient` 单 `m_activeStream` | 后台场景描述与用户提问互相踩踏；`cancelStream()` 会误杀对方流。README 已记录未修 |
| 17 | 索引重复累积 | `insertChunk` 用新 UUID + `INSERT OR REPLACE`；重启后 repr 为空 → 重跑全流程 | 同一视频的 chunk 在 DB 中无限翻倍，检索结果重复、DB 膨胀 |
| 18 | `sampleFrames` 忽略 `videoPath` | 首行 `(void)videoPath;`，实际截当前 player 打开的文件 | 用户在索引期间切换视频 → **索引到错误视频的帧**，正确性缺陷 |
| 19 | 关键帧全量常驻内存 | `Scene::keyframe` 持 QImage，`keyframePath` 永远为空 | 200 帧 × 1080p ARGB ≈ 1.6 GB；且无法持久化复用 |
| 20 | 对话历史无压缩 | `buildRequestPayload` 每次全量拼 `m_histories[convId]`，含全部 tool 消息 | 多轮后必然超 context window |

---

## 三、工程细节缺陷（P2）

| # | 问题 | 位置 |
|---|---|---|
| 21 | `tr("（位于视频 %.0f%% 处）").arg(progress)` — Qt `arg()` 只识别 `%1..%99`，printf 格式不生效，实际输出字面 `%.0f%%` | `video_analysis_service.cpp` `buildSceneContext` |
| 22 | `Scene::keyframeMs` 从未被 `SceneDetector` 赋值，恒为 0，导致取帧永远走中点兜底 | `scene_detector.cpp` |
| 23 | `m_describeQueue` 无锁，`m_activeTasks` 非原子；`setConcurrency(>1)` 时数据竞争 | `video_analysis_service.cpp` |
| 24 | 错误答案会被固化复用：置信度 ≥0.6 即写缓存，0.88 相似度即命中 | `video_agent.cpp` + `qa_cache_manager` |
| 25 | QA 缓存误命中风险："有几个人" vs "有几辆车" BGE 相似度常 >0.88 | `qa_cache_manager.cpp` |
| 26 | 语音段逐句 embedding，无 VAD 合并成语义段，粒度过碎 | `video_indexer.cpp` buildLevel1 |
| 27 | 图片统一 `detail: "auto"` + 1024 缩放，OCR / 小目标场景识别不足 | `agentservice.cpp` `makeUserMessage` |
| 28 | `ReflectionEngine` 全为长度/正则启发式，置信度无实际判别力 | `reflection_engine.cpp` |
| 29 | 无 OCR 通路、无音频事件检测（仅语音） | 全局缺失 |

---

## 四、与专业视频 RAG 方案的差距

业界成熟方案（Video-RAG / VideoAgent / LLoVi / MM-VID 一类）的标准形态与你的实现对比：

| 环节 | 专业做法 | 你的实现 | 差距 |
|---|---|---|---|
| 采样 | 1~2 fps 密采 → embedding 聚类去重 → 代表帧 | 固定 20~600 帧均匀采 | 🔴 密度低 1~2 个数量级 |
| 分层 | shot → clip → event → video 四级 | scene → video 两级 | 🟡 缺 clip/event 中间层 |
| 视觉索引 | 场景内多帧（3~8）分别编码 | 场景首帧 1 帧 | 🔴 粒度过粗 |
| Caption | **帧网格拼图**（3×3=9 帧合成一图）1 次调用理解 9 帧时序 | 单场景 1~2 帧独立调用 | 🔴 成本高 9 倍、时序信息缺失 |
| 文本模态 | ASR + **OCR** + 物体检测标签 + caption 四路 | ASR + caption 两路 | 🟡 缺 OCR / 检测标签 |
| Embedding | Chinese-CLIP / SigLIP（中文对齐） | OpenAI CLIP（英文） | 🔴 中文检索失效 |
| 检索 | query 改写 → dense + BM25 + 时间三路 → RRF → **cross-encoder rerank** | 正则意图 → 二路 → RRF | 🔴 无 rerank、无改写、无稀疏路 |
| 生成 | top-k 片段的**代表帧图像 + 字幕 + OCR + caption + 时间戳**一起注入 VLM | **什么都没注入** | 🔴🔴 断链 |
| 时间定位 | 检索定位 → VLM grounding 二次校验 | 直接输出采样点时间戳 | 🔴 误差 ±18 s |

---

## 五、优化方案

### 阶段 0：修断链（1 天，收益最大）

这三处改完，在**不动任何模型**的前提下效果会有质变——目前是"RAG 建好了但没接上"。

1. **注入检索证据**。`VideoAgent::phaseReasonAndAct` 增加 evidence 参数，在 user message 前插入一条结构化证据块：
   ```
   # 检索到的相关片段（按相关度排序）
   ## [00:32-00:45] 场景3 (视觉命中, 0.81)
   画面描述: ...
   语音: ...
   ```
2. **把 `sceneOverview` 写进 system prompt**，并采用 U 形布局（摘要在前、场景清单在后、问题在最后）。
3. **回传关键帧图像**：evidence 中 top-3 片段的代表帧，作为 `image_url` 附在 user message 上，配文字标注 `[此图为 00:32 处画面]`。这一步是"让模型真正看到画面"的关键。

### 阶段 1：重建取帧与采样（2~3 天，解开性能死锁）

4. **新增 `FrameExtractor`（FFmpeg 直接实现）**：一次 `avformat_open_input`，按递增时间戳批量 seek + 解码，输出缩放后 RGB。CMake 需补链 `swscale`（当前只链了 avformat/avcodec/avutil/swresample）。预期单帧成本从 ~100 ms 降到 ~5 ms，**采样能力从 200 帧提升到 5000+ 帧**。
5. **两阶段采样**：粗采 2 fps → 逐帧 CLIP → 按余弦相似度 >0.95 聚类去重 → 每个 shot 保留 3~8 个代表帧。视觉索引从"场景级 1 帧"变为"shot 级多帧"。
6. **场景边界精修**：直方图粗定位候选边界后，仅在边界 ±1 s 内以原始帧率密采，用 TransNetV2 精确确认（即启用已写好但未被调用的 `detectCandidateBoundaries` + `refineBoundariesWithTransNet` 两阶段路径），并给两者独立阈值。时间误差可压到 ±100 ms。
7. **关键帧落盘** `AppData/keyframes/<videoId>/<ms>.jpg`，`Scene::keyframePath` 赋值，释放内存并支持重启复用。

### 阶段 2：提升索引语义质量（3~4 天）

8. **帧网格拼图 caption**：把一个 shot 的 6~9 个代表帧拼成 3×3 网格图（每格左上角烧入时间戳），**1 次 VLM 调用**产出该 shot 的时序描述。VLM 调用数降低 6~9 倍，同时获得帧间动态信息——这是当前架构最高性价比的一项改造。
9. **替换为 Chinese-CLIP ViT-B/16**（或 SigLIP-so400m 多语言版），解决中文视觉检索失效。同时给 store 的 chunk 增加 `embeddingModel` 字段，杜绝 CLIP/BGE 维度撞车。
10. **caption 结构化解析**：请求侧加 `response_format: {"type":"json_object"}`，落库时拆字段——`summary` 单独 embedding 供检索，`entities` 写入 `entity_profiles`，`actions`/`camera` 存 metadata。避免整段 JSON 污染向量。
11. **新增 OCR 通路**：对代表帧提取画面文字（可先用 VLM caption 中的 `text` 字段，后续接 PaddleOCR ONNX），单独建 chunk。「视频里那个标题写的什么」这类问题目前完全无解。
12. **ASR 语义分段**：按静音间隔 + 标点合并为 15~30 s 语义段再 embedding，替代逐句。

### 阶段 3：检索与生成（2~3 天）

13. **查询改写**：LLM 把用户问题拆成 `{视觉查询, 文本查询, 时间约束, 目标实体}` 结构，替代当前脆弱正则。
14. **补齐三路 + rerank**：加入 BM25 稀疏路和时间路；RRF 后接 cross-encoder（bge-reranker-base ONNX）重排 top-20 → top-5。
15. **去重改为合并**：同一时段的多路命中合并为单条 evidence 并**提升其置信度**，而非丢弃后者。
16. **时间定位二次校验**：涉及"什么时候"的问题，对候选时间点调 `seek_and_analyze` 做 grounding 确认后再输出时间戳。

### 阶段 4：稳定性（1~2 天）

17. `oneShotVLM` 迁移到**独立 `AgentService` 实例 + 独立 `NetworkClient`**，彻底隔离后台索引与用户对话。
18. **索引幂等**：`chunkId` 改为 `hash(videoId + collection + startMs + chunkType)` 确定性生成；写入前 `invalidateVideo` 或按 videoId 清理旧 chunk，消除重复累积。
19. `sampleFrames` 使用传入的 `videoPath`（配合阶段 1 的 `FrameExtractor` 天然解决），修复切换视频时索引错文件。
20. **QA 缓存加固**：阈值提到 0.95，并要求命中项的 `evidenceSceneIds` 与当前检索结果有交集；置信度门槛提到 0.8。
21. 修复 `%.0f`、`keyframeMs` 未赋值、描述队列加锁等 P2 项。
22. **对话历史压缩**：超过 N 轮时将早期轮次摘要化，tool 结果消息只保留最近 2 轮。

---

## 六、优先级与预期

| 阶段 | 工作量 | 预期效果 |
|---|---|---|
| 阶段 0 | 1 天 | **最高性价比**。检索结果和画面首次真正到达模型，"帧理解有限"的主观感受立刻改善 |
| 阶段 1 | 2~3 天 | 解开性能死锁，采样能力 ×25，时间定位误差从 ±18 s → ±100 ms |
| 阶段 2 | 3~4 天 | 索引语义质量质变；VLM 成本降 6~9 倍；中文视觉检索从不可用变可用 |
| 阶段 3 | 2~3 天 | 检索准确率提升，复杂问题可答 |
| 阶段 4 | 1~2 天 | 消除并发踩踏、DB 膨胀、错误答案固化 |

---

