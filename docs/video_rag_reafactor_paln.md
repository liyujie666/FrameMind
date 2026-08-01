## 验证结论

我已继续对照 `docs/video_rag.md`、设计文档和当前 C++ 实现进行静态验证。结论是：

- **核心断链问题已确认**，不是单纯的模型能力问题。
- 当前 RAG 已经完成了部分索引和检索工作，但检索结果没有进入主模型 Prompt，导致“检索器工作、生成器看不到证据”。
- 部分文档中的性能数值、TransNetV2 规范和中文 CLIP 效果属于合理风险判断，但目前仓库中没有基准测试或模型文件，不能直接当作已验证事实。
- 当前没有发现测试目录或自动化 RAG 评估集，因此后续计划必须先补充可重复的验证基线。

---

# 一、已确认的问题

## P0：RAG 检索结果没有进入 LLM Prompt

`VideoAgent::ask()` 中确实执行了检索：

```99:106:d:\Qt\ffmpegProjects\FrameMind\src\service\agent\video_agent.cpp
    if (m_retriever && !m_activeVideoId.isEmpty()) {
        VideoRAGRetriever::Constraints c;
        c.videoId = m_activeVideoId;
        m_retrievedEvidence = m_retriever->retrieve(question, c, 5);
    }

    // === REASON + ACT: 通过 ToolOrchestrator 让 LLM 决定是否调工具 ===
    phaseReasonAndAct(conversationId, question, userFrames, videoCtx);
```

但 `m_retrievedEvidence` 后续只用于：

- `ReflectionEngine`
- `AgentAnswer.evidence`
- QA 缓存相关逻辑

没有传递给 `ToolOrchestrator::runQuery()`，也没有拼接到 `AgentService` 的 system、user 或 tool context 中。

因此当前真实链路是：

```text
用户问题
  -> RAG 检索
  -> evidence 暂存在 VideoAgent 内
  -> evidence 不进入 LLM
  -> LLM 只能依赖摘要、历史和主动调用工具
```

这项问题是当前最重要的功能缺陷，应当优先修复。

---

## P0：`sceneOverview` 已构建，但没有进入 system prompt

`VideoAnalysisService::buildVideoContext()` 确实填充了场景概览：

```620:640:d:\Qt\ffmpegProjects\FrameMind\src\service\agent\video_analysis_service.cpp
    QString overview;
    const int maxScenes = qMin(15, repr->scenes.size());
    for (int i = 0; i < maxScenes; ++i) {
        const Scene& s = repr->scenes[i];
        const QString desc = repr->sceneDescriptions.value(s.id);
        overview += QString::fromUtf8("- [%1-%2ms] 场景%3%4\n")
                        .arg(s.startMs).arg(s.endMs).arg(s.id)
                        .arg(desc.isEmpty() ? QString{} : QStringLiteral(": ") + desc.left(60));
    }
    if (repr->scenes.size() > maxScenes) {
        overview += tr("...（共 %1 个场景）\n").arg(repr->scenes.size());
    }
    ctx.sceneOverview = overview;
    ctx.videoSummary  = repr->videoSummary;
```

但是 `AgentService::buildSystemPrompt()` 只写入了文件名、时长、分辨率和 `videoSummary`，没有使用 `ctx.sceneOverview`。

```41:82:d:\Qt\ffmpegProjects\FrameMind\src\service\agentservice.cpp
    if (!ctx.isEmpty()) {
        prompt += QStringLiteral("\n# 视频背景信息（仅供参考，禁止原文输出）\n");
        if (!ctx.fileName.isEmpty())
            prompt += QStringLiteral("- 文件名: %1\n").arg(ctx.fileName);
        if (ctx.durationMs > 0)
            prompt += QStringLiteral("- 总时长(ms): %1\n").arg(ctx.durationMs);
        if (ctx.width > 0 && ctx.height > 0)
            prompt += QStringLiteral("- 分辨率: %1x%2\n").arg(ctx.width).arg(ctx.height);
        if (!ctx.videoSummary.isEmpty())
            prompt += QStringLiteral("\n## 视频摘要（背景参考，不可主动输出）\n%1\n")
                          .arg(ctx.videoSummary);
    }
```

