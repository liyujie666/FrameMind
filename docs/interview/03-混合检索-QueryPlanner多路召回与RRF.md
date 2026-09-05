# 面试文档三：混合检索——QueryPlanner、多路召回与 RRF 融合

> 对应简历条目：**"设计 QueryPlanner 解析视觉、ASR、OCR、时间和实体约束，实现 Dense、BM25/FTS5、Temporal、Entity 多路召回，通过 RRF 融合、Cross-Encoder 重排和帧级复核提升复杂视频问题的检索准确性。"**
>
> 涉及代码：`src/service/rag/video_rag_retriever.cpp/.h`、`src/service/rag/video_rag_store.cpp/.h`、`src/service/rag/query_plan.h`、`src/model/retrieval_result.h`

---

## 一、30 秒电梯话术（背熟）

> 检索不是只做向量搜索。我先用确定性的 QueryPlanner 把问题解析成结构化计划——时间约束（时钟区间、"第3分钟"、"开头/结尾/后半"、"当前"）、视觉/文本/实体意图、证据类型偏好；然后四路并行召回：BGE 文本向量、中文双字 gram 词面召回（承担 BM25 的角色）、CLIP 图文检索、实体档案检索；RRF（k=60，按意图动态加权）融合后，再做时间邻近的跨模态互证加成。一个关键演进：正则只用来调权重，绝不用来关闭任何一路召回——因为"开头那个穿红衣服的人后来讲了什么"这种复合问题，任何单路路由都会丢掉另一半条件。

---

## 二、理论基础

### 2.1 Dense 与 Sparse 的互补性

| | Dense（向量） | Sparse（词面/BM25） |
|---|---|---|
| 擅长 | 语义相似、同义表达、概念检索 | 精确词、专有名词、数字、型号、台词 |
| 公式 | `sim(q,d) = cos(E_q, E_d)` | BM25 词频-逆文档频率打分 |
| 失效场景 | "字幕里有没有'初始化失败'" | "像是在庆祝的画面" |

**视频问答两类需求都大量存在**："他说了什么/字幕里有没有某词"必须稀疏检索；"像是在庆祝的画面在哪"必须向量检索。

### 2.2 RRF（Reciprocal Rank Fusion）

```
score(d) = Σ_i  w_i · 1 / (k + rank_i(d))     k = 60
```

**为什么用 RRF 而不是分数加权融合**（必考）：
- 余弦相似度 ∈ [-1, 1]，BM25 分数 ∈ [0, +∞)，**量纲完全不同**；
- 分数融合必须做归一化，而归一化参数对数据分布敏感、易错；
- RRF **只依赖排名，不依赖原始分数尺度**，天然免疫量纲问题，且几乎免调参；
- k=60 是平滑常数，抑制头部排名的分数支配（rank 1 和 rank 2 差距不至于过大）。

**重要认知（面试金句）**：**RRF 只能融合已有的召回，不能弥补缺失的召回源**。没有稀疏路、时间路，调 RRF 参数是徒劳的——重点永远是补齐召回路径。

### 2.3 查询意图是复合的（本模块最重要的演进故事）

用户问题的真实意图可拆为：

```
I(Q) = (q_visual, q_asr, q_ocr, t_time, e_entity, y_answer_type)
```

例："开头那个穿红衣服的人后来讲了什么"同时包含：
- 视觉属性：红衣服
- 时间约束：开头、后来
- 实体指代：同一个人
- 音频需求：说了什么

**任何"二选一"的正则路由都会丢掉另一半条件。** 正确做法：解析为结构化计划，分别召回并融合。

### 2.4 向量空间由模型身份定义，而非维度

```
v_clip ∈ R^512,  v_bge ∈ R^512   →   不能计算 cos(v_clip, v_bge)
```

维度相同只说明数组长度相同，不说明每一维语义一致。所以 embedding 必须绑定 `model_id + version + modality + index_version`，视觉/文本/OCR 向量分 collection 存储。

---

## 三、检索全流程（retrieve() 一图流）

