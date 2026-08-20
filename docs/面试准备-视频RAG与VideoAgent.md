# 面试准备文档：多模态视频 RAG 与 Video Agent

> **分册深度版**（每个点一份独立详细文档，含完整代码讲解与面试问答）：
> - [`interview/01-时序索引-四层体系与渐进式索引.md`](./interview/01-时序索引-四层体系与渐进式索引.md)
> - [`interview/02-多模态分析-入库Pipeline与音视频融合.md`](./interview/02-多模态分析-入库Pipeline与音视频融合.md)
> - [`interview/03-混合检索-QueryPlanner多路召回与RRF.md`](./interview/03-混合检索-QueryPlanner多路召回与RRF.md)
> - [`interview/04-证据增强生成-证据包回注与Grounding.md`](./interview/04-证据增强生成-证据包回注与Grounding.md)
> - [`interview/05-VideoAgent-五阶段决策循环与ToolCalling.md`](./interview/05-VideoAgent-五阶段决策循环与ToolCalling.md)
>
> 本文档作为总览与速查。对应简历 5 个核心成果点的深度展开。每个点包含：30 秒电梯话术 → 理论基础 → 架构与代码实现 → 真实工程问题 → 面试官可能的问题与回答要点。
> 所有代码细节均来自本项目真实实现（`src/service/rag/`、`src/service/agent/`），可直接在面试中引用类名和文件名。

---

## 0. 项目一句话与全局架构（开场必背）

**30 秒版本：**

> 这是一个 C++/Qt 的视频理解系统。核心思路是：不把视频当成图片集合，而是先把视频按时间分层索引成场景、代表帧、ASR 语音段、可见文字和实体档案等多模态证据；用户提问时，系统先把问题解析成结构化检索计划，再走 Dense + 词面 + 视觉 + 实体多路召回，用 RRF 融合后把命中片段的代表帧、ASR、OCR 一起组装成证据包回注 VLM 生成答案；Agent 层只负责规划、工具调用和反思校验，索引和证据才是事实来源。

**架构分层（画图题必备）：**

```
┌─────────────────── UI / ViewModel ───────────────────┐
│                       VideoAgent（顶层协调器）          │
│   PERCEIVE → REPRESENT → REASON+ACT → REFLECT        │
├──────────────┬──────────────┬────────────────────────┤
│ Perception-  │ VideoRAG-    │ ToolOrchestrator       │
│ Strategy     │ Retriever    │  ├ ToolRegistry        │
│ 问题分类/采样  │ 多路召回+RRF  │  └ 6 Tools             │
├──────────────┴──────────────┴────────────────────────┤
│ VideoAnalysisService / VideoIndexer（L0/L1/L2 渐进索引）│
│   FrameExtractor(FFmpeg) / SceneDetector /            │
│   AudioVisualAligner / CLIP / BGE / Whisper           │
├───────────────────────────────────────────────────────┤
│ VideoRAGStore（SQLite + 内存向量，4 个 Collection）      │
│   visual_frames │ text_segments │ entity_profiles │ qa_cache │
└───────────────────────────────────────────────────────┘
```

**数据流（一句话）：**
视频打开 → 后台线程建 L0（元信息+场景切分）/L1（代表帧 CLIP 向量 + Whisper 转写）→ 提问时按需触发 L2（VLM 场景描述 + 音视频融合）→ 检索 → 证据包 → VLM 回答 → 反思校验。

**常见追问"为什么不用现成的 LangChain/LlamaIndex"：**
本项目是 C++/Qt 桌面端，追求本地小模型（ONNX Runtime 推理 CLIP/BGE/Whisper，零网络延迟）+ 云端大模型（VLM/LLM）协作的架构，整套 RAG 管线（存储、检索、融合、编排）是从零实现的，因此对 RAG 每一层的取舍都有一手工程经验。

---

## 1. 时序索引：Video → Event/Clip → Shot → Frame 四层体系

### 1.1 电梯话术（30 秒）

> 视频是时序信号不是图片集合，单帧索引无法表达动作变化和事件顺序。我把视频组织成分层索引：全局层做摘要，事件层做 5~30 秒语义片段，场景（Shot）层做镜头切分并保留多张代表帧，帧层做精确定位和 grounding。索引进度按 L0/L1/L2 渐进构建，用户不用等全量分析完就能开始提问。

### 1.2 理论基础（为什么必须分层）

- 视频建模为多模态时序数据 `V = {(f_t, a_t, o_t)}`，画面、音频、OCR 三路观测随时间变化。
- **数据粒度必须和问题粒度匹配**：
  - "视频讲了什么" → Video 层全局摘要
  - "这一段发生了什么" → Event/Clip 层
  - "镜头什么时候切的" → Shot 层
  - "画面右边有几个人 / 招牌写了什么" → Frame 层
- 只保留"场景首帧"在信息论上是有损压缩：一个几十秒的片段被压成一个静态观测，"后来发生了什么""谁进入画面""字幕什么时候变"这类问题在索引阶段信息就不可逆丢失了。
- 分层的本质是**分层检索**：粗粒度先召回，细粒度再局部验证，避免全视频逐帧问大模型，也避免单帧丢时序。

### 1.3 代码实现（可以报出具体细节）

**核心类：`VideoIndexer`（`src/service/agent/video_indexer.cpp`）+ `VideoRepresentation`（三层表示）**