这会直接影响：

- “第几个场景发生了什么”
- “前半段和后半段有什么变化”
- “某人物什么时候出现”
- “视频开头/中间/结尾发生了什么”

---

## P0：Tool Calling 主路径下 assistant 回复没有写入历史

普通路径 `sendMessage()` 会在完成时追加 assistant 消息：

```185:199:d:\Qt\ffmpegProjects\FrameMind\src\service\agentservice.cpp
        [this]() {
            m_streaming = false;
            // 记入历史
            m_histories[m_currentConvId].append(QJsonObject{
                { QStringLiteral("role"), QStringLiteral("assistant") },
                { QStringLiteral("content"), m_accumulated } });
            ChatMessage msg;
```

但 Tool Calling 路径的完成回调只是发送信号：

```327:337:d:\Qt\ffmpegProjects\FrameMind\src\service\agentservice.cpp
        [this]() {
            m_streaming = false;
            emit responseFinishedWithTools(m_currentConvId,
                                            m_pendingToolCalls,
                                            m_pendingFinishReason,
                                            m_accumulated);
        },
```

因此：

- 当前用户消息在 `buildRequestPayload()` 中会写入历史。
- assistant 最终回答不会在 Tool Calling 主路径中写入历史。
- 工具调用轮次只在下一轮请求前临时追加。
- 最终回答完成后没有形成完整的 assistant 消息闭环。

这会造成模型下一轮看不到自己上一轮的完整回答，严重影响多轮指代、上下文承接和纠错。

---

## P0：用户停止生成时，VideoAgent 路径没有正确取消

`ChatViewModel::stopGeneration()` 当前只调用：

```374:379:d:\Qt\ffmpegProjects\FrameMind\src\viewmodel\chatviewmodel.cpp
void ChatViewModel::stopGeneration()
{
    if (!m_streaming) return;
    if (m_agentService) m_agentService->stopGeneration();
    // 最终化由 responseFinished 处理
}
```

而视频问答实际走的是：

```302:344:d:\Qt\ffmpegProjects\FrameMind\src\viewmodel\chatviewmodel.cpp
if (m_videoAgent && !m_activeVideoPath.isEmpty()) {
    m_videoAgent->ask(
        m_currentConversationId,
        text,
        frames,
        ctx,
        currentPos,
```

`VideoAgent::cancel()` 虽然存在，并且会调用 `ToolOrchestrator::cancel()`，但没有从 `ChatViewModel::stopGeneration()` 调用。

因此在视频 Agent 路径中：

1. 网络请求可能被停止；
2. `ToolOrchestrator::m_running` 可能仍然保持 `true`；
3. 后续提问可能被拒绝为“Agent 正在执行，请稍后”。

这是一个确定的状态机缺陷。

---

## P1：流式内容存在重复追加风险

`ToolOrchestrator` 直接监听 `AgentService::responseChunk`：

```20:30:d:\Qt\ffmpegProjects\FrameMind\src\service\agent\tool_orchestrator.cpp
    if (m_agent) {
        connect(m_agent, &AgentService::responseChunk,
                this, &ToolOrchestrator::onAgentChunk);
```

ToolOrchestrator 再通过 `m_onProgress` 把增量转发给 `VideoAgent`，最后由 `ChatViewModel` 追加。

同时，`ChatViewModel::connectAgent()` 又直接监听 `AgentService::responseChunk`：

```41:49:d:\Qt\ffmpegProjects\FrameMind\src\viewmodel\chatviewmodel.cpp
    connect(m_agentService, &AgentService::responseChunk, this,
            [this](const QString& convId, const QString& delta) {
                if (convId != m_currentConversationId || m_assistantRow < 0) return;
                m_messageModel->appendDeltaSilent(m_assistantRow, delta);
                m_dirty = true;
            });
```

而视频路径自身还注册了 `onProgress`：