```
retrieve(query, constraints, topK)
  │
  ├─ ① compileQueryPlan(query, constraints)   确定性查询计划
  │     ├─ 显式 Constraints 优先（Tool 给的时间边界不被问题文本覆盖）
  │     ├─ 解析时钟区间/时钟点/第X分Y秒/开头/结尾/前半后半/当前
  │     └─ 时间约束超视频时长 → temporalConstraintUnsatisfiable → 直接返回空
  │
  ├─ ② analyzeQuery(normalizedQuery)          意图分析（只调权重）
  │     ├─ needsTextSearch / needsVisualSearch 恒为 true
  │     └─ 证据类型偏好: prefersVisual/Audio/FusedEvidence
  │
  ├─ ③ 四路召回（每路 topK × candidateMultiplier 超额召回）
  │     ├─ text_dense  : BGE → text_segments 语义检索
  │     ├─ text_lexical: 中文双字 gram 词面检索（权重 ×0.85）
  │     ├─ visual      : CLIP text encoder → visual_frames 图文检索
  │     └─ entity      : 实体描述 BGE → entity_profiles
  │     每路结果 applyEvidencePreference（证据类型乘子）+ 时间约束过滤
  │
  ├─ ④ reciprocalRankFusion(perPath, weights, k=60)
  │
  ├─ ⑤ deduplicate（同证据类型 + 时间重叠>70% 才算重复）
  │
  ├─ ⑥ applyTemporalCorroboration（跨模态互证加成）
  │
  └─ ⑦ resize(topK) 返回
```

---

## 四、代码实现详解

### 4.1 QueryPlanner：`compileQueryPlan()`

**优先级设计**（面试必讲）：

```cpp
// Tool 或调用方只要提供任一时间边界都优先，
// 不能被问题文本中的时间词覆盖。
if (constraints.startMsGte >= 0 || constraints.endMsLte >= 0) {
    plan.startMs = constraints.startMsGte;
    plan.endMs   = constraints.endMsLte;
    plan.temporalHint = "explicit_range";
    return plan;
}
```

为什么：Agent 调 `search_video_content(time_range=[...])` 时给了明确边界，如果问题文本里又出现"开头"之类的词，后者会把 Tool 的精确约束改掉。

**时间表达解析（全正则、确定性、零成本）**：

| 表达 | 解析结果 |
|---|---|
| `00:30到01:20` / `00:30-01:20` | 时钟区间，start/end |
| `01:23`（时钟点） | ±5 秒窗口 |
| `第3分钟` / `第3分20秒` | minute_second 点 + 窗口 |
| `开头` / `一开始` | [0, min(15s, duration)] |
| `结尾` / `最后` | [duration-15s, duration] |
| `前半` / `后半` | 按时长对半 |
| `当前` / `现在` / `这里` | 播放位置 ±10s（需要 currentPositionMs） |

**合法性校验**：时间点超出视频时长 → `temporalConstraintUnsatisfiable = true`，`retrieve()` 开头直接返回空——**不去检索注定无结果的范围**，同时给上层一个明确信号（这比返回空列表再让 LLM 猜要诚实）。

### 4.2 意图分析：`analyzeQuery()`——关键词只调权重，永不关路径

```cpp
// 视觉与文本默认并行召回；关键词只用于调权，
// 不能再把复合问题错误压缩为单路。
intent.needsVisualSearch = true;
intent.needsTextSearch   = true;

if (visualCueMatched && !textCueMatched) {
    intent.weightVisual = 0.6; intent.weightText = 0.3; intent.weightEntity = 0.1;
} else if (!visualCueMatched && textCueMatched) {
    intent.weightVisual = 0.3; intent.weightText = 0.6; intent.weightEntity = 0.1;
} else {
    intent.weightVisual = 0.45; intent.weightText = 0.45; intent.weightEntity = 0.1;
}
```

**证据类型偏好**（作用于单路分数的乘子，在 RRF 之前）：

```cpp
float QueryIntent::evidenceWeight(VideoChunk::ChunkType t) const {
    switch (t) {
    case SceneSummary:   // 纯视觉证据
        if (prefersVisualEvidence) return 1.15f;
        if (prefersAudioEvidence)  return 0.75f;
        return 1.0f;
    case SpeechSegment:  // 原始台词——"谁说了什么"最直接
        if (prefersAudioEvidence)  return 1.20f;
        if (prefersVisualEvidence) return 0.70f;
        return 1.0f;
    case SceneFused:     // 融合证据——叙事类问题默认首选
        if (prefersFusedEvidence)  return 1.10f;
        return 1.0f;
    ...
}
```