- **渐进式三级索引**：
  - **L0（秒级）**：元信息（时长/分辨率/fps）+ 场景分割。有 TransNetV2 时按 1fps 密集采样（上限 600 帧），无模型时回退直方图差异、每 10s 稀疏采样（上限 200 帧）。
  - **L1（后台异步）**：代表帧 CLIP 向量批量编码 + Whisper 转写 + BGE 文本向量，写入 RAG Store。
  - **L2（按需触发）**：用户提问或检索命中时才对该场景调 VLM 生成描述和做音视频融合——"按需深化"避免浪费 VLM 调用。
- **多代表帧**：`assignRepresentativeFrames()` 给每个场景分配**首帧、中间帧、尾帧**最多 3 张代表帧（`Scene::representativeFrames`），彻底替代"场景只留首帧"。
- **videoId 设计**：`SHA1(文件大小 + 前 1MB 内容)`，文件未变则索引缓存命中直接复用（`startIndex` 里 Level1 以上缓存直接 `emit indexCompleted`），文件变了则 `invalidateVideo` 全量重建。
- **关键帧落盘**：代表帧持久化到 `AppData/keyframes/<videoId>/`，检索命中时按需加载缩略图，避免内存常驻大图。
- **线程与取消**：索引跑在 `QThreadPool`（2 线程）；取消用 `task generation 计数器 + atomic_bool` 双保险——新任务发起时 generation+1，旧任务在每阶段检查 `isTaskCurrent()` 自行退出，避免粗暴 terminate。

**索引的产物是带元数据的 Chunk（`VideoChunk`，`src/model/retrieval_result.h`）：**
每个 chunk 携带 `chunkId / startMs / endMs / chunkType / textContent / textEmbedding / frameEmbedding / keyframePath / metadata(evidence_type, scene_id, embedding_model_id...)`，chunkType 区分 `SceneSummary（纯视觉）/ SceneAudio（音频摘要）/ SpeechSegment（原始台词）/ SceneFused（融合）/ Event / 可见文字` 等——这就是"粒度分层"在数据结构上的落地。

### 1.4 真实工程问题（重点讲这个，最有说服力）

**问题 1：取帧性能是索引的根因瓶颈**
最初抽帧走播放器 SDK 的 `extractThumbnail()`：每抽一帧都要 重新打开文件 → 初始化解码器 → seek → 解码 → 写临时 JPEG → 读回 → 删除。抽几百帧时 I/O 和解码器初始化开销完全不可接受，而且索引服务反向依赖了播放器状态。
**解决**：写了独立的 `FrameExtractor`（`frame_extractor.cpp`），基于 FFmpeg `avformat/avcodec/swscale`：
- 一个视频只打开一次解码器，目标时间戳**排序去重后单次顺序解码**，解码到某个 PTS 就顺序消费所有 ≤PTS 的目标帧（`consumeFrame` 里的 `while (targetIndex < targets.size() && ptsMs >= targets.at(targetIndex))`）；
- `sws_scale` 直接缩放输出 RGB24，不走 JPEG 临时文件；
- 注册 `interrupt_callback` 支持取消打断阻塞 I/O；
- 所有 FFmpeg 资源用 RAII deleter 包装，杜绝泄漏；
- 时间戳用 `best_effort_timestamp` + `av_rescale_q` 换算到毫秒，并区分 `requestedMs`（请求时间）和 `ptsMs`（实际帧时间），避免时间漂移。
**面试金句**：先重构取帧再谈换模型——索引链路的性能上限由最底层的数据获取决定。

**问题 2：B 帧导致的时间戳乱序**
视频有 B 帧时 packet 顺序 ≠ 显示顺序，早期按 packet 到达顺序记时间戳导致场景边界错位。改用 `best_effort_timestamp`（解码器输出的显示时间）解决。

**问题 3：用户切换视频时索引线程的竞态**
用户快速连续打开多个视频，旧索引任务还在跑。用"任务代际号"（每次 `startIndex` 自增 generation，任务各阶段校验 generation + videoId）保证旧任务静默退出，新任务不被旧任务的中间写入污染；同时 RAG Store 层面在新索引开始时 `invalidateVideo(videoId)` 清旧数据。

### 1.5 面试官可能问的问题

| 问题 | 回答要点 |
|---|---|
| 为什么是四层而不是直接固定时长切分（每 10s 一段）？ | 固定切分会把一个语义事件腰斩成两个 chunk，也可能让一个静态画面占满一个 chunk。分层让边界来自内容（镜头检测、ASR 语义段），chunk 才能独立提供完整语义。本项目 chunk 切分就是"场景驱动为主，语音句子级为辅"。 |
| Shot 和 Event 的区别？ | Shot 是物理边界（镜头切换检测出来），Event 是语义单元（按动作/ASR 语义合并，5~30s）。一个 30s 的长镜头里可能有"进门→坐下→拿杯子"三个事件，所以 Shot 不能承担全部语义。 |
| 索引一个 10 分钟视频要多久？怎么优化？ | L0 秒级（稀疏采样直方图切分）；L1 主要耗时在 CLIP 批量编码和 Whisper，用后台线程不阻塞交互；L2 完全按需。优化手段：代表帧数量上限、批量推理、TransNetV2 密集采样仅在可用时启用、索引持久化后二次打开秒级命中。 |
| 关键帧怎么选的？为什么是首/中/尾三帧？ | 首中尾是成本最低的时序覆盖——能表达"开始什么样、过程什么样、结束什么样"，覆盖状态变化方向。更精细的策略（运动剧烈处/OCR 变化处加密）是明确的演进方向，我可以说出方案但当前版本选了最稳的实现。 |
| videoId 用整个文件的 hash 吗？ | 不是，用 SHA1(文件大小 + 前 1MB)。全量 hash 对大视频太慢，前 1MB + 大小在实践中足够区分编辑过的文件。 |

---

