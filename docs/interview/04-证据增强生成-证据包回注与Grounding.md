# 面试文档四：证据增强生成——证据包回注 VLM 与 Grounding

> 对应简历条目：**"将检索结果中的代表帧、视觉描述、ASR、OCR 和时间戳组装为证据包并回注 VLM，结合 evidence_id 和 Grounding 机制，将回答主张映射到具体视频证据，降低视觉细节丢失和模型幻觉。"**
>
> 涉及代码：`src/service/rag/evidence_composer.cpp/.h`、`src/service/agent/reflection_engine.cpp/.h`、`src/service/rag/qa_cache_manager.cpp/.h`、`src/service/agent/video_agent.cpp`（注入部分）

---

## 一、30 秒电梯话术（背熟）

> 文本 caption 是画面的有损投影——"一个人在街道上行走"回答不了他穿什么颜色、左手拿了什么、招牌写了什么。所以检索命中的代表帧必须回注给 VLM：EvidenceComposer 把命中片段格式化成带时间戳、来源标签、互证信息的文本证据进 prompt，同时把证据关键帧（最多 3 张、缩放到 1280）和用户主动附的帧合并直接进 VLM 的多模态输入——这修复了"检索到了关键帧但最终模型没收到"的断链。回答侧做 Grounding：关键主张映射回 evidence_id 和时间戳；置信度由证据覆盖、模态多样性和跨模态互证共同决定，而不是答案长度；反思发现严重问题时扩大检索、注入反馈重答一次。

---

## 二、理论基础

### 2.1 生成公式：多模态 RAG 与文本 RAG 的本质区别

```
正确:  Answer = VLM(Q, retrieved_text, retrieved_frames)
错误:  Answer = LLM(Q, retrieved_caption)
```

**文本 RAG 检索到的文本就是原始证据本身；视频 RAG 检索到的文本（caption/ASR）只是原始证据（帧）的有损投影。** 所以视频 RAG 在生成阶段必须回注原始模态（帧图像）——这不是"视觉增强"的可选项，是 retrieval-augmented multimodal generation 的标准闭环。

Caption 丢什么（`Caption = g(Frame)`）：
- 小目标、精确颜色、数量、姿态、空间位置；
- 细小文字（OCR 转录也只有 VLM 真正"看到"才可靠）；
- **提问时才显得重要的细节**——caption 生成时不知道用户会问什么。

### 2.2 回答质量的木桶约束

```
AnswerQuality ≤ min(RetrievalRecall, EvidenceFidelity, GenerationQuality)
```

推论（面试金句）：**再强的 Agent 推理也变不出没被索引/召回的信息**。没抽到的帧、错误的场景边界、未识别的文字，多轮反思只会加剧幻觉。所以工程优先级永远是：

```
索引看到了 → 检索召回了 → VLM 拿到原始证据 → 回答可追溯 → 最后才轮到 Agent 花活
```

### 2.3 可靠与不可靠的置信度来源

| 可靠 | 不可靠 |
|---|---|
| 召回/重排分数 | 答案长度 |
| 多模态一致性（互证） | 字符串包含关系 |
| VLM 对关键帧的复核 | 模型自评（自我偏好） |
| 主张 → evidence_id 映射覆盖率 | "听起来很流畅" |

### 2.4 Grounding 的定义

每条回答中的关键主张（时间、数量、人物、文字）都应能映射到：
- `evidence_id`（chunkId）
- 时间范围 `[start_ms, end_ms]`
- 代表帧（keyframePath）
- 证据来源类型（visual / audio / visible_text / fused）
- 互证信息（corroborating_chunk_ids / modalities）

证据不足时应明确说"在当前召回的片段中无法确认"，而不是让模型猜。

---

## 三、证据包的数据流

```
VideoRAGRetriever 检索命中（TopK RetrievalResult）
  │
  ├─ EvidenceComposer::formatText() ──→ 文本证据
  │     "## 证据 1 [chunkId]
  │      时间范围：0:32 - 0:45
  │      来源：文本语义检索；证据类型：visual
  │      同期互证：audio、fused
  │      相关内容：人物从画面左侧走向桌边..."
  │                          │
  │                          ▼
  │              VideoContext.retrievalEvidence（进 system/上下文 prompt）
  │
  ├─ EvidenceComposer::mergeFrames() ──→ 视觉证据
  │     证据 keyframePath 去重加载、缩放 ≤1280、最多 3 张
  │     + 用户主动附的帧（如"当前帧提问"）
  │                          │
  │                          ▼
  │              VLM 请求的 images 数组（多模态输入）
  │
  └─ AgentAnswer.evidence ──→ 回答携带完整证据列表
        （UI 时间戳可点击跳转播放器；toJson 持久化到 checkpoint）
```

