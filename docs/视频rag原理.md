我已根据 `docs/rag构建.md` 进行归纳。这个文档的核心观点是：

> **视频 RAG 不是“给视频抽几张图，再让大模型回答”，而是一个以时间为主线、以多模态证据为基础、以检索和可验证回答为闭环的系统。**

---

# 一、先明确视频 RAG 要解决什么问题

普通文本 RAG 的对象通常是段落或文档片段；视频 RAG 的对象则同时包含：

- 画面：人物、物体、动作、场景变化
- 音频：说话内容、声音事件
- OCR：字幕、PPT、招牌、屏幕文字
- 时间：事件发生在什么时候、前后顺序是什么
- 原始证据：代表帧、音频片段、OCR 结果

因此，视频应该被抽象为一个多模态时序信号：

\[
V = \{(f_t, a_t, o_t)\}_{t=1}^{T}
\]

其中：

- \(f_t\)：时刻 \(t\) 的视频画面
- \(a_t\)：时刻 \(t\) 的音频或 ASR 结果
- \(o_t\)：时刻 \(t\) 的 OCR、字幕等观测结果

这意味着视频不能简单地看成“图片集合”。

例如：

- “画面里有什么？”可以通过单帧回答；
- “后来发生了什么？”需要比较前后多个时间点；
- “谁走进了画面？”需要观察人物的进入过程；
- “字幕什么时候发生变化？”需要分析多个时刻的 OCR；
- “开头那个穿红衣服的人后来讲了什么？”同时涉及视觉、实体、时间和 ASR。

所以，视频 RAG 的首要原则是：

> **数据粒度必须和问题粒度匹配。**

---

# 二、整体架构

文档建议的总体结构可以理解为两条流水线。

## 1. 视频入库流水线

```text
VideoAsset
    ↓
IngestionJob
    ├── FrameExtractor
    ├── TemporalSegmenter
    ├── ASR Extractor
    ├── OCR Extractor
    ├── Multi-frame Vision Analyzer
    ├── Visual Embedding
    └── SegmentStore
```

## 2. 问答检索流水线

```text
Question
    ↓
QueryPlanner
    ↓
Hybrid Retrieval
    ├── Dense Retrieval
    ├── Sparse Retrieval / BM25
    ├── Temporal Retrieval
    └── Entity Filtering
    ↓
RRF Fusion
    ↓
Reranker
    ↓
Frame-level Verification
    ↓
EvidenceComposer
    ↓
VLM / Agent Answer
    ↓
Grounding Verification
    ↓
Timestamp Citation
```

其中最重要的是：

> **Agent 不是事实来源，索引和证据才是事实来源。**

---

# 三、第一步：重构取帧，不要先换模型

文档指出，当前项目的性能瓶颈之一是 `SmartPlayer::extractThumbnail()`。

如果每次抽帧都执行：

1. 打开视频文件；
2. 初始化解码器；
3. seek 到目标时间；
4. 解码；
5. 保存临时 JPEG；
6. 重新读取 JPEG；
7. 删除临时文件；

那么抽取几百、几千帧时，I/O 和解码初始化成本会非常高。

## 正确方式

新增独立的 `FrameExtractor`：

- 一个视频只打开一次解码器；
- 按时间递增顺序批量解码；
- 直接输出 RGB 帧或 JPEG；
- 支持粗采样；
- 支持局部密采样；
- 与播放器状态完全隔离。

建议基于 FFmpeg 的：

- `avformat`
- `avcodec`
- `sws_scale`

实现一个专门用于索引的解码器封装，而不是让索引服务依赖播放器 SDK。

## 为什么这是第一优先级

因为如果取帧本身很慢，后续无论使用什么视觉模型，整体索引速度都会被拖慢。

同时，取帧模块是所有上层能力的基础：

- 镜头切分依赖帧；
- 代表帧依赖帧；
- OCR 依赖帧；
- 视觉 embedding 依赖帧；
- 最终回答的引用也依赖帧。

因此正确的改造顺序不是“先换一个更强的 VLM”，而是：

> **先保证视频能高效、稳定、可重复地被读取。**

---

# 四、第二步：从“场景 + 一帧”升级为分层时序索引

文档推荐四层索引结构：

```text
Video
  └── Event / Clip
        └── Shot
              └── Frame
```

## 1. Video 层

描述整个视频：

- 全局摘要；
- 章节；
- 视频主题；
- 总时长；
- 主要人物或实体；
- 主要事件时间线。

适合回答：

> “这个视频主要讲了什么？”

## 2. Shot 层

`Shot` 是一个连续镜头，通常由镜头切换确定。

