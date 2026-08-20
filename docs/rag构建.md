text
VideoAsset
  └─ IngestionJob（可取消、可恢复、版本化）
      ├─ FrameExtractor（单次打开解码器，批量顺序解码）
      ├─ TemporalSegmenter（shot / clip / event）
      ├─ Modality Extractors
      │   ├─ ASR 语义段
      │   ├─ OCR 文本段
      │   ├─ 多帧视觉描述
      │   └─ 视觉 embedding
      └─ SegmentStore（原始证据、派生证据、模型版本）

Question
  └─ QueryPlanner
      ├─ 视觉 / 文本 / OCR / 时间约束解析
      ├─ Hybrid Retrieval（dense + BM25 + temporal）
      ├─ Reranker
      └─ EvidenceComposer
          └─ 时间戳 + caption + ASR + OCR + 代表帧
              └─ VLM / Agent Answer
                  └─ grounding 校验与可点击引用
1. 先重构取帧与切分，不要先换模型
新增独立 FrameExtractor，不要再通过 SmartPlayer::extractThumbnail() 为每帧重复打开文件、seek、JPEG 落盘、读回、删除。

目标是：

一个视频只打开一次解码器；
按时间递增批量抽帧；
输出缩放后的 RGB 帧或 JPEG；
支持粗采样和局部密采样；
与播放器状态完全隔离。
现状的取帧链路是索引性能的根因：

playerservice.cppL144-L166
// 每次 captureFrameAt 都调用 extractThumbnail，经历临时 JPG 落盘和回读。
建议使用 FFmpeg 的 avformat / avcodec / swscale 实现。项目已经链接 FFmpeg，只需补充 swscale 并新增解码器封装；不要让索引服务依赖播放器 SDK。

2. 数据粒度从“场景 + 一帧”改为“shot / clip / event + 多证据”
推荐四层：

Video：全局摘要、章节。
Shot：镜头切换区间；每 shot 保留 3～8 张代表帧。
Clip/Event：按动作、ASR 语义、视觉变化合并成 5～30 秒事件片段。
Frame：用于精确定位、OCR、小物体或最终 grounding。
不要让“场景首帧”承担整个时段的视觉表示。它不适合问“后来发生了什么”“谁进入画面”“字幕什么时候变化”。

3. 用多帧网格描述替代单帧 caption
对一个 shot 选 4～9 张代表帧，做带时间戳的网格图，一次 VLM 调用生成：

视觉事实摘要；
关键人物、物体、动作；
屏幕文字；
状态变化；
不确定项。
这比逐帧调 VLM 成本更低，也让模型获得最关键的时间上下文。当前 doDescribeSceneWithCallback() 仍只取一个关键帧。

4. 模态必须隔离，索引必须携带模型身份
VideoRAGStore 不能再只靠“向量长度相同”来判定是否可比较。每条 embedding 至少记录：

embedding_model_id
embedding_version
modality
dimension
normalized
index_version
视觉向量、文本向量、OCR 向量必须放独立 collection/index；禁止因碰巧同为 512 维而互相比较。

中文场景下，当前 OpenAI CLIP 不应继续作为中文图文检索主模型。至少换为中文对齐或多语模型，例如 Chinese-CLIP 或 SigLIP 多语版本；BGE 仅做文本语义检索。

5. 检索应是“混合召回 + 重排 + 证据包”，不是正则分流
查询计划建议输出结构化对象：

text
{
  visual_query,
  transcript_query,
  ocr_query,
  entity_constraints,
  temporal_constraint,
  expected_answer_type
}
然后：

Dense：视觉、caption、ASR、OCR 分路召回；
Sparse：SQLite FTS5 / BM25 召回精确台词、字幕、标题；
Temporal：开头、结尾、第几分钟、某个范围；
RRF 合并；
cross-encoder rerank；
对 top 片段做帧级复核；
组装证据包给 VLM。
证据包必须携带代表帧图像，而不只是文本：

text
[00:32 - 00:45]
视觉描述：……
ASR：……
OCR：……
代表帧：frame_00:33.jpg, frame_00:39.jpg
证据来源：visual + OCR + ASR
6. Agent 只做“计划、工具调用、回答”，不要成为索引真相源
保留 ToolOrchestrator 有价值，但应该让它只负责：