---

## 四、代码实现详解

### 4.1 EvidenceComposer::formatText()——文本证据格式化

```cpp
QString EvidenceComposer::formatText(const QVector<RetrievalResult>& evidence,
                                     int maxItems /*=6*/,
                                     int maxCharsPerItem /*=240*/)
{
    for (int i = 0; i < limit; ++i) {
        output += QStringLiteral("## 证据 %1 [%2]\n").arg(i + 1).arg(chunk.chunkId);
        output += QStringLiteral("时间范围：%1 - %2\n")
                      .arg(formatMs(chunk.startMs), formatMs(chunk.endMs));
        output += QStringLiteral("来源：%1；证据类型：%2\n")
                      .arg(hitPathLabel(result.hitPath),   // 视觉检索/文本语义检索/文本精确检索/实体检索
                           evidenceTypeLabel(chunk));      // visual/audio/fused/speech_segment...
        // 跨模态互证信息（检索阶段写入的 metadata）
        if (!corroborating.isEmpty())
            output += QStringLiteral("同期互证：%1\n").arg(corroborating.join("、"));
        if (!temporalHint.isEmpty())
            output += QStringLiteral("时间约束：%1\n").arg(temporalHint);
        output += QStringLiteral("相关内容：%1\n\n")
                      .arg(chunk.textContent.left(maxCharsPerItem));
    }
}
```

**设计细节**：
- **默认 6 条 × 240 字符**：token 预算控制——场景描述动辄几百字，多条直拼会超模型有效注意力；截断保头部（摘要性内容在前）；
- **来源标签**（`hitPathLabel`）：中文可读（"视觉检索""文本语义检索""文本精确检索""实体检索"），让 VLM 知道每条证据从哪来；
- **互证信息透传**：检索阶段算出的 `corroborating_modalities` 直接进证据文本——模型能感知"这段有多个独立模态支持"。

### 4.2 EvidenceComposer::mergeFrames()——帧回注（修复断链的关键）

```cpp
QList<QImage> EvidenceComposer::mergeFrames(const QList<QImage>& userFrames,
                                             const QVector<RetrievalResult>& evidence,
                                             int maxEvidenceFrames /*=3*/,
                                             int maxFrameEdge /*=1280*/)
{
    QList<QImage> frames = userFrames;      // 用户主动附的帧在前
    QSet<QString> seenPaths;                 // canonicalFilePath 去重
    for (const RetrievalResult& result : evidence) {
        if (appended >= maxEvidenceFrames) break;
        const QString path = QFileInfo(result.chunk.keyframePath).canonicalFilePath();
        if (path.isEmpty() || seenPaths.contains(path)) continue;
        seenPaths.insert(path);

        QImage frame(path);                  // 按需从磁盘加载
        if (frame.isNull()) continue;
        if (frame.width() > maxFrameEdge || frame.height() > maxFrameEdge)
            frame = frame.scaled(maxFrameEdge, maxFrameEdge,
                                 Qt::KeepAspectRatio, Qt::SmoothTransformation);
        frames.append(frame);
        ++appended;
    }
    return frames;
}
```

**为什么是 3 张、1280**：每帧图像消耗大量 token；3 张代表帧已覆盖主要状态变化；1280 是复核质量与成本的平衡——证据帧是"给模型看的"不是"给用户播放的"。

### 4.3 序列化：toJson / fromJson

```cpp
// 只持久化回答追溯需要的字段；embedding 不进入 checkpoint，避免无意义膨胀。
object.insert("chunk_id", ...);  object.insert("start_ms", ...);
object.insert("text", ...);      object.insert("keyframe_path", ...);
object.insert("chunk_type", ...); object.insert("metadata", ...);
object.insert("score", ...);     object.insert("hit_path", ...);
```

用于 workflow checkpoint（断点续跑）和对话历史持久化——**回答永远可以还原它当时的证据**，这是 Grounding 的持久化形态。

### 4.4 VideoAgent 的注入点（`phaseReasonAndAct`）