## 2. 多模态分析：入库 Pipeline 与带时间戳的多帧网格 VLM

### 2.1 电梯话术（30 秒）

> 入库 Pipeline 包含批量取帧、镜头切分、Whisper 转写、代表帧 CLIP 编码和 VLM 场景描述。VLM 分析不是单帧调用，而是把一个场景的多张带时间戳的代表帧拼成一次调用，让模型同时看到前中后状态。同时我坚持多模态证据隔离：视觉描述、音频摘要、原始台词、融合推论、可见文字各自独立成 chunk 入库，绝不互相覆盖，每条都带模态标签和模型身份。

### 2.2 理论基础

- **多帧网格**：把时间序列编码为空间布局 `[f_t1...f_tk] ⇒ Temporal Grid`。逐帧调用 VLM 是 O(n) 成本且每次只见局部；单帧是 O(1) 但丢时序；多帧一次调用近似 O(1) 且覆盖多个时间状态——是普通 VLM（非时序模型）下成本/覆盖/实现难度的最佳折中。
- **模态不等价**：视觉、ASR、OCR 描述的是不同事实。ASR 说"这是小王"不能证明画面里的人就是小王；字幕可能是画外旁白。证据模型应为 `E = {E_v, E_a, E_o, E_f}`（视觉事实/音频事实/OCR 事实/融合推论），检索和回答阶段才能区分"看到了什么 / 说了什么 / 模型推断了什么"。
- **Caption 是有损投影**：`Caption = g(Frame)`，只保留模型当时认为值得说的内容，小目标、颜色数量、细小文字在索引期就丢了——所以原始帧必须在生成阶段回注（呼应第 4 点）。

### 2.3 代码实现

**入库 Pipeline（`video_indexer.cpp` L1 + `video_analysis_service.cpp` L2）：**

1. **L1 视觉编码**：收集所有场景代表帧 → `ClipService::encodeImages()` **批量**编码（一次 ONNX 推理多帧，不逐帧调用）→ 向量挂回对应 `(sceneIndex, frameIndex)`。
2. **L1 语音**：Whisper 全量离线转写 → `SpeechSegmenter` 按语义切分段（带 start/end 时间戳）→ BGE 编码文本向量。
3. **L2 场景描述（多帧）**：`VideoAnalysisService::describeScene` 把场景的多张代表帧（带时间戳）+ 同期 ASR 文本一起送 VLM，输出结构化描述（摘要/实体/动作/可见文字/不确定项）。
4. **音视频融合（`audio_visual_aligner.cpp`）**：**同期 ASR 语义对齐 + 语义门控 + 保守融合**——只有当音频语义与画面内容判定相关（`AudioVisualRelation`）才融合；无法判定关系时保守地不融合，宁可少融合也不错融合。
5. **三类证据分离入库（`video_analysis_service.cpp` 的 `writeSceneEvidence`）**：
   - 纯视觉描述 → `SceneSummary`，`evidence_type=visual`
   - 音频摘要 → `SceneAudio`，`evidence_type=audio`
   - 可见文字（字幕/招牌/PPT，来自 VLM 转录）→ 独立 chunk，`evidence_type=visible_text`
   - 融合描述 → `SceneFused`，且**只在融合结果 ≠ 视觉描述时才写**（无音频时不重复占 chunk）
   - 融合描述永远不覆盖纯视觉事实。
6. **模型身份绑定**：每条 chunk 的 metadata 写 `embedding_model_id`（如 `bge_text` / `clip_visual`）和 `embedding_version`（如 `passage_v2`），检索时 Filter 校验（详见第 3 点）。

### 2.4 真实工程问题

**问题 1：融合描述污染纯视觉事实**
早期把 VLM 描述和 ASR 内容直接拼成一条"场景描述"。后果：台词里的信息（"这是小王"）被当成了画面事实，"画面里有什么"的纯视觉问题被音频内容误导。
**解决**：拆成 visual / audio / fused 三条独立 chunk，各自带 `evidence_type` 元数据，检索阶段按问题类型对证据类型加权（详见第 3 点的 `evidenceWeight`）。

**问题 2：音视频盲目融合的"张冠李戴"**
同期的 ASR 未必和画面相关（背景音乐、旁白、画面外声音）。做了**语义门控 + 保守融合**：只有判定相关才融合，Unknown/Independent 关系保持分离，并在 metadata 里记录 `audio_relation`，检索的跨模态互证还会**排除同源转写**（`isDerivedFromSameTranscript`，两条都来自 whisper 的证据不算互证）。

**问题 3：VLM oneShot 通道与用户对话互相干扰**
场景描述生成初期复用了用户对话的 AgentService 实例和会话状态，索引期间用户发消息会把系统 prompt 和场景分析的上下文串在一起。解决方向是独立通道/独立会话隔离（面试可坦诚说这是踩坑后明确的改造项，体现对状态隔离的敏感度）。

**问题 4：中文图文检索模型选择**
OpenAI CLIP 中文对齐差，中文 query 的"以文搜图"召回质量低。选型结论：视觉图文对齐用中文对齐模型（Chinese-CLIP / 多语 SigLIP），BGE 只做纯文本语义检索——两者语义空间不同，不能互相替代。

### 2.5 面试官可能问的问题