判断是否需要局部复核；
调 analyze_time_range 或 seek_and_analyze；
基于证据包回答；
给出引用和不确定性。
ReflectionEngine 不应再以答案长度、字符串包含关系定义置信度。更可靠的置信度应来自：

召回/重排分数；
多模态一致性；
VLM 对关键帧的复核结果；
回答中的每条主张是否能映射到 evidence ID。

1. 为什么要先重构取帧与分段
理论基础：视频不是图片集合，而是时序信号
视频语义可表示为：

𝑉
=
{
(
𝑓
𝑡
,
𝑎
𝑡
,
𝑜
𝑡
)
}
𝑡
=
1
𝑇
V={(f 
t
​
 ,a 
t
​
 ,o 
t
​
 )} 
t=1
T
​
 
其中 
𝑓
𝑡
f 
t
​
  是画面，
𝑎
𝑡
a 
t
​
  是音频，
𝑜
𝑡
o 
t
​
  是 OCR/字幕等观测。
单张首帧只能覆盖 
𝑓
𝑡
0
f 
t 
0
​
 
​
 ，不能表示：

动作：需要比较 
𝑓
𝑡
1
→
𝑓
𝑡
2
f 
t 
1
​
 
​
 →f 
t 
2
​
 
​
 ；
进入/离开画面：依赖时间变化；
屏幕文字变化：依赖多个时刻；
场景内事件顺序：依赖时间序列。
因此“每场景仅首帧”在信息论上就是有损压缩：一个可能长达几十秒的片段被压成单个静态观测。对“发生了什么”“后来谁出现”“什么时候变了”这类问题，信息在索引阶段已经不可逆丢失。

你当前就是该情况：

scene_detector.cppL137-L158
// 每个 Scene 的 keyframe 固定取 sceneStartIdx，对应场景首帧。
所以建议从 scene + 1 frame 升级为 shot / clip / event + multi-frame，并不是增加复杂度，而是让数据粒度与问题粒度匹配。

2. 为什么要把检索命中的帧再次送给 VLM
理论基础：文本 caption 是视觉信息的有损投影
可以理解为：

Caption
=
𝑔
(
Frame
)
Caption=g(Frame)
其中 
𝑔
g 是 VLM 生成的文本描述。Caption 只保存模型当时“认为值得描述”的一部分信息，通常会丢掉：

小目标；
精确位置；
颜色、数量、姿态；
细小文字；
提问时才显得重要的细节。
例如 caption 为“一个人在街道上行走”，无法可靠回答：

穿什么颜色衣服？
左手拿了什么？
后方招牌写什么？
是第几个人走入画面？
因此 RAG 的生成阶段应使用：

Answer
=
VLM
(
𝑄
,
retrieved text
,
retrieved frames
)
Answer=VLM(Q,retrieved text,retrieved frames)
而不是只使用：

Answer
=
LLM
(
𝑄
,
retrieved caption
)
Answer=LLM(Q,retrieved caption)
当前实现虽然能从索引恢复缩略图，但它们没有进入最终 runQuery() 的帧列表：

video_rag_retriever.cppL175-L185
// 检索命中会加载 keyframeThumb。
video_agent.cppL339-L344
// 最终模型调用仍只传入 userFrames，而非检索命中的关键帧。
所以“检索帧回注 VLM”是标准的 retrieval-augmented multimodal generation 闭环，不是可选的视觉增强。

3. 为什么要做多帧网格，而不是逐帧大量调用 VLM
理论基础：时序覆盖与调用成本之间的压缩优化
假设一个片段有 
𝑛
n 张候选帧：

逐帧 VLM：成本约为 
𝑂
(
𝑛
)
O(n)，但模型每次只能看到局部；
单首帧：成本 
𝑂
(
1
)
O(1)，信息损失很大；
𝑘
k 张代表帧拼图：成本接近 
𝑂
(
1
)
O(1)，覆盖多个时间状态。
网格图的作用本质上是把时间序列编码为空间布局：

[
𝑓
𝑡
1
,
𝑓
𝑡
2
,
…
,
𝑓
𝑡
𝑘
]
⇒
Temporal Grid
[f 
t 
1
​
 
​
 ,f 
t 
2
​
 
​
 ,…,f 
t 
k
​
 
​
 ]⇒Temporal Grid
如果每格烧录时间戳，VLM 可以同时观察“前—中—后”状态，从而理解状态变化。它不是严格的视频时序模型，但在普通 VLM 上是成本、覆盖度和实现难度之间很实用的折中。