```315:319:d:\Qt\ffmpegProjects\FrameMind\src\viewmodel\chatviewmodel.cpp
            [this](const QString& delta) {
                // onProgress：流式 delta，与 AgentService::responseChunk 等效
                m_messageModel->appendDeltaSilent(m_assistantRow, delta);
                m_dirty = true;
            },
```

因此同一段 delta 可能经过两条路径写入 UI：

```text
AgentService::responseChunk
    ├─> ChatViewModel::connectAgent()
    └─> ToolOrchestrator::onAgentChunk()
          -> VideoAgent::onProgress
          -> ChatViewModel::onProgress
```

应当明确视频 Agent 路径和普通 Agent 路径的流式事件归属，不能让两个消费者同时修改同一个 UI 行。

---

## P1：索引帧采样使用当前播放器文件，而不是传入的视频路径

`VideoIndexer::sampleFrames()` 明确忽略了 `videoPath`：

```325:346:d:\Qt\ffmpegProjects\FrameMind\src\service\agent\video_indexer.cpp
QVector<QImage> VideoIndexer::sampleFrames(const QString& videoPath,
                                            int64_t durationMs, int count,
                                            QVector<int64_t>* outTimestamps)
{
    (void)videoPath;
    QVector<QImage> frames;
```

之后使用：

```337:342:d:\Qt\ffmpegProjects\FrameMind\src\service\agent\video_indexer.cpp
        auto future = m_player->captureFrameAt(ts, 2000);
        future.waitForFinished();
```

而 `PlayerService::captureFrameAt()` 从播放器当前 `m_videoInfo.filePath` 取文件：

```134:143:d:\Qt\ffmpegProjects\FrameMind\src\service\playerservice.cpp
    QString filePath;
    {
        std::lock_guard<std::mutex> lk(m_infoMutex);
        filePath = m_videoInfo.filePath;
    }
```

如果用户在后台索引期间切换视频，索引任务可能对错误的文件进行截帧。这不是性能问题，而是数据正确性问题，应优先修复。

---

## P1：场景关键帧时间没有赋值

`Scene` 有 `keyframeMs` 字段，但场景检测逻辑只设置了 `keyframe` 图像：

```144:151:d:\Qt\ffmpegProjects\FrameMind\src\service\scene_detector.cpp
            Scene s;
            s.id       = currentSceneId++;
            s.startMs  = timestampsMs[sceneStartIdx];
            s.endMs    = timestampsMs[i + 1];
            s.keyframe = frames[sceneStartIdx];
            scenes.append(s);
```

最后一个场景也同样没有设置 `keyframeMs`。

仓库中的搜索结果只发现 fallback 场景设置过：

```d:\Qt\ffmpegProjects\FrameMind\src\service\agent\video_indexer.cpp
single.keyframeMs = repr->metadata.durationMs / 2;
```

所以正常场景的 `keyframeMs` 基本恒为 `0`。当内存关键帧缺失时，代码会退回场景中点，而不是实际关键帧时间。

---

## P1：关键帧不会持久化，且可能造成较高内存占用

`Scene` 同时保存：

- `QImage keyframe`
- `QString keyframePath`

但 `SceneDetector` 没有给 `keyframePath` 赋值，`VideoIndexer` 写入视觉 chunk 时也只是读取空路径：

```238:249:d:\Qt\ffmpegProjects\FrameMind\src\service\agent\video_indexer.cpp
                c.chunkType = VideoChunk::FrameDesc;
                c.textContent = tr("场景 %1 关键帧").arg(s.id);
                c.frameEmbedding = embeddings[i];
                c.keyframePath = s.keyframePath;
```

这意味着：

- RAG 数据库无法恢复关键帧文件；
- 重启后只有 embedding，没有可回传给 VLM 的图片；
- 场景图片在内存中长期驻留；
- 后续无法按需加载检索命中的关键帧。

---

## P1：索引数据不是幂等写入

多个地方使用随机 UUID 作为 chunk ID：

```213:214:d:\Qt\ffmpegProjects\FrameMind\src\service\agent\video_indexer.cpp
c.chunkId = QUuid::createUuid().toString(QUuid::WithoutBraces);
```

场景描述和语音段也使用同样方式。

数据库虽然使用了：