| 问题 | 回答要点 |
|---|---|
| 多帧网格和真视频时序模型（VideoLLaMA 类）的差距？ | 网格不是严格时序建模，帧间精确运动、快速动作仍可能丢失。但普通 VLM 上它是成本和覆盖的最优折中；如果上时序模型，取帧和分层索引这套底座不变，只换 L2 的分析器。 |
| 为什么 OCR 单独一路而不是混在视觉描述里？ | 错误模式不同：OCR 是精确文本（标题、型号、数字），视觉描述是语义概览。"招牌上写了什么"这类问题需要词面精确匹配（配合第 3 点的 lexical 召回），混进 caption 里基本检索不到。 |
| Whisper 转写的 timestamp 误差怎么处理？ | 用语义分段而不是逐句独立检索；检索约束用 Overlaps（区间重叠）而不是精确包含，容忍边界抖动。 |
| VLM 描述质量不稳定怎么办？ | 结构化 prompt 约束输出（要求区分"确定看到的"和"推测的"）；不确定项显式输出；反思阶段把无证据支撑的断言标为疑似幻觉。 |

---

## 3. 混合检索：QueryPlanner + 多路召回 + RRF 融合

### 3.1 电梯话术（30 秒）

> 检索不是只做向量搜索。我先用确定性的 QueryPlanner 把问题解析成结构化计划——时间约束（时钟区间、"第3分钟"、"开头/结尾/后半"、"当前"）、视觉/文本/实体意图、证据类型偏好；然后四路并行召回：BGE 文本向量、中文双字 gram 词面召回（对应 BM25 的角色）、CLIP 图文检索、实体档案检索；RRF（k=60，带意图权重）融合，再做时间跨模态互证加成。正则只用来调权重，绝不用来关闭任何一路召回。

### 3.2 理论基础

- **Dense 和 Sparse 互补**：向量检索擅长语义相似（"像是在庆祝的画面"），但对专有名词、数字、精确台词、字幕关键词不稳定；词面检索（BM25/FTS5/双字 gram）恰好相反。视频问答两类需求都大量存在。
- **RRF 公式**：`score(d) = Σ_i w_i · 1/(k + rank_i(d))`，k=60 平滑常数。**核心优点：只依赖排名不依赖原始分数尺度**——余弦相似度和 BM25 分数量纲完全不同，加权分数融合必须做分数归一化（易错），RRF 天然免疫。
- **RRF 只能融合已有的召回，不能弥补缺失的召回源**——所以重点是补齐稀疏路、时间路，而不是调融合参数。
- **查询意图是复合的**："开头那个穿红衣服的人后来讲了什么"同时含视觉属性+时间约束+实体指代+音频需求，任何"二选一"的路由都会丢一半条件。

### 3.3 代码实现（`video_rag_retriever.cpp`，可讲得很细）

**Step 1 — `compileQueryPlan()`：确定性查询计划**
- 优先级：**显式 Constraints（来自 Tool 调用）> 问题文本中的时间表达**——Tool 给了时间边界就绝不被问题里的时间词覆盖。
- 解析：时钟区间（`00:30到01:20`）、时钟点（±5s 窗口）、"第 X 分 Y 秒"、开头/结尾（各 15s）、前半/后半、"当前/现在"（播放位置 ±10s）。
- 时间约束超出视频时长 → `temporalConstraintUnsatisfiable` 直接返回空，不去检索。

**Step 2 — `analyzeQuery()`：意图只调权重，不关路径**
- 视觉关键词命中且文本未命中 → visual 0.6 / text 0.3 / entity 0.1，反之亦然，都命中 0.45/0.45/0.1。
- **`needsVisualSearch` / `needsTextSearch` 恒为 true**——注释原话："关键词只用于调权，不能再把复合问题错误压缩为单路"。
- 证据类型偏好：画面细节类（穿/颜色/几个人）`prefersVisualEvidence`，台词类（说了什么/字幕）`prefersAudioEvidence`，叙事类（发生了什么/为什么）`prefersFusedEvidence`；两类同时命中则都不压制，交给融合证据。

**Step 3 — 四路召回（每路 `topK × candidateMultiplier` 超额召回）**
- `textPathSearch`：BGE 编码 query → `text_segments` 语义检索；
- `lexicalTextPathSearch`：**中文双字 gram + ASCII 词**的词面召回（`VideoRAGStore::searchLexical`），专吃精确台词、字幕、专名；
- `visualPathSearch`：CLIP text encoder 编码 query → `visual_frames` 图文检索；
- `entityPathSearch`：实体描述 BGE 编码 → `entity_profiles`。
- 每路结果按意图做 `applyEvidencePreference`：证据类型乘子（如台词类问题 SpeechSegment×1.20、纯视觉证据×0.70）。

**Step 4 — RRF 融合 + 去重 + 跨模态互证**
- `reciprocalRankFusion(perPath, weights, k=60)`，权重来自意图分析，`preferPath` 额外 ×1.25。
- `deduplicate`：**只在"同一证据类型 + 时间重叠 >70%"时才算重复**——同一场景的视觉/音频/融合证据时间相同但内容互补，绝不能互相去重（这是真实踩坑改出来的规则）。
- `applyTemporalCorroboration`：**时间邻近（<4s）且模态不同的命中互为佐证**，分数 ×(1 + 0.10×支持数，封顶 3)，并把 `corroborating_chunk_ids / corroborating_modalities` 写进 metadata 供生成和反思阶段使用。两个关键排除：① 候选本身是 independent/unknown 音频不参与；② **同源转写（都来自 whisper）不算互证**——避免"一条 ASR 被切两段"伪造成跨模态一致。

**Step 5 — 存储层的模型身份校验（`video_rag_store.cpp`）**
- `Filter::expectedEmbeddingModelId / expectedEmbeddingVersion`：检索时校验 chunk 的 `embedding_model_id`，**向量空间由模型身份定义而非维度**——512 维的 CLIP 和 512 维的 BGE 不能比余弦。旧数据未声明身份时仅在调用方未要求隔离时兼容，不会静默混用。
- 存储形态：内存 `vector<(chunk, embedding>` 暴力余弦 + SQLite 持久化 + 按视频惰性加载，接口按 FAISS IndexFlatIP 的 API 设计，规模上来可无缝替换。