每个 `Shot` 保存：

- 起止时间；
- 3～8 张代表帧；
- 视觉摘要；
- OCR；
- ASR；
- embedding；
- 模型版本。

适合回答：

> “镜头什么时候发生切换？”

## 3. Clip / Event 层

`Clip` 或 `Event` 是具有完整语义的事件片段，通常为 5～30 秒。

例如：

```text
00:10 - 00:18：人物进入房间
00:18 - 00:25：人物坐下
00:25 - 00:33：人物拿起桌上的杯子
```

一个 30 秒的镜头中可能包含多个事件，因此不能让一个 `Shot` 承担全部语义。

适合回答：

> “这一段发生了什么？”

## 4. Frame 层

原始帧主要用于：

- 精确定位；
- OCR；
- 小目标识别；
- 颜色、数量、姿态判断；
- 最终 grounding；
- 点击时间戳查看原视频。

适合回答：

> “画面右边有几个人？”  
> “这个人手里拿的是什么？”  
> “招牌上写了什么？”

## 为什么只保留场景首帧是不够的

当前如果每个 Scene 只保存 `sceneStartIdx`，就相当于把几十秒的视频压缩为一个静态画面。

这是一种不可逆的信息损失：

- 无法表示动作；
- 无法表示人物进入或离开；
- 无法表示字幕变化；
- 无法表示事件顺序；
- 无法回答“后来发生了什么”。

因此，升级为 `Shot / Clip / Event + Multi-frame` 不是简单增加复杂度，而是让索引结构真正符合视频语义。

---

# 五、第三步：使用多帧网格，而不是逐帧调用 VLM

文档建议对一个 `Shot` 选取 4～9 张代表帧，制作带时间戳的网格图：

```text
[00:10] [00:14] [00:18]
[00:22] [00:26] [00:30]
```

然后一次调用 VLM，生成：

- 视觉事实摘要；
- 关键人物和物体；
- 动作；
- 屏幕文字；
- 状态变化；
- 不确定项。

## 为什么不能只使用单帧

假设一个片段包含 \(n\) 张候选帧：

### 逐帧调用

\[
O(n)
\]

优点是局部细节多，缺点是调用成本高，而且模型每次只能看到一个局部状态。

### 只看首帧

\[
O(1)
\]

成本最低，但时序信息损失最大。

### 多帧网格

\[
O(1)
\]

一次调用观察多个时间状态，在成本和信息覆盖之间取得平衡。

它本质上是把时间序列编码为空间布局：

\[
[f_{t_1}, f_{t_2}, \dots, f_{t_k}]
\Rightarrow \text{Temporal Grid}
\]

如果每张图上标注时间戳，VLM 就能同时观察：

- 前期状态；
- 中间状态；
- 后期状态；
- 状态变化过程。

这不是严格意义上的视频时序模型，但对普通 VLM 来说，是一个非常实用的工程折中。

## 建议的代表帧策略

可以采用：

- 均匀采样；
- 首帧、中间帧、尾帧；
- 场景变化较大处加密采样；
- OCR 发生变化处加密采样；
- 人物进入、离开画面处加密采样；
- 运动剧烈处加密采样。

不要固定只取首帧。

---

# 六、第四步：视觉、ASR、OCR 必须隔离存储

视频中的不同模态并不等价。

| 模态 | 表达的事实 |
|---|---|
| 视觉 | 画面中实际看到了什么 |
| ASR | 音轨中说了什么 |
| OCR | 画面上的文字是什么 |
| 融合描述 | 模型根据多个证据形成的解释 |

例如：

- ASR 说“这是小王”，不能证明画面中的人就是小王；
- 字幕出现某句话，不一定代表画面中的人物说了这句话；
- 视觉模型说“一个人在开会”，也不代表它看清了屏幕上的具体内容。

因此，系统应保留证据来源：

\[
E = \{E_v, E_a, E_o, E_f\}
\]

其中：

- \(E_v\)：视觉事实；
- \(E_a\)：音频事实；
- \(E_o\)：OCR 事实；
- \(E_f\)：融合推论。

不要让一种模态覆盖另一种模态。

## 推荐的数据记录

每条证据至少包含：

```text
evidence_id
video_id
segment_id
start_time
end_time
modality
raw_content
derived_content
source_frame_ids
model_id
model_version
confidence
```

这样回答时才能区分：

> “画面中看到了什么”  
> “说话内容是什么”  
> “模型推断了什么”

---

# 七、第五步：Embedding 必须记录模型身份

文档特别强调，不能只通过向量维度判断两个向量能否比较。