```cpp
INSERT OR REPLACE
```

但由于每次 chunk ID 都不同，实际上不会覆盖旧数据。重复索引同一视频会不断累积重复 chunk。

`VideoRAGStore::invalidateVideo()` 已经存在，因此可以在开始完整重建前清理旧索引；更长期的方案是使用确定性 chunk ID。

---

## P1：历史消息会持续增长，且包含多模态图片和 Tool 结果

`buildRequestPayload()` 会把当前 user message 直接加入内存历史：

```135:139:d:\Qt\ffmpegProjects\FrameMind\src\service\agentservice.cpp
    const QJsonObject userMsg = makeUserMessage(text, frames);
    messages.append(userMsg);
    history.append(userMsg);
```

`makeUserMessage()` 会把图片编码成 base64 data URI。多轮问答后，历史中会永久保留早期图片。

Tool Calling 还会追加：

- assistant tool_calls
- tool result JSON
- 后续用户消息
- 最终 assistant 文本缺失或不完整

因此长对话容易出现：

- 请求体过大；
- 上下文超限；
- provider 拒绝畸形 tool 消息序列；
- 历史图片反复上传。

---

## P1：后台 VLM 和用户问答共用单流网络状态

`VideoAnalysisService::oneShotVLM()` 使用同一个 `AgentService`：

```706:729:d:\Qt\ffmpegProjects\FrameMind\src\service\agent/video_analysis_service.cpp
    const QString convId = QStringLiteral("__vlm_oneshot_")
                            + QUuid::createUuid().toString(QUuid::WithoutBraces);
...
    m_agent->sendMessage(convId, fullUser, frames, emptyCtx);
```

而 `NetworkClient` 只有一个：

```cpp
QNetworkReply* m_activeStream
```

每次新请求都会：

```cpp
cancelStream();
```

当前场景描述服务通过递归串行调用，已经部分规避了“多个后台 VLM 同时发送”的问题，但仍然存在以下风险：

- 用户问答会和后台场景描述竞争同一个流；
- 用户操作可能取消后台 VLM；
- 后台请求可能取消用户请求；
- `AgentService` 的 `m_currentConvId`、`m_accumulated` 和 tool 状态属于全局成员；
- 一个请求的回调容易受到另一个请求状态影响。

因此文档所说的并发隔离问题，**架构风险确认，但“索引队列永久卡死”需要运行时故障注入才能最终确认**。

---

# 二、部分确认或需要基准验证的问题

## 1. 中文 CLIP 检索风险：风险确认，随机噪声未确认

当前使用的是 OpenAI CLIP ViT-B/32 风格模型，视觉和文本编码都为 512 维：

```19:33:d:\Qt\ffmpegProjects\FrameMind\src\service\clip_service.cpp
//   clip_visual.onnx
//     - input:  "pixel_values"  float32[1, 3, 224, 224]
//     - output: "image_embeds"  float32[1, 512]
//
//   clip_text.onnx
//     - input:  "input_ids"     int64[1, 77]
//     - output: "text_embeds"   float32[1, 512]
```

中文查询直接进入 CLIP tokenizer：

```161:165:d:\Qt\ffmpegProjects\FrameMind\src\service\clip_service.cpp
std::vector<float> ClipService::encodeText(const QString& text)
{
    if (!m_textEngine->isLoaded() || text.isEmpty()) {
        return {};
    }
```

但当前项目没有：

- 中文视觉检索测试集；
- 英文、中文、同义词召回率对比；
- CLIP tokenizer 的中文覆盖率验证；
- Chinese-CLIP 或 SigLIP 对照实验。

因此应表述为：

> 当前模型与中文视频问句存在明显语义对齐风险，需要通过离线 Recall@K 测试确认影响程度；不能在没有测试数据的情况下直接断言“等同随机”。

---

## 2. TransNetV2 当前实现存在强风险，但需要按模型实际输入确认

当前代码使用：

```397:401:d:\Qt\ffmpegProjects\FrameMind\src\service\scene_detector.cpp
//   input  "input" : float32 [1, 100, 27, 48, 3]
//   output "534"   : float32 [1, 100, 1] — single-frame 切换概率
```