### 3.4 真实工程问题

**问题 1：正则分流把复合问题压缩成单路（最重要的演进故事）**
最初版本：视觉正则命中 → 只走视觉检索；文本正则命中 → 只走文本检索。"开头那个穿红衣服的人说了什么"被分成视觉问题后，"说了什么"的 ASR 召回直接丢失。
**解决**：三步走——① 所有路径默认全开；② 关键词只调 RRF 权重；③ 证据类型偏好做乘子而非硬过滤。这个故事能讲 2 分钟，面试效果极好。

**问题 2：缺稀疏路，RRF 无米下锅**
"字幕里有没有'初始化失败'"这类精确词面问题，向量检索召回率很低。SQLite FTS5 对中文分词不友好，实现了**中文双字 gram + ASCII 词**的轻量词面召回，权重设为 dense 路的 0.85（辅助定位）。

**问题 3：去重误杀跨模态证据**
早期去重按时间重叠 >70% 就去掉"重复"，结果同一场景的视觉证据和音频证据时间完全相同，被误判重复互相挤掉。改为"同类型 + 时间重叠"双条件才去重。

**问题 4：同源证据伪互证**
ASR 段 A 和段 B 都来自同一份 whisper 转写，时间邻近，被互证逻辑当成"音频+音频跨模态一致"加分。加 `isDerivedFromSameTranscript` 排除。

### 3.5 面试官可能问的问题

| 问题 | 回答要点 |
|---|---|
| RRF 的 k=60 是什么？为什么不用分数加权融合？ | k 是平滑常数，抑制头部排名的分数支配。分数融合要跨量纲归一化（cos ∈ [-1,1]，BM25 ∈ [0,∞)），归一化参数对数据分布敏感；RRF 只用排名，鲁棒且免调参。 |
| 为什么没有上 Cross-Encoder 重排？ | 当前互证逻辑（跨模态时间邻近加成）起到了轻量 rerank 的作用且零额外推理成本；Cross-Encoder（如 BGE-Reranker）是预留的下一档升级：在 RRF Top-30 上精排到 Top-10，管线位置已留好。 |
| 为什么不用 FAISS？ | 单机单视频规模：10 分钟视频约 100~200 个 chunk、1~3MB 向量，内存暴力检索延迟毫秒级，FAISS 是引入外部依赖换不来的收益。但存储层接口按 IndexFlatIP 设计，规模上来直接换。 |
| 时间约束怎么和向量检索结合？ | 检索前过滤（Filter 里 `startMsGte/endMsLte` + Overlaps 区间重叠模式）而不是检索后过滤——先过滤避免 Top-K 全被时间外的结果占满。 |
| Entity 路召回的是什么？ | 实体档案（`entity_profiles` collection：类型/别名/出现记录/描述向量），用于"那个人/刚才的人"的指代解析，配合 Agent 层把实体档案显式注入 prompt。 |
| QueryPlanner 为什么不用 LLM 解析？ | 分层策略：时间表达、指代词这类模式用确定性规则解析（快、免费、可测试、无幻觉）；复杂复合意图才值得花一次 LLM 调用。规则先行是成本和确定性的正确默认。 |

---

## 4. 证据增强生成：证据包回注 VLM + Grounding

### 4.1 电梯话术（30 秒）

> 文本 caption 是画面的有损投影，只靠 caption 回答会丢颜色、数量、位置、细小文字。所以检索命中的代表帧必须回注给 VLM：EvidenceComposer 把命中片段格式化成带时间戳、来源标签的文本证据进 prompt，同时把证据关键帧（最多 3 张，缩放到 1280）和用户帧合并直接进 VLM 请求的多模态输入。回答侧做 Grounding：关键主张映射回 evidence_id、时间戳和代表帧，置信度由证据覆盖、模态多样性和跨模态互证共同决定，而不是答案长度。

### 4.2 理论基础

- 生成公式应为 `Answer = VLM(Q, retrieved_text, retrieved_frames)`，而不是 `LLM(Q, retrieved_caption)`。
- **回答质量上限受木桶约束**：`AnswerQuality ≤ min(召回率, 证据保真度, 生成质量)`——再强的 Agent 推理也变不出没被索引/召回的信息，所以优先级永远是：索引看到了 → 检索召回了 → VLM 拿到原始证据 → 回答可追溯 → 最后才轮到 Agent 花活。
- 可靠的置信度来源：召回/重排分数、多模态一致性、关键帧复核、主张→证据映射；**不可靠的来源：答案长度、字符串包含**。

### 4.3 代码实现

**`EvidenceComposer`（`src/service/rag/evidence_composer.h`）——证据包组装：**
- `formatText(evidence, maxItems=6, maxCharsPerItem=240)`：把命中片段格式化为 `[mm:ss-mm:ss] (来源标签) 内容` 的文本证据，写入 `VideoContext.retrievalEvidence` 进 prompt；
- `mergeFrames(userFrames, evidence, maxEvidenceFrames=3, maxFrameEdge=1280)`：**检索命中的代表帧 + 用户主动附的帧合并后直接进入 VLM 请求**——这一步修复了"检索到了关键帧但最终模型没收到"的断链；
- `toJson/fromJson`：只持久化回答追溯需要的字段，embedding 不进 checkpoint，避免膨胀。