例如：

\[
v_{clip} \in \mathbb{R}^{512}
\]

\[
v_{bge} \in \mathbb{R}^{512}
\]

即使两者都是 512 维，也不能直接计算：

\[
\cos(v_{clip}, v_{bge})
\]

因为它们属于不同的语义空间。

## 每条 embedding 至少记录

- `embedding_model_id`
- `embedding_version`
- `modality`
- `dimension`
- `normalized`
- `index_version`

## 存储原则

视觉、文本、OCR 向量应放在独立的 collection 或 index 中：

```text
visual_index
text_index
ocr_index
```

不能因为“都是 512 维”就混在一起比较。

## 中文场景的模型选择

文档认为当前 OpenAI CLIP 不适合作为中文图文检索的主模型。

更合理的选择是：

- 中文对齐模型；
- 多语视觉语言模型；
- Chinese-CLIP；
- 多语 SigLIP；
- BGE 等文本模型用于纯文本语义检索。

注意：

> `BGE` 适合文本语义检索，不应该直接替代视觉图文对齐模型。

---

# 八、第六步：查询不能只靠正则分流

当前如果通过关键词或正则把问题简单划分成：

- 视觉问题；
- 文本问题；
- 实体问题；

会丢失复杂问题中的组合约束。

例如：

> “开头那个穿红衣服的人后面说了什么？”

这个问题同时包含：

- 视觉属性：红衣服；
- 时间约束：开头、后面；
- 实体指代：同一个人；
- 音频需求：说了什么。

更合理的做法是先将查询解析为结构化计划：

```json
{
  "visual_query": "穿红衣服的人",
  "transcript_query": "说了什么",
  "ocr_query": "",
  "entity_constraints": {
    "same_entity": true
  },
  "temporal_constraint": {
    "from": "beginning",
    "relation": "later"
  },
  "expected_answer_type": "transcript"
}
```

## QueryPlanner 的职责

它不一定每次都需要调用大模型，可以分层处理：

### 简单问题

用规则解析：

- 开头；
- 结尾；
- 第几分钟；
- 某个时间范围；
- 之前；
- 之后。

### 复杂问题

使用轻量 LLM 或小模型解析：

- 人物指代；
- 多条件组合；
- 跨模态关联；
- 事件顺序；
- 实体跟踪。

这样可以兼顾成本和准确率。

---

# 九、第七步：采用混合检索，而不是只做向量检索

视频问题同时存在两类需求。

## 1. Dense Retrieval

适合语义相似搜索：

> “有哪些像是在庆祝的画面？”

一般使用余弦相似度：

\[
sim_{dense}(q,d)
=
\cos(E_q,E_d)
\]

它擅长：

- 同义表达；
- 语义概念；
- 视觉场景；
- 描述性问题。

但它对以下内容通常不稳定：

- 专有名词；
- 数字；
- 产品型号；
- 精确台词；
- 字幕中的某个词。

## 2. Sparse Retrieval

使用 SQLite FTS5、BM25 等方法，适合：

- 精确台词；
- 字幕关键词；
- 人名；
- 产品型号；
- 数字；
- OCR 文本；
- 标题。

例如：

> “字幕里有没有‘初始化失败’这几个字？”

这类问题不能只靠向量相似度。

## 3. Temporal Retrieval

独立处理时间约束：

- 开头；
- 结尾；
- 第 3 分钟；
- 10～20 秒；
- 某事件之前；
- 某事件之后。

时间不是普通文本过滤条件，而是视频检索的核心维度。

## 4. Entity Retrieval

对人物、物体或其他实体进行约束：

- 同一个人；
- 某人出现过几次；
- 某物体什么时候出现；
- 某人物进入画面后发生了什么。

## 5. 多路召回与 RRF

可以得到多路结果：

```text
R_dense
R_sparse
R_temporal
R_entity
```

然后使用 RRF 合并：

\[
R =
RRF(R_{dense}, R_{sparse}, R_{temporal}, R_{entity})
\]

需要注意：

> RRF 只能融合已有的召回结果，不能弥补缺少的召回路径。

如果没有稀疏检索和时间检索，仅仅加入 RRF 并不能解决问题。

---

# 十、第八步：Reranker 负责精排

初步召回追求覆盖率，可能会带来很多“看起来相关、实际不够相关”的片段。

因此需要对候选 Top-N 做更精确的重排：

\[
TopK = Rerank(Q, R_{1:N})
\]

重排时可以综合：