并按每 100 帧独立批次推理，同时对不足 100 帧的部分进行零填充。

存在需要验证的风险：

- 输入帧是否应为连续原始帧；
- 模型是否要求特定的时间窗口上下文；
- 输出概率对应当前帧、前后帧还是窗口中心；
- 最后一个 batch 的 padding 是否会污染边界；
- 当前输入像素值范围是否符合导出模型要求；
- `m_threshold` 是否同时适用于直方图距离和 TransNet 概率。

尤其是当前检测条件：

```106:109:d:\Qt\ffmpegProjects\FrameMind\src\service\scene_detector.cpp
    const bool isSparse = avgIntervalMs > 2000;
```

以及：

```120:123:d:\Qt\ffmpegProjects\FrameMind\src\service\scene_detector.cpp
        if (!probs.empty() && i < static_cast<int>(probs.size())) {
            score = probs[i];
        } else {
            score = histogramDifference(frames[i + 1], frames[i]);
```

说明当前会根据采样间隔选择不同算法，但没有将“模型要求的输入帧率”和“实际采样帧率”显式绑定。

计划中应先做模型输入/输出校验，再决定是否继续使用 TransNetV2，而不是直接重写。

---

## 3. 采样密度与性能数值需要实测

当前采样上限是：

```15:22:d:\Qt\ffmpegProjects\FrameMind\src\service\agent\video_indexer.cpp
constexpr int kDenseFps           = 1;
constexpr int kSparseSamplePerSec = 10;
constexpr int kMinSampleCount    = 20;
constexpr int kMaxDenseSample    = 600;
constexpr int kMaxSparseSample   = 200;
```

实际采样仍然是逐帧调用：

```333:346:d:\Qt\ffmpegProjects\FrameMind\src\service\agent\video_indexer.cpp
    for (int i = 0; i < count; ++i) {
        if (m_cancelRequested) break;
        const int64_t ts = static_cast<int64_t>(
            (static_cast<double>(i) + 0.5) / count * durationMs);
        auto future = m_player->captureFrameAt(ts, 2000);
        future.waitForFinished();
```

而 `captureFrameAt()` 每次会调用 `SmartPlayer::extractThumbnail()`，并创建临时 JPEG 文件。

因此“逐帧重复打开文件、采样慢”已确认；但以下数值尚未验证：

- 单帧耗时；
- 10 分钟、30 分钟和 60 分钟视频的真实采样耗时；
- FFmpeg 批量解码可以提升多少；
- 5000 帧是否适合目标硬件；
- 时间定位误差是否确实为采样间隔量级。

---

# 三、当前系统的优先级排序

我建议不要立即先做 FAISS、换模型或重写整个索引器。当前最优策略是先修复“数据已经存在但没有送到模型”的断链，再通过基准测试决定是否进行大规模架构改造。

## 第一优先级：生成链路修复

目标：让模型真正看到已有的 RAG 证据和视频结构。

### 任务 1：注入检索 evidence

建议将 `RetrievalResult` 转换为结构化上下文，至少包含：

- `startMs`
- `endMs`
- `sceneId`
- `chunkType`
- `hitPath`
- 原始相似度或融合分数
- `textContent`
- `keyframePath`

推荐注入格式：

```text
# 当前问题的检索证据
以下内容来自视频索引，仅可作为回答依据，不要把证据元数据当成事实。

## 证据 1
时间范围：00:32 - 00:45
来源：场景描述 + 语音
相关内容：会议室内几个人正在讨论方案。

## 证据 2
时间范围：01:18 - 01:31
来源：视觉检索
相关内容：画面右侧出现一名穿红色衣服的人。
```

不要把 evidence 放在 `VideoAgent` 成员变量中后只用于反思。应明确传递：

```text
VideoAgent
  -> ToolOrchestrator
      -> AgentService
          -> buildSystemPrompt / buildRequestPayload
```

更推荐将 evidence 作为当前请求的独立上下文，而不是污染用户原始问题。

### 任务 2：把 `sceneOverview` 加入 system prompt

应当：