**`VideoAgent::phaseReasonAndAct`（`video_agent.cpp`）——注入：**
- `enrichedCtx.retrievalEvidence = EvidenceComposer::formatText(m_retrievedEvidence)` + `evidenceFrames = EvidenceComposer::mergeFrames(...)`；
- 实体档案显式注入（最多 10 个：描述/类型/别名/出现次数）；
- 反思闭环（详见第 5 点）会把 `fixSuggestion` 作为"反思反馈"追加进证据，让重答模型知道上一版错在哪。

**`ReflectionEngine`（`reflection_engine.cpp`）——置信度与四项校验：**
- 四项检查：一致性（如"宣称无音频但元信息有音频"）、证据支撑（>80 字的事实性回答必须有可追溯证据）、时间合理性（正则抽取 `[mm:ss]` 时间戳校验是否超出视频时长）、幻觉检测（声明 token 在已知信息池的覆盖率 <1/3 视为疑似幻觉）。
- **置信度公式（重写后）**：基础 0.65（有证据）/0.35（无证据）+ 0.05×模态数（封顶 3）+ 0.05（有跨模态互证）− 0.12×issue 数，clamp 到 [0.2, 0.95]。**明确由证据覆盖和互证决定，不再由答案长度决定**——代码注释里直说"它仍是启发式分数，不能替代 VLM 复核"。

**QA 缓存（`qa_cache_manager.cpp`）：**
- 相似度阈值 0.88 的高门槛命中才复用；命中返回带"历史分析结论 + 相似度%"标记；播放器操作和"重新分析"意图强制绕过缓存。

### 4.4 真实工程问题

**问题 1：检索命中帧没有进入最终 VLM 输入（最经典的断链）**
检索层加载了 keyframeThumb 用于 UI 展示，但 `runQuery` 最终只传用户手动截的帧——检索帧在生成阶段被丢弃，等于 RAG 只做了"文本 RAG"。
**解决**：`EvidenceComposer::mergeFrames` 把证据帧并进 VLM 请求，形成 retrieval-augmented multimodal generation 的完整闭环。这是"文档里说这是标准闭环、不是可选增强"的直接体现。

**问题 2：反思置信度曾被答案长度和字符串包含定义**
旧版 ReflectionEngine：长答案没证据才判证据不足；"幻觉检测"是声明文本是否在信息池字符串中出现——这会把正确的同义改写判为幻觉，把复述证据的废话判为可信。
**解决**：重写为证据覆盖 + 模态多样性 + 互证加成 − issue 惩罚的结构，并把"需要 VLM 帧级复核"设为明确的演进方向（启发式只做初筛）。

**问题 3：QA 缓存误命中**
相似但不同的问题（"有几个人"vs"有几个人在说话"）命中缓存会返回错误答案。把阈值提到 0.88、返回时显式标注"历史结论+相似度"、播放器操作/重新分析强制绕过缓存三重防护。

**问题 4：证据文本撑爆上下文**
场景描述动辄几百字，多条证据直接拼接超过模型有效注意力。`formatText` 限 6 条、每条 240 字符截断，加上 U 形注意力布局（关键信息放开头结尾）。

### 4.5 面试官可能问的问题

| 问题 | 回答要点 |
|---|---|
| 多模态 RAG 和普通文本 RAG 最本质的区别？ | 检索单元从"文本 chunk"变成"带时间区间的多模态证据包"，且生成阶段必须回注原始模态（帧图像），因为 caption 是有损投影；文本 RAG 检索到的就是原始证据本身，视频 RAG 检索到的文本只是证据的投影。 |
| Grounding 具体怎么落地？ | 每个 chunk 有 chunkId；检索时互证逻辑写入 corroborating_chunk_ids；证据文本带时间戳和来源标签进 prompt；反思阶段校验时间戳合法性和主张的证据覆盖率；UI 上时间戳可点击跳转播放器。 |
| 置信度怎么算的？为什么不用 LLM 自评？ | 结构化启发式（证据覆盖+模态多样性+互证−惩罚），便宜、确定、可解释；LLM 自评有自我偏好且每次结果不稳定。承认局限：启发式不能替代帧级 VLM 复核，这是下一步。 |
| 证据冲突（视觉说 A、ASR 说 B）怎么办？ | 证据隔离存储保证冲突可见而不是被融合掩盖；检索按问题类型加权（画面类问题压低音频证据权重）；生成 prompt 要求区分事实与推断；互证元数据让模型知道哪些证据有跨模态支持。 |
| 为什么帧数限制 3 张、1280 边长？ | token/成本预算：每帧图像消耗大量 token，3 张代表帧已覆盖主要状态；1280 是质量与成本的平衡点，证据帧是"复核用"不是"播放用"。 |

---

## 5. Video Agent：规划、Tool Calling 与反思闭环

### 5.1 电梯话术（30 秒）

> Agent 采用五阶段循环 PERCEIVE → REPRESENT → REASON → ACT → REFLECT。PERCEIVE 做问题分类（13 种 QuestionType）和采样规划；REPRESENT 走 RAG 检索并受采样计划约束；REASON+ACT 通过 ToolOrchestrator 走多轮 Tool Calling（最多 5 轮、单答案工具调用数有上限），基于 SSE 解析增量 tool_calls；REFLECT 做四项校验，置信度过低时扩大检索、注入反思反馈重答一次。设计原则：Agent 只做规划、工具调用和回答，索引和原始多模态证据才是事实来源。

### 5.2 理论基础