这也是为什么建议“shot 内选 4～9 帧，再一次描述”，而不是按现在的单关键帧生成描述：

video_analysis_service.cppL265-L280
// 当前仅优先取 s.keyframe，缺失时才走单次截帧。
4. 为什么要将 shot、clip、event 分层
理论基础：不同语义单元有不同时间尺度
视频问题天然跨尺度：

问题	合适粒度
“画面里有什么？”	Frame
“镜头什么时候切换？”	Shot
“这一段发生了什么？”	Clip / Event
“视频主要讲什么？”	Video
“某人一共出现几次？”	Entity timeline
若只有“场景”和“全视频摘要”两层，系统无法稳定处理中等时间尺度的问题。
比如一个镜头可能持续 30 秒，但其中包含“人进门 → 坐下 → 拿起杯子”三个事件；单一 Scene 并不能表达它们。

建议的层级本质是一个分层索引：

Video
⊃
Event/Clip
⊃
Shot
⊃
Frame
Video⊃Event/Clip⊃Shot⊃Frame
好处是检索先用粗粒度召回，再在局部用细粒度验证。这样既不需要对全视频逐帧问大模型，也不会因只保留一帧失去时序信息。

5. 为什么要分开视觉、ASR、OCR，并保留来源
理论基础：多模态之间不能默认“语义等价”
视觉、语音、文字分别描述不同事实：

视觉：屏幕上看得到什么；
ASR：音轨中说了什么；
OCR：画面文字是什么；
融合描述：模型根据上述信息形成的解释。
它们不是同一种证据。例如台词提到“小王”，不能证明画面中的人就是小王；字幕也可能来自画外旁白。

因此比较稳健的证据模型应是：

𝐸
=
{
𝐸
𝑣
,
𝐸
𝑎
,
𝐸
𝑜
,
𝐸
𝑓
}
E={E 
v
​
 ,E 
a
​
 ,E 
o
​
 ,E 
f
​
 }
其中：

𝐸
𝑣
E 
v
​
 ：视觉事实；
𝐸
𝑎
E 
a
​
 ：音频事实；
𝐸
𝑜
E 
o
​
 ：OCR 事实；
𝐸
𝑓
E 
f
​
 ：融合推论。
你当前已经朝这个方向做了一部分：纯视觉、音频摘要、融合描述被拆开写入，而不是强行互相覆盖。这是正确演进方向。

video_analysis_service.cppL475-L485
// 视觉、音频、融合结果被作为不同 chunk 写入。
我建议继续扩展 OCR，不是因为“功能越多越好”，而是因为 OCR 与 ASR、视觉的错误模式不同，对“标题是什么”“PPT 写了什么”“字幕内容”有不可替代价值。

6. 为什么不能只靠当前正则选择视觉/文本检索
理论基础：查询意图是潜变量，关键词只是弱观测
用户问题的真实查询意图通常可拆为：

𝐼
(
𝑄
)
=
(
𝑞
𝑣
,
𝑞
𝑎
,
𝑞
𝑜
,
𝑡
,
𝑒
,
𝑦
)
I(Q)=(q 
v
​
 ,q 
a
​
 ,q 
o
​
 ,t,e,y)
其中：

𝑞
𝑣
q 
v
​
 ：视觉查询；
𝑞
𝑎
q 
a
​
 ：音频/转写查询；
𝑞
𝑜
q 
o
​
 ：OCR 查询；
𝑡
t：时间约束；
𝑒
e：实体约束；
𝑦
y：答案类型。
例如“开头那个穿红衣服的人后面说了什么”同时包含：

视觉属性：红衣服；
时间约束：开头、后面；
实体指代：同一个人；
文本需求：说了什么。
靠正则把它划成“视觉”或“文本”，必然丢掉另一半条件。当前实现确实是关键词分流：

video_rag_retriever.cppL63-L115
// 通过视觉/文本/实体正则决定检索路径和权重。
更合理的是由轻量 LLM 或可控规则将查询解析为结构化计划，再分别召回并融合。这并不意味着每次都必须调用昂贵大模型：可以先用规则处理明显的时间表达，复杂查询再调用小模型或 LLM。

7. 为什么需要混合检索与 rerank
理论基础：dense retrieval 和 sparse retrieval 互补
向量检索擅长语义相似：