- 将毫秒转成 `mm:ss` 或 `hh:mm:ss`；
- 保留场景 ID、时间范围和简短描述；
- 设置最大字符数；
- 对超长视频只保留摘要和相关场景；
- 不让场景描述无限扩大 system prompt。

建议结构：

```text
# 视频结构
- [00:00-00:12] 场景 0：会议室全景
- [00:12-00:35] 场景 1：人物发言
- [00:35-01:10] 场景 2：镜头切换到屏幕
```

### 任务 3：修复 Tool Calling 历史闭环

需要明确记录以下消息序列：

```text
user
assistant(tool_calls)
tool
assistant(final answer)
```

其中：

- `finish_reason == tool_calls` 时追加 assistant tool-call 消息；
- tool 结果追加 tool 消息；
- `finish_reason == stop` 时追加最终 assistant 文本；
- 普通回答和 Tool Calling 回答使用一致的历史写入机制；
- 错误、取消和超时状态不能重复追加 assistant 消息。

### 任务 4：修复停止和重复流式输出

建议采用单一输出路径：

- 普通裸 Agent：`ChatViewModel` 监听 `AgentService::responseChunk`；
- 视频 Agent：只由 `ToolOrchestrator -> VideoAgent -> ChatViewModel` 传递增量；
- `ChatViewModel` 在视频 Agent 工作期间忽略裸 `responseChunk`；
- `stopGeneration()` 根据当前工作路径调用：
  - `m_videoAgent->cancel()`
  - 或 `m_agentService->stopGeneration()`

---

## 第二优先级：数据正确性和索引幂等性

### 任务 5：让索引器真正使用 `videoPath`

不要让 `VideoIndexer` 通过当前播放器状态间接截帧。建议增加明确接口：

```text
VideoIndexer
  -> FrameExtractor(videoPath)
```

短期方案可以给 `PlayerService` 增加带路径的截帧接口；长期方案再引入独立 `FrameExtractor`。

必须满足：

- 索引任务绑定固定 `videoPath`；
- 视频切换不会影响正在运行的索引任务；
- 任务开始时记录 `videoId`；
- 返回结果前校验任务是否仍是当前任务；
- 取消旧任务后不能把旧结果写入当前视频。

### 任务 6：修复 `keyframeMs`

每个正常场景至少应设置：

```text
keyframeMs = timestampsMs[sceneStartIdx]
```

同时要明确关键帧策略：

- 首帧用于场景边界；
- 中间帧用于场景内容描述；
- 多帧用于动作和时序理解。

不应让 `keyframeMs == 0` 代表所有场景。

### 任务 7：关键帧落盘和按需加载

建议目录：

```text
<AppData>/keyframes/<videoId>/<timestamp>.jpg
```

写入数据库时保存：

- `keyframe_path`
- `keyframe_ms`
- `scene_id`
- 图像宽高
- 图像生成版本

检索结果返回时按需加载缩略图，只有需要 VLM 复核时才读取较高分辨率图片。

### 任务 8：索引幂等

短期可以在开始索引时执行：

```text
invalidateVideo(videoId)
```

更稳妥的长期方案是确定性 ID：

```text
hash(videoId + chunkType + startMs + endMs + contentVersion)
```

并增加：

- `index_version`
- `embedding_model`
- `caption_model`
- `created_at`
- `source_file_size`
- `source_file_mtime`

这样模型或索引策略变更时可以判断旧数据是否需要重建。

---

## 第三优先级：上下文和检索质量

### 任务 9：让 evidence 携带实际图像

当前 `RetrievalResult` 有：

```cpp
QImage keyframeThumb;
```

但检索器没有填充它，`VideoChunk` 也只是保存路径。

建议先实现最小闭环：

1. 检索 top-3；
2. 读取对应 `keyframePath`；
3. 转换成低分辨率图片；
4. 在当前 user message 中追加图片；
5. 每张图片前增加时间标签。

注意限制图片数量和总 payload 大小，避免每个结果携带多张原图。

### 任务 10：对话历史窗口化

在引入完整 `ConversationMemory` 之前，可以先做低风险版本：