- **Agent 不是事实来源**：回答质量 ≤ min(召回率, 证据保真度, 生成质量)。没有抽到的帧、错误的边界、未识别的文字，Agent 再多轮反思也变不出来，反而更容易幻觉。所以 Agent 的职责收敛为：判断是否需要局部复核 → 调工具 → 组织证据 → 回答并给引用。
- 受控循环必须有**硬上限 + 兜底**：工具循环不设限就是成本黑洞和死循环温床；到上限后必须"基于现有信息强制回答"而不是空转。

### 5.3 代码实现

**`VideoAgent::ask()`（`video_agent.cpp`）主流程：**
1. **快速路径**：QA 缓存命中直接返回；播放器操作（seek/play/pause 正则识别）和"重新分析"意图**强制绕过缓存**。
2. **PERCEIVE**：`PerceptionStrategy::classifyQuestion`（13 种 QuestionType：全局摘要/时间定位/实体查询/因果推理/计数/当前帧…）+ `decideSampling`（采样密度/帧预算/时间范围）。
3. **REPRESENT**：RAG 检索。两个约束协调规则（面试可细讲）：
   - **问题本身有时间表达时，采样计划不得覆盖它**（"00:30 发生了什么"不能被感知策略的候选范围改掉）；
   - 按问题类型调 topK 和路径偏好（CurrentFrame→3、GlobalSummary→8、EntityQuery→6 且 preferPath=entity）。
4. **REASON+ACT**：`phaseReasonAndAct` 组装证据上下文 + 实体档案（≤10 个）→ `ToolOrchestrator::runQuery`；播放器操作用 **`tool_choice` 强制指定** `control_player`（防止模型绕过工具直接说话）。
5. **REFLECT**：`ReflectionEngine::reflect` → 置信度 <0.5 且未重试 → **反思闭环**：去掉时间约束扩大检索（topK=10）、合并去重新证据、把 `fixSuggestion` 作为反思反馈追加进 prompt、重新走 REASON+ACT；重答后再反思一次但**不递归重试**（最多 1 次，防循环）。

**`ToolOrchestrator`（`tool_orchestrator.cpp`）多轮工具编排：**
- 流式 SSE：`AgentService::sendMessageWithTools` 发起带 tools 的请求，`responseChunk` 增量上抛给 UI 流式显示；
- `finishReason == "tool_calls"` 时解析 `delta.tool_calls` 增量拼装成 `ToolCall{id, name, arguments(JSON)}`；
- **限流**：`MAX_ROUNDS`（5 轮）+ `MAX_TOOL_CALLS_PER_ANSWER`（单答案工具总数上限），超限裁剪或强制用现有信息收尾；
- 工具结果组装成 `role=tool` 消息回填 `continueWithToolResults` 进入下一轮；
- 6 个工具：`seek_and_analyze`（跳转截帧分析）、`analyze_time_range`（区间多帧分析）、`search_video_content`（语义搜画面）、`get_transcript`、`get_scene_info`、`control_player`（seek/play/pause 联动播放器）。

**Workflow 模式**：另有基于 JSON 预设（`workflow/presets/video_qa.json`）的工作流执行器（LLM 节点 + Function 节点 + checkpoint 断点续跑），把固定问答链路从自由 Agent 循环里固化出来。

### 5.4 真实工程问题

**问题 1：OpenAI 协议 400 —— tool 消息前缺 assistant tool_calls 消息**
回填工具结果时只发了 `role=tool` 消息，OpenAI 兼容 API 直接 400（tool 消息必须紧跟对应的 assistant tool_calls 消息）。解决：缓存 `m_lastAssistantToolCalls`，下一轮回填时先补 assistant 消息再跟 tool 消息。这是所有自己撸 Tool Calling 的人都会撞的协议坑。

**问题 2：模型绕过工具直接回答播放器操作**
"跳到 2 分钟"这类指令，模型经常直接回"好的我帮你跳转"而不真的调工具。解决：识别播放器操作意图后把 `tool_choice` 从 `"auto"` 改为强制 `{"type":"function","function":{"name":"control_player"}}`。

**问题 3：工具调用死循环与成本失控**
模型可能反复调同一个搜索工具不收敛。三层防护：轮次上限（5）、单答案工具总数上限（超限裁剪本轮 calls）、达上限后"基于现有信息强制回答"兜底——用户体验上是拿到了不完美答案而不是无限等待。

**问题 4：并发重入**
用户连发两条消息，第二个 ask 会破坏第一个的状态（回调、m_retrievedEvidence 等成员被覆盖）。用 `m_busy` 防重入，忙时直接报"Agent 正在处理"。更彻底的方案是每请求一个独立会话对象（可坦承是已知改进点）。

**问题 5：反思重试的递归风险**
反思失败→重答→再反思失败→再重答……无限套娃。用 `m_reflectionRetries < kMaxReflectionRetries` 硬限制一次，且重答后只评估不再触发重试。

### 5.5 面试官可能问的问题

| 问题 | 回答要点 |
|---|---|
| 你的 Agent 和 ReAct 的区别？ | 形式上是 ReAct 的强化版：多了前置的问题分类和采样规划（PERCEIVE）、结构化证据注入（REPRESENT）、以及带证据校验的 REFLECT 反思闭环（失败会扩大检索带反馈重答），而不是简单 Thought→Action→Observation 循环。 |
| 为什么限制 5 轮工具调用？ | 经验值：视频问答绝大多数 1~2 轮足够（一次检索 + 一次区间复核）；超过 5 轮通常说明检索没召回该召回的，继续循环是浪费——正确动作是反思扩大检索，而不是让模型空转。 |
| 工具是并行执行的吗？ | 当前顺序执行（代码注释里注明"后续可并行独立工具"），因为多数场景单轮只有一个调用；并行化需要做依赖分析和结果聚合，是明确的优化项。 |
| Agent 的记忆怎么做的？ | 三层：对话历史（SQLite 持久化 + 压缩策略）、QA 缓存（向量检索相似问题，0.88 阈值复用）、实体档案（跨问题保持实体身份一致性，支持"那个人"指代）。 |
| REFLECT 失败后怎么恢复？ | 置信度 <0.5：去掉时间约束扩大检索 topK=10、合并新证据、把反思 issue 作为显式反馈注入 prompt 重答一次；再失败就带不确定性说明返回。 |
| 如果检索完全没命中怎么办？ | ReflectionEngine 会判"具体结论但无可追溯证据"；Agent 层面应触发 seek_and_analyze/analyze_time_range 做局部复核（工具层按需取帧分析），这就是"感知不足→下探采样"的设计意图。 |