sim
𝑑
𝑒
𝑛
𝑠
𝑒
(
𝑞
,
𝑑
)
=
cos
⁡
(
𝐸
𝑞
,
𝐸
𝑑
)
sim 
dense
​
 (q,d)=cos(E 
q
​
 ,E 
d
​
 )
但对精确词、专有名词、数字、字幕、产品型号、短句台词常不稳定。

BM25/FTS 擅长词面精确匹配：

score
𝐵
𝑀
25
(
𝑞
,
𝑑
)
score 
BM25
​
 (q,d)
但不理解近义表达。

视频中恰好两类问题都很多：

“他说了什么”“字幕里有没有某词” → 稀疏检索非常重要；
“像是在庆祝的画面在哪” → 向量检索更合适。
因此常见策略是多路召回后融合：

𝑅
=
RRF
⁡
(
𝑅
𝑑
𝑒
𝑛
𝑠
𝑒
,
𝑅
𝑠
𝑝
𝑎
𝑟
𝑠
𝑒
,
𝑅
𝑡
𝑒
𝑚
𝑝
𝑜
𝑟
𝑎
𝑙
,
𝑅
𝑒
𝑛
𝑡
𝑖
𝑡
𝑦
)
R=RRF(R 
dense
​
 ,R 
sparse
​
 ,R 
temporal
​
 ,R 
entity
​
 )
然后对候选 top-
𝑁
N 使用更精确但更慢的重排器：

TopK
⁡
=
Rerank
⁡
(
𝑄
,
𝑅
1
:
𝑁
)
TopK=Rerank(Q,R 
1:N
​
 )
你现在有 RRF，但没有稀疏路、真正的时间路与 rerank；RRF 只能融合“已有的召回”，不能弥补缺失的召回源。

8. 为什么强调 embedding 模型身份，而非只看维度
理论基础：向量维度相同不代表语义空间相同
即使两个 embedding 都是 512 维：

𝑣
𝑐
𝑙
𝑖
𝑝
∈
𝑅
512
,
𝑣
𝑏
𝑔
𝑒
∈
𝑅
512
v 
clip
​
 ∈R 
512
 ,v 
bge
​
 ∈R 
512
 
也不能计算：

cos
⁡
(
𝑣
𝑐
𝑙
𝑖
𝑝
,
𝑣
𝑏
𝑔
𝑒
)
cos(v 
clip
​
 ,v 
bge
​
 )
因为它们的坐标轴不表示同一种语义，向量空间没有对齐。维度相同只说明数组长度相同，不说明每一维的含义一致。

当前存储层以向量长度是否相等作为可比较条件：

video_rag_store.cppL320-L325
// 仅比较 target.size() 与 queryVector.size()，没有校验模型和模态身份。
这属于数据契约缺失。即便目前 collection 恰好分开，后续增加模型、重建索引或升级 embedding 后，仍会埋下静默错误。记录 model_id + version + modality 是正确性要求，不只是方便调试。

9. 为什么建议把 Agent 降为“证据消费者”
理论基础：RAG 质量的上限由证据质量约束
一个简单但重要的关系：

Answer Quality
≤
min
⁡
(
Retrieval Recall
,
Evidence Fidelity
,
Generation Quality
)
Answer Quality≤min(Retrieval Recall,Evidence Fidelity,Generation Quality)
再复杂的 Agent loop、更多的 tool 调用和更长的 prompt，都无法从缺失的帧、错误的边界、未识别的 OCR 中恢复真相。

所以优先级应该是：

索引是否看到了正确的信息；
检索是否召回了正确的片段；
生成模型是否接收到足够原始证据；
最后才是 Agent 如何推理、是否反思、如何调用工具。
当前 ReflectionEngine 主要依据字符串规则、答案长度和已知文本池判断，无法做真正事实验证：

reflection_engine.cppL70-L80
// 长答案且没有 evidence 才判定证据不足。
reflection_engine.cppL124-L148
// “幻觉检测”基于声明文本是否能在信息池中出现。
所以我不建议继续强化这层启发式，而是把“grounding”前移：对时间、数量、人物、文字等关键主张，回到候选原始帧/ASR/OCR 做验证。

结论
这些建议的共同理论基础是：

时序信息不能由单帧代表；
文本描述不能完全替代原始视觉证据；
不同模态需独立建模、带来源归因；
检索需要召回互补性与重排精度；
生成质量受证据质量上限约束；
系统边界清晰比在 Agent 层堆复杂状态机更重要。