- 最近 2 轮保留完整图片；
- 更早图片转成文本占位；
- Tool 结果只保留最近 1~2 个轮次；
- 早期 assistant/user 轮次按字符数或估算 token 数裁剪；
- 超过预算后触发摘要；
- 永远保留当前问题、最近一轮和必要的 tool 调用闭环。

建议先将历史管理从裸 `QJsonArray` 封装到一个内部组件，再决定是否新增独立 `ConversationMemory` 类。

### 任务 11：加固 QA 缓存

当前缓存默认阈值是 `0.88`：

```24:27:d:\Qt\ffmpegProjects\FrameMind\src\service\rag\qa_cache_manager.h
    /// 关键阈值：默认 0.88（较高），只有非常相似的问题才复用
```

但缓存命中没有利用：

- 当前问题的意图；
- 当前检索证据；
- 视频版本；
- 原回答置信度；
- 指代问题特征。

建议分阶段处理：

1. 含“他、她、它、那个、这个、刚才、后来、继续、再详细”等指代词时跳过缓存；
2. 要求原回答置信度达到更高阈值；
3. 缓存记录 `index_version`；
4. 缓存记录 evidence scene IDs；
5. 当前检索场景与缓存场景没有交集时不命中；
6. 将播放器操作、时间定位问题排除在缓存之外。

---

## 第四优先级：采样和模型质量

这部分应在第一到第三优先级完成并建立评估数据后实施。

### 任务 12：建立离线评估集

至少准备以下问题类型：

| 类型 | 示例 |
|---|---|
| 全局摘要 | 视频主要讲了什么 |
| 时间定位 | 红衣服的人什么时候出现 |
| 视觉检索 | 画面中有没有汽车 |
| 语音检索 | 谁提到了某个方案 |
| OCR | 屏幕上的标题是什么 |
| 指代追问 | 他后来做了什么 |
| 多轮承接 | 刚才提到的第二个人是谁 |
| 播放器操作 | 跳转到 01:20 |

每条数据记录：

- 期望时间范围；
- 期望场景；
- 期望文本证据；
- 是否需要图片；
- 是否允许“不确定”。

指标至少包括：

- Recall@1 / Recall@5；
- 时间定位误差；
- 证据覆盖率；
- 答案事实一致性；
- 首 token 延迟；
- 总请求大小；
- 索引耗时；
- 内存峰值。

### 任务 13：优化帧提取

只有当基准确认 `captureFrameAt()` 是主要瓶颈后，再引入独立 `FrameExtractor`：

```text
FrameExtractor
  - open(videoPath)
  - decodeSequential(timestamps)
  - extractThumbnail(timestamp)
  - cancel()
  - close()
```

实现方向：

- 一次打开视频；
- 优先递增顺序解码；
- 避免每帧创建临时 JPEG；
- 输出统一缩放后的 `QImage`；
- 按用途区分缩略图、CLIP 图和 VLM 图。

### 任务 14：从场景级关键帧升级到多帧 chunk

建议不要直接把所有视频改成 1~2 fps 全量索引。可以采用渐进式策略：

```text
粗采样
  -> 场景边界
  -> 每个场景选 3~8 帧
  -> 帧间相似度去重
  -> 代表帧 embedding
  -> 按需 VLM 多帧描述
```

这样可以控制：

- 索引耗时；
- 存储空间；
- VLM 调用数；
- 单视频内存。

### 任务 15：模型替换必须通过 A/B 测试

CLIP 替换为 Chinese-CLIP、SigLIP 或其他多语言模型之前，应比较：

- 中文视觉查询 Recall@K；
- 英文查询 Recall@K；
- 推理速度；
- 模型大小；
- 512 维兼容性；
- 运行内存；
- 现有数据库迁移成本。

数据库应增加模型标识，不能只用向量维度判断向量类型。当前 `VideoRAGStore::search()` 主要依赖向量长度匹配，512 维的不同模型可能被静默混用。

---

# 四、建议的实施顺序

## 阶段 0：先修断链，预计 1 天

目标是让现有索引能力立刻产生实际效果。