```cpp
VideoContext enrichedCtx = ctx;
enrichedCtx.retrievalEvidence = EvidenceComposer::formatText(m_retrievedEvidence); // 文本证据
enrichedCtx.currentPositionMs  = m_currentPlayerPosMs;
const QList<QImage> evidenceFrames = EvidenceComposer::mergeFrames(
    userFrames, m_retrievedEvidence);                                             // 视觉证据

// 实体档案显式注入（最多 10 个）：
// "## 实体 1: 穿蓝色衬衫的男性 / 类型: 人物 / 别名: ... / 出现次数: 5"
enrichedCtx.entityContext = entityText;

m_orchestrator->runQuery(convId, question, evidenceFrames, enrichedCtx, ...);
```

证据帧 + 用户帧合并后作为 VLM 请求的 images；文本证据 + 实体档案进 VideoContext 组装 prompt。

### 4.5 ReflectionEngine：四项校验 + 重写后的置信度

**四项校验**：

```cpp
ReflectionResult reflect(answer, evidence, repr) {
    checkConsistency(answer, repr);     // 事实一致性
    checkEvidenceSupport(answer, evidence); // 证据支撑
    checkTemporalValidity(answer, repr);    // 时间合理性
    checkHallucination(answer, repr);       // 幻觉检测
}
```

| 检查 | 实现 |
|---|---|
| 一致性 | 例：答案宣称"没有音频"但 metadata.hasAudio=true → 矛盾 |
| 证据支撑 | **>80 字的事实性回答必须有可追溯证据**；简短澄清/拒答允许无证据 |
| 时间合理性 | 正则抽取 `[mm:ss]` 时间戳，校验是否超出视频时长 |
| 幻觉检测 | 声明按标点切 token，在"已知信息池"（摘要+场景描述+语音文本）中查覆盖率；3+ 实词且 >2/3 未命中 → 疑似幻觉 |

**置信度公式（重写后）**：

```cpp
float confidence = evidence.isEmpty() ? 0.35f : 0.65f;   // 基础分：有无证据
confidence += 0.05f * qMin(3, modalities.size());        // 模态多样性（最多+0.15）
if (hasCorroboration) confidence += 0.05f;               // 跨模态互证
confidence -= 0.12f * issues.size();                     // 校验问题惩罚
result.confidence = qBound(0.2f, confidence, 0.95f);
```

代码注释原话（面试可直接引用）："置信度由可追溯证据覆盖、跨模态互证和校验问题共同决定；**不再由答案长度或单纯问题数量决定**。它仍是启发式分数，不能替代人工或 VLM 复核。"——主动承认局限比吹嘘完备更加分。

### 4.6 QACacheManager：对话记忆 RAG

```cpp
// 写缓存的条件（cache()）：
// confidence >= 0.7 且有证据场景，无 embedding 不缓存（不能被检索的东西没有价值）

// 读缓存的条件（tryAnswer()）——五重防护：
1. 相似度 >= m_threshold (0.88)         // 高门槛，只复用"非常相似"的问题
2. evidenceIds 非空                      // 没证据的答案不缓存不复用
3. originalConfidence >= 0.7             // 低置信历史答案不复用
4. modelId == "bge_text" && version == "query_v2"  // 模型身份校验
5. 未过期（m_maxAgeDays 有效期）
```

命中后返回带标记的结果：`[历史分析结论，相似度 92%] ...`——**让用户和上层永远知道这是缓存**，可补充可修正。

**绕过缓存的强制规则**（在 VideoAgent.ask）：
- 播放器操作意图（seek/play/pause）——每次必须真正执行；
- "重新分析/再检查一次"意图——用户显式要求新鲜结果。

---

## 五、真实工程问题（war stories）

### 问题 1：检索命中帧没有进入最终 VLM 输入（最经典的断链，必讲）

**现象**：检索层加载了 `keyframeThumb` 用于 UI 展示，但 `runQuery` 最终只传用户手动截的帧——检索到的关键帧在生成阶段被丢弃。系统实际上退化成了"文本 RAG + 用户提供图片"。
**根因**：检索和生成是两个团队/两个阶段各自实现的，接口上没人负责"把帧带过去"。
**解决**：`EvidenceComposer::mergeFrames` 把证据帧并进 VLM 请求，形成完整闭环。
**方法论金句**：多模态 RAG 的"多模态"必须贯穿到生成阶段，否则只是"给文本 RAG 加了图片入口"。

### 问题 2：反思置信度曾被答案长度和字符串包含定义

**旧版行为**：
- "长答案且没有证据才判证据不足"——短的无证据胡说直接通过；
- "幻觉检测 = 声明文本是否在信息池字符串中出现"——正确的同义改写被判幻觉（"男士"vs"男人"），复述证据的废话被判可信。