- 问题与视觉描述的匹配度；
- 问题与 ASR 的匹配度；
- OCR 命中情况；
- 时间约束是否满足；
- 实体约束是否满足；
- 多模态之间是否相互支持；
- 是否存在原始帧证据。

典型流程是：

```text
多路召回 Top 50
    ↓
RRF 合并 Top 30
    ↓
Cross-Encoder 重排 Top 10
    ↓
帧级复核 Top 3～5
    ↓
构造证据包
```

---

# 十、第九步：检索命中的帧必须回注给 VLM

这是视频 RAG 和普通文本 RAG 的关键区别。

文本 caption 是原始画面的有损投影：

\[
Caption = g(Frame)
\]

它只能保留模型当时认为重要的信息，可能丢掉：

- 小目标；
- 精确颜色；
- 数量；
- 姿态；
- 空间位置；
- 细小文字；
- 用户提问时才重要的细节。

例如 caption 是：

> “一个人在街道上行走。”

它无法可靠回答：

- 他穿什么颜色的衣服？
- 左手拿了什么？
- 后方招牌写了什么？
- 他是第几个人进入画面的？

因此最终回答应使用：

\[
Answer =
VLM(Q, retrieved\ text, retrieved\ frames)
\]

而不是：

\[
Answer =
LLM(Q, retrieved\ caption)
\]

## 证据包示例

```text
[00:32 - 00:45]

视觉描述：
人物从画面左侧走向桌边。

ASR：
“我们接下来打开这个文件。”

OCR：
屏幕上出现“初始化配置”。

代表帧：
frame_00:33.jpg
frame_00:39.jpg

证据来源：
visual + ASR + OCR
```

最终 VLM 应该同时接收：

- 用户问题；
- 视觉描述；
- ASR；
- OCR；
- 代表帧；
- 时间范围；
- 证据 ID。

这才构成真正的多模态 Retrieval-Augmented Generation。

---

# 十一、第十步：Agent 只负责规划和调用工具

`ToolOrchestrator` 仍然有价值，但职责应该受控。

它可以负责：

1. 判断是否需要局部复核；
2. 调用 `analyze_time_range`；
3. 调用 `seek_and_analyze`；
4. 组织证据；
5. 基于证据包生成回答；
6. 输出引用和不确定性。

但它不应该：

- 自己猜测视频事实；
- 代替索引系统保存真相；
- 仅通过多轮反思弥补缺失证据；
- 使用答案长度判断置信度。

## 为什么 Agent 不能成为事实源

视频 RAG 的回答质量受下面这个关系约束：

\[
AnswerQuality
\leq
\min(
RetrievalRecall,
EvidenceFidelity,
GenerationQuality
)
\]

也就是说，只要其中一项很差，最终质量就会被它限制。

例如：

- 没有抽到关键帧，Agent 无法凭空恢复；
- shot 边界错误，Agent 无法准确理解事件；
- OCR 没识别到文字，Agent 无法可靠回答屏幕内容；
- 检索没有召回正确片段，Agent 的思考越多反而越容易幻觉。

因此优先级应该是：

```text
索引是否看到了正确信息
    ↓
检索是否召回正确片段
    ↓
VLM 是否拿到了原始证据
    ↓
回答是否正确引用证据
    ↓
Agent 是否需要进一步推理
```

---

# 十二、Grounding：让每个结论都能回到证据

专业视频 RAG 不能只输出一句答案，还应说明答案依据什么。

每条回答中的关键主张，都应该能映射到：

- `evidence_id`
- 时间范围；
- 代表帧；
- ASR 片段；
- OCR 片段；
- 证据来源类型。

例如：

```text
回答：
穿红衣服的人在 00:42 左右走到桌旁，随后说“开始配置”。

引用：
[00:40 - 00:45]
evidence_id: ev_1024
来源：visual + ASR
```

## 置信度不应由字符串规则决定

更可靠的置信度来源包括：

- 召回分数；
- 重排分数；
- 多模态一致性；
- VLM 对关键帧的复核结果；
- 回答主张是否都能映射到证据；
- 时间范围是否明确；
- 是否存在相互冲突的证据。

如果证据不足，应明确回答：

> “在当前召回的片段中无法确认。”

而不是让 Agent 猜测。

---

# 十三、推荐的数据模型

一个合理的视频 RAG 至少需要以下实体。

## `Video`

```text
video_id
uri
duration
width
height
fps
global_summary
index_version
created_at
```

## `Shot`

```text
shot_id
video_id
start_time
end_time
representative_frames
visual_summary
asr_ids
ocr_ids
embedding_refs
```

## `Event / Clip`

```text
event_id
video_id
start_time
end_time
event_type
description
related_shots
entity_refs
evidence_ids
```