1. 将 evidence 传入生成链路；
2. 将 `sceneOverview` 注入 system prompt；
3. 修复 Tool Calling assistant 历史；
4. 修复视频 Agent 停止逻辑；
5. 消除重复流式追加；
6. 增加调试日志，记录：
   - 检索结果数量；
   - 注入 evidence 数量；
   - Prompt 字符数；
   - 图片数量；
   - 历史消息数量；
   - Tool round 数量。

验收标准：

- 日志中能看到 evidence 被注入；
- 模型可以回答基于场景描述的问题；
- 连续两轮问答能看到上一轮 assistant 内容；
- 停止后可以立即发起新问题；
- 回答文本不重复。

---

## 阶段 1：修数据正确性，预计 1~2 天

1. `sampleFrames()` 不再忽略 `videoPath`；
2. 修复 `keyframeMs`；
3. 给关键帧增加落盘路径；
4. 启动重建前清理旧视频索引；
5. 增加索引任务 ID 和视频 ID 校验；
6. 加入索引完成后的 chunk 数量核对。

验收标准：

- 切换视频时不会把帧写入错误视频；
- 重复索引同一视频后 chunk 数量不会增长；
- 重启后可以恢复关键帧；
- 检索结果可以加载对应图片。

---

## 阶段 2：补评估和历史管理，预计 2~3 天

1. 建立最小视频问答测试集；
2. 增加检索 Recall@K 统计；
3. 增加时间定位误差统计；
4. 对话历史窗口化；
5. 加固 QA 缓存；
6. 对 tool 消息序列做合法性检查。

验收标准：

- 能复现每次检索结果；
- 能比较修复前后的 Recall@5；
- 长对话不会无限增长；
- 指代追问不会直接误命中旧缓存；
- provider 不再因 tool 消息顺序报 400。

---

## 阶段 3：优化图像证据和采样，预计 3~5 天

1. 检索结果加载 top-3 关键帧；
2. 关键帧附带时间标签送入 VLM；
3. 引入独立帧提取器；
4. 场景内多帧采样；
5. 评估是否需要 TransNetV2；
6. 对长视频采用分层采样。

验收标准：

- 视觉问题的答案可以引用实际画面；
- 时间定位误差有可量化下降；
- 索引耗时和内存峰值有对比数据；
- 长视频不会因采样任务阻塞用户问答。

---

## 阶段 4：检索质量升级，预计 3~5 天

1. 查询改写；
2. 时间表达式解析；
3. BM25 或 SQLite FTS 稀疏检索；
4. RRF 后 rerank；
5. 同时间段多模态结果合并，而不是简单丢弃；
6. 增加 OCR；
7. ASR 语义段合并；
8. 根据 A/B 测试决定是否替换视觉模型。

---

## 阶段 5：并发隔离，预计 2~4 天

最终将后台分析和用户问答彻底分离：

```text
用户问答 AgentService
  -> 用户 NetworkClient

后台视频分析 AgentService
  -> 后台 NetworkClient
```

同时将当前的全局状态：

- `m_currentConvId`
- `m_accumulated`
- `m_pendingToolCalls`
- `m_pendingFinishReason`
- `m_streaming`

收进每次请求的 `RequestContext`，避免请求之间互相覆盖。

---

# 五、最终建议

当前最值得立即实施的不是换 CLIP、引入 FAISS 或重写 TransNet，而是下面六项：

1. **把 evidence 真正注入 LLM Prompt**
2. **把 `sceneOverview` 注入 system prompt**
3. **补齐 Tool Calling 的 assistant 历史**
4. **修复 VideoAgent 的停止和状态释放**
5. **消除双重流式追加**
6. **修复索引器使用错误视频路径的问题**

这六项完成后，系统会从：

```text
RAG 已检索，但模型看不到
```

变成：

```text
RAG 检索
  -> 结构化 evidence
  -> 场景时间轴
  -> 关键帧
  -> Tool Calling
  -> 多轮历史
  -> 最终回答
```

之后再根据评估数据决定是否投入较大的工作量进行帧提取器、中文视觉模型、OCR、rerank 和多流并发改造。