---

## 6. 高频综合问题（跨模块，面试最后冲刺）

| 问题 | 回答框架 |
|---|---|
| **这个系统最难的地方？** | 不是接模型 API，是三件事：① 数据粒度设计（什么信息在索引期保留、什么丢弃——丢了就永远找不回）；② 多模态证据的边界（融合 vs 污染）；③ Agent 层的克制（让它消费证据而不是编造证据）。 |
| **如果视频从 10 分钟变成 10 小时，架构怎么变？** | 取帧：分片并行解码；索引：增量 + 分段任务队列；存储：内存向量换 FAISS IVF；L2 描述：从"按需单场景"变"优先级队列 + 预算控制"；检索：加粗粒度 Video/Event 层先召回再下探（分层索引的初衷就在这）。 |
| **幻觉怎么系统性抑制？** | 四道防线：索引期证据分离（推断不覆盖事实）→ 检索期互证（跨模态一致性加分）→ 生成期原始帧回注（有损投影问题）→ 反思期主张-证据覆盖率校验 + 低置信重答。 |
| **怎么评估这个系统好坏？** | 分层指标：索引质量（关键事件召回、边界合理性）、检索质量（Recall@K / MRR / 时间定位误差）、证据质量（引用帧是否真支撑结论）、生成质量（幻觉率、引用准确率）、性能（索引耗时、VLM 调用次数、端到端延迟）。 |
| **为什么整个用 C++ 自研而不是 Python 生态？** | 桌面端产品形态 + 本地小模型推理（ONNX Runtime/whisper.cpp）+ 与播放器深度联动（seek/截帧/播放控制）；RAG 核心逻辑（检索、融合、编排）本身不依赖 Python 生态，自研换来了对每一层的完全掌控。 |
| **一句话总结设计哲学？** | 先把视频变成可定位、可检索、可验证的多模态时序证据，再让模型基于证据回答——而不是让 Agent 猜答案，再用复杂流程掩盖索引不足。 |

---

## 7. 面试演示路线（如果被要求白板推导）

```
用户问："开头那个穿红衣服的人后来讲了什么？"

1. VideoAgent::ask
   ├── isPlayerOp? 否 / QA缓存? 未命中
   ├── PERCEIVE: EntityQuery + TemporalLocalization
   │     采样计划: 开头区间候选
   └── REPRESENT: compileQueryPlan
         问题含"开头" → startMs=0, endMs=15s（显式时间约束优先）

2. 四路召回（时间约束过滤 + Overlaps）
   ├── text_dense: BGE("穿红衣服的人...") → 场景描述/台词
   ├── text_lexical: 双字gram → 精确词面
   ├── visual: CLIP text → 关键帧
   └── entity: "那个人" → entity_profiles

3. RRF(k=60, 按意图加权) → 去重(同类型+时间重叠)
   → 跨模态互证(<4s, 排除同源whisper) → TopK

4. EvidenceComposer
   ├── formatText: [00:00-00:15](visual) 红衣男子进入画面...
   └── mergeFrames: 证据代表帧 ×3 → VLM 请求

5. ToolOrchestrator (≤5轮)
   └── (可选) analyze_time_range(后期区间, focus=人物讲话)

6. REFLECT: 时间戳合法性 / 证据覆盖 / 置信度
   └── 低置信 → 扩大检索 + 反思反馈 → 重答一次

7. Answer: "红衣男子在 [00:12] 首次出现，
   [04:35] 开始讲解……（时间戳可点击跳转）"
```

---

## 附：速查卡（面试前 10 分钟过一遍）

- **关键类名**：VideoAgent / VideoIndexer / VideoAnalysisService / FrameExtractor / VideoRAGStore / VideoRAGRetriever / QueryPlan / QueryIntent / EvidenceComposer / ToolOrchestrator / ReflectionEngine / PerceptionStrategy / QACacheManager / EntityTracker / AudioVisualAligner
- **关键数字**：RRF k=60；互证窗口 4s、加成 10%/个（封顶3）；去重阈值 时间重叠 70%；QA 缓存阈值 0.88；工具轮次 5；代表帧 ≤3/场景；证据文本 ≤6 条 ×240 字符；证据帧 ≤3 张 ×1280；TransNetV2 采样 1fps（≤600 帧）/直方图回退 10s 一帧（≤200）；videoId = SHA1(大小+前 1MB)
- **四个必讲的工程坑**：① extractThumbnail 逐帧重开解码器 → FrameExtractor 单开顺序解码；② 正则分流压缩复合问题 → 关键词只调权重；③ tool 消息缺 assistant tool_calls → 400；④ 检索帧没进最终 VLM 输入 → EvidenceComposer.mergeFrames
- **三条理论金句**：① 数据粒度必须和问题粒度匹配；② Caption 是有损投影，原始帧必须回注；③ AnswerQuality ≤ min(召回率, 证据保真度, 生成质量)