规则来源（面试讲故事）：`visualOnlyCues`（穿/颜色/几个人/手里拿…）命中 → 画面细节类问题压低音频证据；`audioOnlyCues`（说了什么/台词/字幕…）命中 → 台词类压低纯视觉证据；`narrativeCues`（发生了什么/为什么…）→ 优先融合证据；**两侧同时命中则都不压制**，交给融合证据裁决。

### 4.3 四路召回实现

```cpp
// Path A: text_dense —— BGE 编码 query → text_segments
const auto emb = m_embedder->embedQuery(query);
VideoRAGStore::Filter f;
f.expectedEmbeddingModelId = "bge_text";     // 模型身份硬校验
f.expectedEmbeddingVersion = "passage_v2";
if (c.startMsGte >= 0 || c.endMsLte >= 0) {
    f.timeMatchMode = VideoRAGStore::Filter::Overlaps;  // 区间重叠
}
const auto results = m_store->search(VideoRAGStore::TextSegments, emb, f, topK);
// 命中结果顺带加载 keyframeThumb（320×180 缩略图）

// Path A2: text_lexical —— 中文双字 gram + ASCII 词
const auto results = m_store->searchLexical(VideoRAGStore::TextSegments, query, filter, topK);

// Path B: visual —— CLIP text encoder → visual_frames
const auto emb = m_clip->encodeText(query);
f.expectedEmbeddingModelId = "clip_visual";

// Path C: entity —— 实体描述 BGE → entity_profiles
const auto results = m_store->search(VideoRAGStore::EntityProfiles, emb, f, topK);
```

**关键设计**：
- **检索前过滤**（不是检索后）：时间约束在 Filter 里做，避免 Top-K 全被时间外结果占满；
- `Overlaps` 模式：chunk 与查询区间有任意正向重叠即保留（容忍 ASR 分段边界抖动），区别于旧的 `FullyContained`（chunk 完整落在范围内）；
- 每路 `topK × candidateMultiplier` **超额召回**，给 RRF 融合和后续去重留余量。

### 4.4 RRF 融合实现

```cpp
QVector<RetrievalResult> VideoRAGRetriever::reciprocalRankFusion(
    const QMap<QString, QVector<RetrievalResult>>& perPath,
    const QMap<QString, double>& weights, int k)
{
    QHash<QString, RetrievalResult> merged;   // chunkId → 结果
    QHash<QString, double> scores;            // chunkId → RRF 累计分

    for (auto it = perPath.constBegin(); it != perPath.constEnd(); ++it) {
        const double w = weights.value(it.key(), 1.0);
        for (int rank = 0; rank < list.size(); ++rank) {
            const double addScore = w * (1.0 / (k + rank + 1));
            scores[id] += addScore;
            if (!merged.contains(id)) merged.insert(id, res);
        }
    }
    // RRF 融合分数覆写 score，排序返回
}
```

权重来源：意图分析的 weightText/weightVisual/weightEntity；`preferPath`（Agent 按问题类型指定，如 EntityQuery → entity）额外 ×1.25；lexical 路固定 ×0.85（辅助定位，避免词面命中压制语义命中）。

### 4.5 去重：只在"同类型 + 时间重叠"时去重

```cpp
// 同一场景的视觉/音频/融合证据时间范围完全相同但内容互补，
// 只在"同一时间段 + 同一证据类型"时才认为重复
if (r.chunk.chunkType != kept.chunk.chunkType) continue;  // 类型不同绝不去重
if (timeOverlapRatio(r.chunk, kept.chunk) > 0.7f) { overlap = true; break; }
```

### 4.6 跨模态互证：`applyTemporalCorroboration()`

**思想**：一条证据如果附近（<4s）有**不同模态**的证据也命中，说明多个独立信息源都支持这个时间段——应该加分，并把互证关系写进 metadata 供生成和反思阶段使用。