**解决**：重写为结构化置信度（基础分 + 模态多样性 + 互证 − 惩罚）；幻觉检测改为 token 覆盖率启发式。
**坦诚的下一步**：启发式只做初筛，真正的 grounding 验证应该是"对关键主张回到候选原始帧/ASR/OCR 做 VLM 复核"——反思引擎的架构位置已留好。

### 问题 3：QA 缓存误命中

**现象**："有几个人"vs"有几个人在说话"相似度 0.9+，命中缓存返回错误答案。
**解决**：五重防护（见 4.6）——高阈值 0.88、必须有证据、原置信度 ≥0.7、模型身份、有效期；外加显式的"历史结论"标记和两类强制绕过。

### 问题 4：证据文本撑爆上下文

**现象**：6 条场景描述直拼超过模型有效注意力，答案质量反而下降（中间信息被忽略）。
**解决**：formatText 限 6 条 × 240 字符；配合 U 形注意力布局（关键信息放开头结尾、对话历史压缩）。

---

## 六、面试官可能问的问题

| 问题 | 回答要点 |
|---|---|
| **多模态 RAG 和文本 RAG 最本质的区别？** | 检索单元从文本 chunk 变成带时间区间的多模态证据包；生成阶段必须回注原始模态（帧）——文本 RAG 检索到的就是原始证据，视频 RAG 检索到的文本只是投影。 |
| **Grounding 具体怎么落地？** | chunk 贯穿始终：检索写互证元数据 → 证据文本带时间戳/来源/互证进 prompt → 回答带 evidence 列表 → toJson 持久化到 checkpoint → UI 时间戳点击跳转播放器。 |
| **置信度怎么算？为什么不用 LLM 自评？** | 结构化启发式（证据覆盖+模态多样性+互证−惩罚），便宜、确定、可解释；LLM 自评有自我偏好且不稳定。承认局限：不能替代帧级 VLM 复核，是明确的下一步。 |
| **证据冲突（视觉说 A、ASR 说 B）怎么办？** | 证据隔离存储保证冲突可见而非被融合掩盖；检索按问题类型加权（画面类压低音频权重）；prompt 要求区分事实与推断；互证元数据标明哪些证据有多模态支持。 |
| **为什么帧限 3 张、1280？** | token 成本：每帧大量 token；3 张覆盖主要状态；1280 平衡复核质量与成本。可按问题类型动态调整（细节类问题给更多帧）是演进方向。 |
| **QA 缓存会不会返回过时答案？** | 视频文件变了 → videoId 变 → 全量失效（含缓存）；有效期 maxAgeDays；置信度 <0.7 的答案从不入缓存。 |
| **怎么评估生成质量？** | 幻觉率（无依据断言占比）、引用准确率（引用的时间戳/证据是否真支撑结论）、时间定位误差、完整性；用标注 QA 对做自动评估流水线。 |
| **如果检索完全没命中？** | 反思判"具体结论但无可追溯证据"；Agent 层触发 seek_and_analyze/analyze_time_range 做局部复核（工具层按需取帧分析）；最终诚实返回"无法确认"而非编造。 |

---

## 七、白板演练：证据包的完整形态

```
检索命中 3 条（场景 5 视觉证据 + 场景 5 语音段 + 场景 5 融合证据）

文本证据（进 prompt）:
┌──────────────────────────────────────────────┐
│ ## 证据 1 [f3a8c2e1-...]                     │
│ 时间范围：0:32 - 0:45                         │
│ 来源：文本语义检索；证据类型：visual          │
│ 同期互证：audio、fused                        │
│ 相关内容：人物从画面左侧走向桌边，手持文件...  │
│                                              │
│ ## 证据 2 [...] 来源：文本精确检索；证据类型： │
│ speech_segment                               │
│ 时间范围：0:33 - 0:41                         │
│ 相关内容："我们接下来打开这个配置文件"         │
│                                              │
│ ## 证据 3 [...] 证据类型：visible_text        │
│ 相关内容：server.port=8080                    │
└──────────────────────────────────────────────┘

视觉证据（进 VLM images）:
  frame_00:33.jpg (1280×720)
  frame_00:39.jpg (1280×720)
  frame_00:44.jpg (1280×720)

VLM 输入 = 用户问题 + 文本证据 + 实体档案 + 3 张原始帧
       ↓
回答："该人物在 [0:33] 走到桌边并说'打开配置文件'，
       屏幕显示端口配置 8080"
       ↓
ReflectionEngine:
  时间戳 [0:33] 合法 ✓ / 有可追溯证据 ✓ / 多模态支持 ✓
  confidence = 0.65 + 0.15(3模态) + 0.05(互证) = 0.85
```