## `Frame`

```text
frame_id
video_id
timestamp
image_uri
width
height
ocr_result
visual_embedding_ref
```

## `Evidence`

```text
evidence_id
modality
source_type
source_ref
start_time
end_time
content
model_id
model_version
confidence
```

## `Embedding`

```text
embedding_id
model_id
model_version
modality
dimension
normalized
index_version
vector_ref
```

这里最重要的是：

> **原始证据、派生描述、模型身份和索引版本都必须保留。**

这样模型升级、索引重建、问题追溯和质量评估才有基础。

---

# 十四、结合当前项目，推荐的改造顺序

根据文档中对当前实现的分析，建议按以下顺序实施。

## 阶段一：基础读取能力

先改造：

- `FrameExtractor`
- 批量顺序解码；
- RGB/JPEG 输出；
- 粗采样和密采样；
- 解码器生命周期管理。

目标是消除 `SmartPlayer::extractThumbnail()` 的重复打开和临时文件链路。

## 阶段二：时序分层

增加：

- `Shot`；
- `Clip`；
- `Event`；
- 多代表帧；
- 帧级时间戳；
- 事件边界。

不要继续让单个场景首帧代表整个场景。

## 阶段三：多模态索引

分别建立：

- 视觉描述；
- ASR 语义段；
- OCR 文本段；
- 视觉 embedding；
- 文本 embedding；
- OCR embedding。

同时保存模型 ID、版本、模态和索引版本。

## 阶段四：多帧视觉分析

将当前单关键帧描述改为：

- Shot 内选择 4～9 帧；
- 合成带时间戳网格；
- 一次调用 VLM；
- 输出视觉事实、变化、不确定项和屏幕文字。

## 阶段五：混合检索

完善：

- QueryPlanner；
- Dense Retrieval；
- SQLite FTS5/BM25；
- Temporal Retrieval；
- Entity Filtering；
- RRF；
- Cross-Encoder Reranker。

## 阶段六：证据回注

修复当前“检索到了关键帧，但最终模型没有收到关键帧”的问题：

- 检索结果加载代表帧；
- 将代表帧加入最终 VLM 输入；
- 同时传入 ASR、OCR、caption 和时间戳。

## 阶段七：Grounding 和评估

增加：

- evidence ID；
- 时间戳引用；
- 关键主张验证；
- 多模态一致性评分；
- 证据不足提示；
- 可点击跳转到视频时间点。

---

# 十五、如何判断系统是否“合理专业”

不要只看最终回答是否流畅，还要评估整个链路。

## 索引质量

- 关键事件是否被抽到？
- 镜头切分是否合理？
- 代表帧是否覆盖前中后状态？
- OCR 和 ASR 是否有准确时间戳？

## 检索质量

- Recall@K；
- MRR；
- nDCG；
- 时间定位误差；
- 精确台词召回率；
- 视觉事件召回率。

## 证据质量

- 回答引用的帧是否真的支持结论？
- 是否混淆了台词和画面人物？
- 是否把模型推断当成视觉事实？
- 是否存在无法验证的主张？

## 生成质量

- 答案是否正确；
- 是否完整；
- 是否引用准确；
- 是否表达不确定性；
- 是否出现幻觉。

## 性能指标

- 单视频索引耗时；
- 单视频解码次数；
- VLM 调用次数；
- 平均检索延迟；
- 存储成本；
- 增量索引耗时。

---

# 最终总结

一个有理论基础的专业视频 RAG，应遵循以下原则：

1. **视频是时序信号，不是图片集合。**
2. **数据粒度要分层：Video、Event、Shot、Frame。**
3. **不能让场景首帧代表整个时间段。**
4. **多帧网格是成本和时序覆盖之间的实用折中。**
5. **视觉、ASR、OCR 必须独立建模并保留来源。**
6. **Embedding 必须绑定模型身份、版本、模态和索引版本。**
7. **查询要解析为视觉、音频、OCR、时间、实体、答案类型等结构化计划。**
8. **检索要采用 Dense、Sparse、Temporal、Entity 多路召回。**
9. **RRF 后还需要精排和帧级复核。**
10. **检索命中的原始帧必须回注给 VLM。**
11. **Agent 只负责规划、工具调用和回答，不负责保存事实。**
12. **最终答案必须能够回溯到带时间戳的证据。**

一句话概括：

> **先把视频转换成可定位、可检索、可验证的多模态时序证据，再让 VLM 或 Agent 基于证据回答；而不是先让 Agent 猜答案，再用复杂流程掩盖索引不足。**