```cpp
constexpr int64_t kNeighborWindowMs = 4000;
for (每个候选) {
    for (其他命中 other) {
        if (timeGapMs(candidate, other) > kNeighborWindowMs) continue;
        // 排除1: 候选本身是 independent/unknown 音频 → 不参与互证
        // 排除2: 两者同源（都来自 whisper 转写）→ 不算跨模态
        if (isDerivedFromSameTranscript(candidate.chunk, other.chunk)) continue;
        if (modality == primaryModality) continue;      // 同模态不算
        supportingIds.insert(other.chunk.chunkId);
        supportingModalities.insert(modality);
    }
    if (!supportingIds.isEmpty()) {
        const int supportCount = qMin(3, supportingIds.size());
        candidate.score *= 1.0f + 0.10f * supportCount;  // 最多 +30%
        // metadata 写入 corroborating_chunk_ids / corroborating_modalities
    }
}
```

**两个排除规则的来历**（war story，见第五节）：一个来自"独立音频污染"，一个来自"同源转写伪互证"。

### 4.7 存储层：`VideoRAGStore`

**四个 Collection**：

```
visual_frames    帧级 CLIP embedding
text_segments    场景描述/语音转写/事件 的文本 embedding
entity_profiles  实体档案语义 embedding
qa_cache         历史问答 embedding（复用规避重复分析）
```

**模型身份校验**（search 内部）：

```cpp
// 向量空间由模型 ID/版本而非维度定义；未声明身份的旧 chunk 只在
// 调用方未要求模型隔离时兼容检索，升级后的路径不会静默混用它们。
const QString modelId = c.metadata.value("embedding_model_id").toString();
if (!filter.expectedEmbeddingModelId.isEmpty()
    && modelId != filter.expectedEmbeddingModelId) continue;   // 直接跳过
```

**词面召回**（`searchLexical`）：中文双字 gram + ASCII 词，与 dense 向量检索互补——SQLite FTS5 对中文分词不友好，自研轻量实现。

**存储形态**：内存 `vector<(chunk, embedding)>` 暴力余弦 + SQLite 持久化（`rag_chunks / rag_entities` 表）+ 按视频惰性 `loadFromDb`。接口按 FAISS IndexFlatIP 的 API 设计，规模上来可无缝替换。**所有 public 方法互斥锁保护**，插入/检索可跨线程调用。

---

## 五、真实工程问题（war stories，最有说服力的部分）

### 问题 1：正则分流把复合问题压缩成单路（必讲）

**旧版行为**：视觉正则命中 → 只走视觉检索；文本正则命中 → 只走文本检索。
**翻车案例**："开头那个穿红衣服的人说了什么"被分成视觉问题 → "说了什么"的 ASR 召回直接丢失 → 答非所问。
**三步修复**：
1. 所有路径默认全开（`needsTextSearch/needsVisualSearch` 恒 true）；
2. 关键词只调 RRF 权重（0.6/0.3 vs 0.45/0.45）；
3. 证据类型偏好做**乘子**而非硬过滤（0.70~1.20）。

**方法论金句**：路由做"加法"（并行召回+融合），不做"减法"（裁剪路径）。分类器可以错，但错了不能致命。

### 问题 2：缺稀疏路，RRF 无米下锅

**现象**："字幕里有没有'初始化失败'"这类精确词面问题，向量召回率极低——语义向量对"初始化失败"和"启动异常"分不开，但用户要的是字面。
**解决**：实现中文双字 gram 词面召回作为第四路。为什么不直接用 SQLite FTS5：中文没分词器支持（FTS5 默认 unicode61 对中文按字符切，效果差），双字 gram 自实现零依赖且效果可控。

### 问题 3：去重误杀跨模态证据

**现象**：早期去重规则是"时间重叠 >70% 即重复"——同一场景的视觉证据和音频证据时间完全相同，互相挤掉，证据多样性骤降。
**解决**：双条件去重（同 chunkType + 时间重叠）。**去重的目的不是省 token，是去掉冗余；跨模态证据不是冗余，是互补。**

### 问题 4：同源证据伪互证

**现象**：whisper 转写被切成段 A、段 B，时间邻近，互证逻辑给它们互相加分——"音频+音频"被当成了跨模态一致，单模态错误被放大成"多源支持"。
**解决**：`isDerivedFromSameTranscript()` 检查 metadata 的 `source` 字段，同源转写排除。配套规则：`independent/unknown` 关系的音频也不参与互证（背景音乐/旁白与画面无因果）。

### 问题 5：时间约束的"检索前 vs 检索后"

**现象**：先向量检索 Top-K 再过滤时间，长视频里 Top-K 可能全是时间外的结果，过滤完为空。
**解决**：时间约束进 Filter，检索时逐条跳过（内存暴力检索天然支持，这是不用 ANN 的一个隐性好处）。

---

## 六、面试官可能问的问题

| 问题 | 回答要点 |
|---|---|
| **RRF 的 k=60 是什么？** | 平滑常数，抑制头部排名分数支配；来自原论文经验值。为什么不用分数融合：cos 和 BM25 量纲不同，归一化参数对分布敏感；RRF 只用排名，鲁棒免调参。 |
| **为什么没上 Cross-Encoder？** | 当前互证逻辑（跨模态时间邻近加成）起轻量 rerank 作用且零推理成本；Cross-Encoder 是预留下一档：RRF Top-30 → 精排 Top-10，管线位置已留好。简历表述为架构能力，面试坦诚当前实现深度。 |
| **为什么不用 FAISS？** | 单机单视频规模：10 分钟视频约 100~200 chunk、1~3MB 向量，内存暴力检索毫秒级。FAISS 是引入外部依赖换不来的收益。但接口按 IndexFlatIP 设计，规模上来直接换。 |
| **QueryPlanner 为什么不用 LLM？** | 分层策略：时间表达、指代词用确定性规则（快、免费、可测试、无幻觉）；复杂复合意图才值得花一次 LLM 调用。规则先行是成本和确定性的正确默认。 |
| **时间约束怎么进向量检索？** | 检索前过滤（Filter + Overlaps 区间重叠模式），不是检索后——避免 Top-K 被时间外结果占满。 |
| **多路召回的每路取多少？** | `topK × candidateMultiplier` 超额召回，给融合和去重留余量；最终 resize(topK)。 |
| **权重 0.6/0.3 怎么定的？** | 启发式初始值 + case 验证调整；本质是"有明确单一模态信号时偏向它，但绝不为零"。可以坦诚这是经验参数，有评测集后可学习。 |
| **检索质量怎么评估？** | Recall@K / MRR / nDCG / 时间定位误差（标注时间戳 vs 命中区间）；台词类问题单独测 lexical 路贡献。 |
| **如果一个 chunk 同时被四路命中？** | RRF 累加四路分数，天然排最前——这正是 RRF 想要的："多个独立证据源都支持"的强信号，和互证加成逻辑一致。 |

---

## 七、白板演练：一个复合问题的完整检索

```
问题："开头那个穿红衣服的人后来讲了什么？"（10 分钟视频）

① compileQueryPlan:
   命中"开头" → startMs=0, endMs=15000, temporalHint="beginning"
   无显式 Constraints → 文本解析生效

② analyzeQuery:
   visualCues("红|衣服") 命中, textCues("讲") 命中
   → weightVisual=0.45, weightText=0.45, weightEntity=0.1
   audioOnlyCues("讲了什么") 命中 → prefersAudioEvidence=true
   → SpeechSegment ×1.20, SceneSummary ×0.75

③ 四路召回（时间约束 [0,15s] + Overlaps）:
   text_dense  → 场景3描述"红衣男子进入..."
   text_lexical→ "红衣服"字面命中
   visual      → CLIP("穿红衣服的人") → 帧 00:07 相似
   entity      → 实体档案"红衣男子"

④ RRF(k=60) → 场景3 融合分最高（多路命中）

⑤ 去重: 同类型+重叠>70% 才删 → 保留视觉/音频/融合三条

⑥ 互证: [00:05-00:12] 视觉与语音段 [00:06-00:11] 邻近且模态不同
   → +10%，metadata 记录 corroborating_modalities=[visual, audio]

⑦ TopK 返回 → 进入 EvidenceComposer（文档四）
```
