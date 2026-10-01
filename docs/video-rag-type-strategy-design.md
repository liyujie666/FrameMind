# 按视频类型选择 RAG 构建策略：FrameMind 改造设计

日期：2026-10-01。状态：P0–P2 代码已落地，真实素材质量验收待完成。本文保留原设计和改造前定位，实际实现及已知差异见 [实施记录](video-rag-implementation-status.md)。

本文依据当前 `src/` 实现编写。实现落地时应同步更新 `architecture-design.md`（模块与数据库）、`agent-core-design.md`（Agent 与证据规则）、`api-protocol.md`（工具参数），以及 `rag_flow.md`（实际执行流程）。本文作为这次改造的实施方案，不覆盖现有文档的事实描述。

## 1. 目标与核心决定

让视频类型在正式的语义切分与昂贵分析之前介入，选择整套构建策略。策略决定提取顺序、切分依据、取帧预算、结构化产物、摘要和检索偏好。

核心决定：

1. 保留 `Scene` 作为视觉镜头，新增 `SemanticUnit` 作为策略决定的理解单元。
2. 新增轻量探测、类型识别、策略注册与构建编排；复用已有解码、ASR、向量、模型通道和存储。
3. `VideoIndexer` 执行离线提取，`VideoAnalysisService` 执行模型分析；两者不自行决定整套构建流程。
4. 按议题、知识点、步骤或事件做 Level 2，并从这些单元生成全局摘要。
5. 原始证据与派生产物分别管理。类型纠正优先重建派生产物，缺少的原始证据按需补提取。
6. 用构建清单、产物状态和版本替代“存在任何 chunk 就认为索引完成”的判断。

首批完整实现 `Meeting`、`Interview`、`Lecture`、`Tutorial`、`Generic` 策略；`Narrative` 在后续阶段实现。会议和访谈共享对话处理能力，但使用不同的知识字段与摘要规则。

## 2. 当前代码与问题定位

| 当前位置 | 当前行为 | 本次改动 |
|---|---|---|
| `service/agent/video_indexer.cpp::startIndex()` | 串行 L0/L1；新任务提前 `invalidateVideo()`；内存缓存主要判断 level | 由编排器传入提取计划；按清单复用证据；新构建成功前保留已发布版本 |
| `VideoIndexer::buildLevel0()` | 按是否启用 TransNetV2 选择采样密度，再调用 `detectScenes()` | 镜头成为可选提取能力；正式采样由策略计划确定 |
| `VideoIndexer::buildLevel1()` | 先视觉向量，再完整 ASR；额外写入语音合并窗口 | 按策略调度；语音主导策略优先转写；原始段仍保存 |
| `service/rag/speech_segmenter.cpp` | 按时长、停顿、句末标点合并；默认约 12–30 秒窗口 | 保留为长度约束和回退；新增真正的主题／问答／步骤分段 |
| `VideoAnalysisService` 构造函数 | 监听 `levelReady(1)` 后自动 `startDescribeAllScenes()` | 移除这一隐式触发，统一由编排器启动单元分析 |
| `VideoAnalysisService::detectVideoType()` | 仅看前 3 个 Scene 的关键帧；类型存在内存和摘要前缀中 | 前移到探测阶段；共享模型；独立持久化类型、依据与不确定性 |
| `getScenePromptForType()` | 类型只改变场景描述 Prompt | 移入策略 Prompt 与结构 schema；视觉事实 Prompt 保持严格取证 |
| `AudioVisualAligner::overlappingSpeechSegments()` | 每个 Scene 默认最多 8 段、1500 字 | 保留小场景对齐用途；完整主题摘要使用分页证据包 |
| `VideoAnalysisService::summarizeVideo()` | 只汇总 `sceneDescriptions` | 汇总所有语义单元，再做章节／全局归并 |
| `VideoIndexer::representation()` | 从 SceneSummary 反推场景；从场景末尾估计时长；存在描述即可推断 L2 | 从原始快照和构建清单恢复，不从描述反推结构与完成状态 |
| `VideoAnalysisService::onVideoOpened()` | `hasIndexedContent()` 为真就跳过构建 | 校验文件、策略、模型、产物完整性后复用或续建 |
| `VideoRAGRetriever` | 文本 dense、词面、CLIP、实体召回与 RRF；按问题调权 | 保留，增加活动版本过滤、单元种类偏好与上下文展开 |
| `PerceptionStrategy::checkSufficiency()` | 部分问题用“有命中”或“L2 完成”判断充分 | 针对问题检查原始证据、分析覆盖和缺失能力 |
| `QACacheManager` / `VideoAgent` | QA 证据以 scene ID 列表为核心 | 改为 chunk／unit 引用和构建版本 |
| `VideoAnalysisViewModel` / 时间线 / 总结页 | 主要使用 scene 信号和 scene 列表 | 增加语义单元、类型和构建状态；镜头仍可查看 |

必须考虑的现有能力边界：

- `AudioDecoder` 已支持指定区间解码，可以用于短音频探测；区间转写时间需加回绝对偏移。
- `SpeechSegment` 当前只有时间和文本，没有说话人身份、词级时间和质量字段。
- 当前没有专用 OCR、说话人分离、动作连续识别或非语音音频事件索引服务；不能在方案里将其当成已经可用。
- `Scene::visibleTexts` 来源于 VLM；与未来专用 OCR 共用文字证据接口，但来源必须明确。
- `EmbeddingService` 的 BGE 输入上限为 512 token。大主题不能直接拼接全文后做一个向量；必须保留短证据、摘要索引及父子展开。
- `OneShotVlmChannel` 有独立后台 AgentService，内部仍串行；可以发送空 frames 做文本请求。它不会让同一个通道并发执行。
- `DatabaseManager` 的 SQLite 连接属于创建线程；`VideoRAGStore::insertChunk()` 已做跨线程封送，但其他新增读写也必须遵循同一约束。

## 3. 目标职责与目录

```text
ChatViewModel / UI
    ↓ 现有 onVideoOpened / analyzeVideo 门面
VideoAnalysisService
    ↓ 交给统一编排入口
VideoRAGBuildCoordinator
    ├─ MediaProbe + VideoProbeService
    ├─ VideoTypeClassifier
    ├─ VideoRAGStrategyRegistry → IVideoRAGBuildStrategy
    ├─ VideoIndexer → 离线证据提取
    ├─ SemanticUnitBuilder → 分段、证据关联与验证
    ├─ VideoAnalysisService → 单元描述／结构提取／摘要
    └─ VideoRAGStore → 清单、快照、单元、证据与发布
```

建议新增文件：

```text
src/model/video_content_profile.h
src/model/video_rag_build_plan.h
src/model/video_build_context.h
src/model/semantic_unit.h
src/service/agent/media_probe.h/.cpp
src/service/agent/video_probe_service.h/.cpp
src/service/agent/video_type_classifier.h/.cpp
src/service/agent/video_rag_build_coordinator.h/.cpp
src/service/rag/semantic_unit_builder.h/.cpp
src/service/rag/unit_evidence_builder.h/.cpp
src/service/rag/strategies/video_rag_build_strategy.h
src/service/rag/strategies/video_rag_strategy_registry.h/.cpp
src/service/rag/strategies/generic_strategy.h/.cpp
src/service/rag/strategies/dialogue_strategy.h/.cpp
src/service/rag/strategies/lecture_strategy.h/.cpp
src/service/rag/strategies/tutorial_strategy.h/.cpp
src/service/rag/strategies/narrative_strategy.h/.cpp   # 后续阶段
```

`DialogueStrategy` 提供 Meeting / Interview 两种 preset，避免复制相同分段逻辑。策略对象是无状态规则对象，不拥有 DB、播放器或网络客户端。

`VideoAnalysisService` 保留现有公共门面与 Level 3 工具能力，增加非拥有的编排器指针。编排器调用它的新执行方法；执行方法不再调用构建入口，也不自动响应 indexer 的 L1 信号，避免形成递归流程。`VideoIndexer::startIndex()` 在过渡期仅作为旧通用流程兼容入口；应用内入口全部迁移到编排器。

第一版可继续由 `VideoIndexer` 保存路径到 representation 的映射，但必须区分正在构建的预览与已发布读视图。`VideoAnalysisService::representation()` 和 Agent 获取已发布视图，ViewModel 可另外订阅构建预览；恢复 DB 的代码改为 store 读取快照／清单。后续再将 representation 生命周期完全收敛到编排器，避免这次同时重写所有调用方。

## 4. 类型、策略与数据模型

### 4.1 类型映射

将 cpp 匿名命名空间里的 `VideoContentType` 移到共享模型，并提供稳定字符串序列化。保留已有代码的字符串兼容：

| 内容标签 | 构建策略 | 首期行为 |
|---|---|---|
| `meeting`（新增） | `meeting_v1` | 议题、观点、明确决策与待办 |
| `interview` | `interview_v1` | 问答、观点、论据；不默认提取会议待办 |
| `educational` | `lecture_v1` | 知识点、解释、例子、推导 |
| `presentation` | `lecture_v1` | 共享分段能力，使用报告／演讲 preset |
| `tutorial` | `tutorial_v1` | 步骤、前置条件、动作和结果 |
| `documentary` / `drama` | `narrative_v1` | 完成该策略前明确映射到 `generic_v1`，不标记为已支持专用叙事 |
| `news` / `vlog` / `unknown` | `generic_v1` | 保守多模态分段，后续新增专用策略 |

标签与策略分开：新增业务标签未必需要新增一整套构建器。

### 4.2 内容画像与构建计划

以下为接口草图，实际头文件需补齐 enum、JSON codec、错误类型和 Qt metatype：

```cpp
struct VideoContentProfile {
    VideoContentType primaryType;
    QVector<VideoContentType> secondaryTypes;
    float classificationConfidence = 0.0f;
    QString classificationSource;       // user / multimodal / heuristic / fallback
    QString reasoning;
    QStringList probeEvidenceIds;
    QStringList missingSignals;
    QString classifierVersion;
    bool userOverride = false;
};

struct VideoRAGBuildPlan {
    QString strategyId;
    QString strategyVersion;
    ExtractionPolicy extraction;        // 必需／可选能力、执行顺序
    SegmentationPolicy segmentation;    // 边界信号、长度限制、上下文
    SamplingPolicy sampling;            // 基础覆盖、单元采样、局部密集预算
    AnalysisPolicy analysis;            // prompt/schema、分页预算、重试
    IndexPolicy index;                  // 原始／摘要／结构字段索引
    RetrievalPolicy retrieval;          // 单元偏好、展开关系、默认配额
    QString planFingerprint;
};
```

构建计划必须包含生效的模型 ID／版本、Prompt/schema 版本、能力协商结果及关键参数。模型自报的分类 confidence 只是调度信号，不当作已校准的概率。每项任务有 Required / Optional 属性，不支持的能力在计划中标明降级结果。

### 4.3 SemanticUnit

```cpp
struct SemanticUnit {
    QString unitId;
    QString buildId;
    SemanticUnitKind kind;              // Topic / QAPair / Concept / Step / Event / Chapter
    int64_t startMs = 0;
    int64_t endMs = 0;                   // 全部区间统一 [startMs, endMs)
    QString title;
    QString parentUnitId;               // 可为空；章节层级
    QStringList previousUnitIds;
    QStringList nextUnitIds;
    QVector<int> shotIds;               // 与现有 Scene 关联
    QStringList sourceChunkIds;         // 原始 ASR／帧／文字证据
    VideoContentProfile localProfile;
    QString visualDescription;
    QString audioSummary;
    QString fusedDescription;
    QJsonObject structuredFacts;
    UnitAnalysisState analysisState;
    EvidenceCoverage coverage;
};
```

`structuredFacts` 中每条观点、定义、步骤、决策都带 `source_chunk_ids`；责任人、期限等可为空，并有 evidence-supported / uncertain 状态。LLM 不得生成不存在的证据 ID。结构字段通过 JSON schema 和程序验证后才入库。

镜头与单元通过时间关联，一个镜头可关联多个知识点，一个议题可跨多个镜头。`Scene` 不改名、不换含义，也不把 unit ID 塞进 `scene_id`。

`EvidenceCoverage` 区分：原始材料是否存在、文本分页是否完整处理、视觉采样实际覆盖、失败页、主动跳过项。没有使用视觉证据不等于覆盖率 100%；几个帧也不代表证明了整个连续动作。

### 4.4 VideoRepresentation 与状态

新增 `profile`、`buildPlan`、`semanticUnits`、`activeBuildId`、`rawSnapshotId`、`buildRevision`、`artifacts`，保留原有 scenes、speechSegments、sceneDescriptions 等兼容字段。

产物状态使用 Pending / Running / Ready / Partial / Failed / Skipped，加上 required、处理范围与缺失原因。索引级别保留作 UI 概览：

| 级别 | 新含义 |
|---|---|
| L0 | 文件元信息与探测结果可用，类型／策略已解析或显式回退 |
| L1 | 计划要求的基础证据与初始语义单元可检索 |
| L2 | 必需单元分析与全局摘要完整；降级结果显示 Partial，不伪装为完整 L2 |
| L3 | 当前问题相关区间的补充取证，不全局升级视频 |

业务逻辑查看产物状态；不再用 `level >= Level2` 判断任意问题都能回答。首次构建的部分产物可用，但 UI 和检索上下文必须注明状态。

## 5. 类型识别前移与执行状态机

### 5.1 探测

`MediaProbe` 基于已有 FFmpeg 依赖从文件读取真实 VideoInfo，独立于播放器当前状态。探测失败不能用 0 时长继续做百分比采样。

`VideoProbeService` 收集：

- 分散位置的低分辨率画面；至少覆盖主体、中后段，避免只看片头。
- 少量局部连续帧，判断是否是操作、动作或静态讲解。
- 若 ASR 可用，调用区间解码和短转写；返回绝对时间戳及采样范围。
- 已有有效原始转写／帧快照可直接复用，不重复探测。

可从 5 个分散位置、每个 10–20 秒音频的预算开始试验；短视频合并重叠范围。该预算是待评测参数，不保证类型正确，也不替代主体内容的完整提取。

首期探测转写仅用于分类，不与完整转写简单拼接入库，避免重复 ASR 段。后续按相同分块任务键复用它，才允许作为完整原始证据的一部分。

类型选择优先级：用户明确指定 > 当前文件且版本兼容的有效画像 > 多模态分类 > 可用信号启发式 > Generic。自动分类不确定时仍继续工作，显示回退状态；需要纠正时用户可切换策略。

分类请求的输出直接对应共享 profile：type、secondary_types、confidence、reasoning、probe_evidence_ids、missing_signals。程序验证标签白名单与真实样本 ID，再由 registry 选择策略；不允许模型返回任意类名或执行计划。用户覆盖保留自动检测结果作参考，不能被后续自动分类悄悄改回。

### 5.2 执行状态机

```text
ResolveFileAndCache
  → Probe
  → Classify
  → ResolveStrategyAndCapabilities
  → ExtractRequiredEvidence
  → BuildAndValidateSemanticUnits
  → AnalyzeUnits
  → BuildChaptersAndSummary
  → ValidateAndPublish
```

原则是“少量证据先分类，正式构建按策略执行”，不要求全量 ASR 完成后才选择策略。计划内无依赖的任务允许并行，但共享模型实例遵循单任务执行限制。

缓存规则：同路径再次打开先验证当前文件指纹；有 raw evidence 不代表对应策略已完成；有摘要也不代表当前策略摘要可复用。手动 `analyzeVideo()` 新增 forceDerivedRebuild 选项，避免当前转发 `onVideoOpened()` 后再次命中缓存而无法重建。

## 6. 各策略的具体构建规则

### 6.1 Meeting / Interview

优先完整 ASR，基础视觉覆盖作为辅助。以话题和问答结构为主边界，句末、停顿为边界候选，镜头切换默认是弱信号。一个话题可跨人物镜头切换。

Meeting 输出观点、议题结论、明确决策、待办和未解决问题；Interview 输出问题、回答、观点和论据。两者的单元摘要都关联原始发言。

说话人分离是增强能力：现阶段没有 diarization 时 `speaker_id` 为 unknown，不从单张人脸推断“这句话是谁说的”。日后新增说话人服务也不能直接将声纹标签等同于真实姓名。

### 6.2 Lecture / Presentation

ASR 主题为主，PPT／板书文字和内容变化为辅助。构建“概念—解释—例子／推导”结构；章节用于大主题导航。

PPT 换页提供候选边界，同一知识点跨页可以合并，同一页面讲多个主题可以拆开。没有 OCR 时使用带来源的 VLM 可见文字；无法辨认的公式保留不确定状态，不编造。

长静态镜头内按知识点生成多个单元。完整转写分批分析，不再经过 Scene 的 8 段／1500 字限制。

### 6.3 Tutorial

结合解说、界面／对象状态变化构建 Step。镜头切换可作为辅助，状态变化优先于画面颜色变化。每步提取目标、前置条件、可见操作和结果。

取帧围绕步骤开始、操作过程及结果状态；预算不足或操作未采到时只描述可见状态并记录缺口。首期候选来自解说中的步骤语句及视觉变化，模糊候选交给 VLM；后续接入专用 OCR 或连续动作能力。

### 6.4 Generic / Narrative

Generic 同时保留文本窗口与视觉证据，保守构建通用主题／事件单元；没有语音则按画面变化与长度限制建立单元。

Narrative 在后续阶段实现：以镜头骨架结合对白、人物、地点、事件连续性合并事件，使用序列帧复核。不将同时间的旁白直接归因给画面人物，不从前后顺序自动推导因果关系。

### 6.5 策略参数与本地内容覆盖

第一版优先 C++ 规则与 preset；不需要动态插件系统。min/max duration、token budget、frame budget 和边界权重全部进入计划及 fingerprint。

类型不同必须产生可观察的差异：提取顺序、边界、关联帧、结构 schema、摘要和检索展开至少体现相应业务需求，不能只是更换 Prompt。

局部策略覆盖在全局策略跑通后增加：课程中的实验演示可使用 Tutorial 子策略，会议中的屏幕操作可生成 Step 子单元。子策略保留 parent/unit links，避免相互覆盖整个视频。

## 7. 分段、分页和证据包

### 7.1 分段流程

1. 按绝对时间整理原始转写段、视觉边界和文字变化，生成候选边界。
2. 对相邻文本上下文做主题相似度比较；按策略组合边界信号。
3. 用停顿、句末、长度约束调整边界；问答策略尝试保持问题及回答关系。
4. 首期不要求 Whisper 提供词级时间，边界落在已有段端点；无法准确切分的大原始段记录精度限制。
5. 歧义部分可批量调用文本模型细化；只允许引用输入证据 ID 和合法边界。
6. 校验时间范围、顺序、source IDs、长度与证据覆盖，失败时回退到受约束窗口。

不取所有边界的并集。长度上限是计算约束，不是“超过就换话题”。超长主题可生成同父单元的 Part，保留语义连续关系。

### 7.2 分页取证与摘要

`UnitEvidenceBuilder` 从原始快照取关联 ASR、帧和文字；时间重叠表示关联候选，音画关系仍需保守判断。

每个证据包包含来源 ID、时间、文本、实际帧 PTS、来源类型、覆盖状态、token 预算。页内可以带前后上下文，但拥有的核心范围明确，去重后统计处理覆盖。

```text
单元所有原始证据
  → token 受限证据页
  → 每页结构化分析 + 原始引用
  → 单元摘要与合并后的结构事实
  → 章节摘要
  → 全局摘要
```

不能通过增大 `AudioVisualAligner::Limits` 把整段长视频塞进一次调用。某页失败则重试或将单元标为 Partial；全局摘要说明对应缺口。

BGE 索引采用有上限的子证据文本与短单元摘要；embedding 前计算实际 tokenizer 长度，超限分块，不依赖模型静默截断。长主题的上下文通过 source links 和层级展开取回。

### 7.3 模型输出分层

- 视觉事实：可见文字、物体、动作／状态、实际帧时间，不从视频类型推断台词。
- 音频事实：原始转写及其受支持的内容提取。
- 融合解释：带来源与音画关系的主题、步骤或事件描述。

纯视觉分析 Prompt 不再要求仅凭人物表情推断访谈观点。业务类型 Prompt 用于已具备相应证据的单元理解阶段。

## 8. 关键接口调整

以下是目标接口示意，表示职责而非可直接编译的补丁：

```cpp
class IVideoRAGBuildStrategy {
public:
    virtual ~IVideoRAGBuildStrategy() = default;
    virtual QString id() const = 0;
    virtual VideoRAGBuildPlan makePlan(
        const VideoContentProfile&, const AvailableCapabilities&) const = 0;
    virtual UnitSchema unitSchema() const = 0;
    virtual PromptBundle prompts() const = 0;
};

class VideoRAGBuildCoordinator : public QObject {
public:
    void start(const QString& videoPath, const BuildOptions&);
    void cancel();
    void changeType(const QString& videoPath, VideoContentType);
};

// VideoIndexer：工作线程产出值对象，主线程提交快照。
void extractAsync(const VideoBuildContext&, const ExtractionRequest&,
                  std::function<void(ExtractionResult)>);

// VideoAnalysisService：独立执行分析，不自行启动下一整条构建。
void analyzeUnitAsync(const VideoBuildContext&, const SemanticUnit&,
                      const UnitEvidenceBundle&, const PromptBundle&,
                      std::function<void(UnitAnalysisResult)>);
void summarizeUnitsAsync(const VideoBuildContext&, const SummaryRequest&,
                         std::function<void(SummaryResult)>);
```

`SemanticUnitBuilder` 执行候选算法及校验，策略参数控制行为；它不直接访问 DB。需要 LLM 的分段请求由编排器通过统一模型通道提交。

`SceneDetector` 第一阶段不扩展成多模态巨类。只补齐区间覆盖及连续帧边界接口，语义切分留在新模块。

`VideoRAGStore` 新增快照／清单／单元读写、按版本读取、按单元查来源、批量提交和发布接口。明确 store 是这些新表的访问入口，避免 UI 自己拼接 SQL 解释 active build。

推荐接口分组：`loadActiveBuild(videoId)` / `loadRawSnapshot(snapshotId)`；`saveCandidateBuild(manifest)` / `saveUnitBatch(buildId, units, links, chunks)`；`publishBuild(buildId, expectedActiveBuildId)`；`getUnit(buildId, unitId)` / `getUnitSources(...)`；`resolveReadView(videoId)`。发布使用 expectedActiveBuildId 检查，避免过期候选覆盖更新结果。新增 batch 提交只有 DB 事务成功后才更新内存，不能沿用当前 insertChunk 先写内存、后写 DB 的顺序宣称已经原子。

Indexer 的 Stage 在原枚举末尾追加 Probe、Classify、SemanticSplit；保留原信号作适配，新编排进度按计划中的任务权重计算。`indexCompleted` 仍只表示离线提取任务结束，另加 `buildFinished(videoId, buildId, status)` 表示完整构建终态，UI 不用一次提取完成信号宣称摘要已完成。

## 9. 线程、取消与版本隔离

### 9.1 主线程与工作线程

- MediaProbe、解码、ASR、向量编码、确定性分段在工作线程执行。
- QObject 编排器、模型队列、SQLite 读写、canonical representation 提交与 UI 信号在主线程。
- 工作者接收不可变快照和每任务独立 cancellation token，返回值对象；不边修改共享 `repr` 边让 UI 读取。
- 相同 Whisper／CLIP／embedding 实例的调用序列化，或使用各自串行执行器。不要假设已有服务能跨线程并发推理。
- 数据库批量写入排队到 store 线程；主线程不能同步等待正在使用 BlockingQueuedConnection 回写的 worker。

### 9.2 构建上下文

```cpp
struct VideoBuildContext {
    QString videoId;
    QString fileFingerprint;
    QString buildId;                     // 每次候选构建唯一 ID
    QString rawSnapshotId;
    quint64 taskGeneration;
    QString strategyId;
    QString planFingerprint;
    QString cancellationKey;             // videoId:buildId:taskGeneration
    std::shared_ptr<std::atomic_bool> cancelled;
};
```

缓存复用键是 spec fingerprint；候选构建 ID 则区分不同运行。每个后台回调在 DB 写入、repr 提交和下一任务调度前校验完整上下文。

保留 `QPointer` 防止对象销毁，但它不能代替版本检查。现有通道取消后台请求后可能不执行 onDone；编排器取消必须独立进入 Cancelled 状态，不能等待递归计数收尾。

给 `OneShotVlmChannel` 增加带 Success / Failed / Cancelled 状态的结果回调，兼容旧 QString 回调；取消 pending 和 active 时做到每个请求最多一个终态。交互请求优先于尚未执行的后台请求，当前后台请求不会自动被抢占。

### 9.3 发布

新构建写入独立 build namespace；旧 active build 继续可读。所需产物通过校验后，事务内切换 `video_metadata.active_build_id`，再替换内存读视图和发出信号。

首次构建可发布明确的 Partial 视图，后续在同 build 下提升 `buildRevision`。查询开始时固定 build ID、revision 与 raw snapshot；有变化则拒用旧 QA／工作流结果。使用内存读快照隔离同一查询的读取，不在升级过程中让一次查询混用两代内容。

取消或失败不清空旧可用索引。历史候选可按保留策略清理；开始新任务不调用全视频 `invalidateVideo()`。

## 10. 持久化与缓存迁移

### 10.1 建议新增／扩展表

以下是核心字段，不是最终 migration SQL；所有动态值仍使用参数绑定。

| 表 | 核心字段与职责 |
|---|---|
| `video_metadata`（扩展） | 保留原字段；新增 `active_build_id`。旧 summary/level 作为活动版本的兼容投影，不作为独立权威状态 |
| `video_raw_snapshots`（新增） | `snapshot_id` PK、`video_id`、文件 fingerprint、`media_info_json`、`shots_json`、`extraction_manifest_json`、created_at；保存真实元信息、镜头及提取器版本／覆盖 |
| `video_rag_builds`（新增） | `build_id` PK、video_id、raw_snapshot_id、spec fingerprint、strategy_id/version、profile_json、plan_json、artifact_state_json、status、revision、summary、diagnostics_json、时间；类型与构建的权威来源 |
| `video_semantic_units`（新增） | `(build_id, unit_id)` PK、kind、start/end、title、parent_id、local_profile_json、analysis_json、structured_json、coverage_json、state |
| `video_unit_links`（新增） | build_id、from_unit_id、relation、target_kind、target_id；表达 source_chunk、related_shot、parent、previous、next 等关系 |
| `rag_chunks`（扩展） | 新增 `build_id`、`raw_snapshot_id`；原有 metadata_json 继续携带 unit_id、unit_kind、strategy、模型、evidence role 等 |

`rag_chunks` 的原始证据 build_id 为空、raw_snapshot_id 必须有效；派生产物 build_id 与 raw_snapshot_id 均有效。查询只读取活动 build 所关联的 raw snapshot，以及该 build 的已发布派生产物。

原始 snapshot 的提取清单是恢复关键：只恢复 SceneSummary 会漏掉没有做视觉描述的会议镜头和实际采样帧。帧路径／PTS 及 chunk IDs 应写入镜头 JSON 或对应 source links。

表索引至少包括 `(video_id, status)`、`(video_id, raw_snapshot_id, build_id)`、`(build_id, start_ms, end_ms)`、`(build_id, from_unit_id, relation)`。来源可能跨表，foreign key 可用于 build/unit 的确定关系，跨来源关系由程序校验。

通过 `DatabaseManager::ensureColumn()` 风格做增量迁移，新表用 `CREATE TABLE IF NOT EXISTS`。迁移幂等，新增 migration version，仅在全部 DDL 成功后提升版本；不在迁移中删除旧数据。

### 10.2 ChunkType 与集合

保持现有 4 个 Collection，不新增每类型一个向量集合。新语义单元和结构事实仍放 `TextSegments`。

在现有 `VideoChunk::ChunkType` 末尾追加 `UnitSummary`、`UnitFact`、`TextEvidence`、`ChapterSummary`；保留旧枚举整数，更新 store 的字符串映射、EvidenceComposer 和知识库显示。不要让新类型反序列化成 SceneSummary。

- `TextEvidence` 区分 frame_text／ocr_text／transcript_slice，并保留 source 引用。
- `UnitFact` 索引一条短事实，避免把大量结构字段拼成一个不可检索的大 JSON。
- `UnitSummary` 与 `ChapterSummary` 是导航／召回入口，回答精确事实时展开原始证据。
- 现有 Event 曾用于 speech_semantic；不将它直接解释成新业务事件。

原始 chunk ID 包含 snapshot ID 和提取版本；派生 chunk ID 在 `makeChunkId()` 的 discriminator 中加入 build ID、unit ID、role 和分片号，避免不同策略在相同时间区间相互覆盖。

### 10.3 复用与重建

| 变化 | 处理 |
|---|---|
| 类型、分段参数、Prompt/schema 变化 | 复用兼容原始证据；重建单元和派生产物；缺少的采样补提取 |
| embedding 模型变化 | 原始内容与单元结构可复用，重新编码相关索引 |
| Whisper 模型或语言变化 | 重建对应转写快照和依赖转写的单元；帧证据可复用 |
| 视觉预算上升 | 补帧并建立新的完整快照；不认为稀疏旧帧足够 |
| 文件内容变化 | 建立新来源版本，旧结果不参与新文件检索 |
| 无变化但构建未完成 | 按清单续建 Failed/Pending 任务，已成功产物复用 |

现有 videoId 使用文件大小和头 1MB hash，不是完整内容身份。首期保留 ID 兼容，但新增文件验证 fingerprint（大小、修改时间及分散采样 hash）；遇到冲突／文件可疑变化应重新验证，严格内容身份可用完整文件 hash。不能仅凭路径或旧 videoId 判定可复用。

### 10.4 旧数据

旧数据统一视为 `legacy_scene_v2`，可以显式提供 legacy 检索视图，不能标成任一种专用策略已经完成。旧原始 SpeechSegment/FrameDesc 在可验证来源、版本和时间后导入快照；其余需要补提取。

类型字符串可以从旧摘要前缀提取作弱提示，但不能作为可靠分类结果。旧 scene_descriptions 保留兼容读取；专用策略构建成功后切换到新的活动构建。

Store 的读接口要同时处理 build 过滤与 legacy fallback，避免 loadVideo() 把历史候选全部加入默认检索。无 active build 但存在失败候选时，只展示已明确发布的原始／legacy 视图。

## 11. 检索、Agent 与工具

### 11.1 检索计划

`QueryPlan` 新增活动 build/revision、preferredUnitKinds、expandSources、expandParent、expandNeighbors 和各类证据预算。`Constraints` 的显式时间约束仍优先。

Retriever 从 store 的当前 build context 获取 profile 与 retrieval policy，不发额外分类网络请求。问题需求优先于类型默认偏好：会议里问 PPT 数字仍检索文字／视觉；课程里问原话仍优先原始转写。

```text
固定查询版本
  → 原始文本 dense + 词面 + 单元摘要／事实 + 视觉／实体召回
  → RRF + 策略与问题的证据偏好
  → 单元／来源／必要邻居展开
  → 时间与 token 预算裁剪
  → 证据充分性判断
```

同一话题内多条短事实可以共享时间范围，现有“相同 chunkType + 时间重叠 > 0.7”去重规则不能继续全部删除。新去重键包含 unit ID、fact ID／来源 ID 和 role；时间重叠仅用于候选分组。

重复的 ASR 原段、合并窗口和单元摘要不算三份独立互证；维持当前的来源依赖排除，并沿 source links 扩展到新产物。

### 11.2 充分性与补充感知

Meeting 问决策责任人：检查决策事实是否有明确责任人来源；Lecture 问公式：需要清晰文字或原始发言；Tutorial 问按钮位置：需要相应状态帧；动作或计数问题：检查实际时间采样覆盖，再决定局部密集取证。

`needsLocalVerification` 必须由 Agent 的执行路径消费。复核结果保存为活动 build 关联的补充证据，升级 revision 并失效依赖旧证据的缓存；不因一次局部复核把整个视频声明为完整 L2。

### 11.3 工具与上下文

- `SearchVideoContentTool`：描述改为支持画面、台词、知识点、议题、步骤等；返回 chunk_id、unit_id、unit_kind、build_id、source IDs，保留 timestamp_ms/end_ms 等现有字段。新增可选 unit_kind/expand_context。
- 新增 `GetSemanticUnitTool`：按 unit ID 获取受预算约束的摘要、结构事实及原始证据；unit ID 必须绑定当前 video/build。
- `GetSceneInfoTool` 保留镜头语义，不把它改成议题查询。
- `GetTranscriptTool` 按活动 raw snapshot 读取、时间升序输出，统一半开区间重叠判断。
- `VideoContext` 新增 contentType、strategyLabel、unitOverview、buildId、revision 与 completeness；`sceneOverview` 保留。
- `ContextBudgetManager` 注入语义概览和有限证据；不把所有单元或整个转写放进 system prompt。

### 11.4 QA 与工作流

`QACacheManager::CachedAnswer` 新增 evidenceChunkIds、evidenceUnitIds、rawSnapshotId、buildId、buildRevision；`evidenceSceneIds` 只作为旧版兼容。缓存命中前校验来源存在和构建版本，策略切换／证据更新失效旧结论。

修改 `VideoAgent` 的 evidenceSceneIds 提取逻辑及缓存调用，同时修改 `DIContainer` 注入的 workflow 检索处理器。当前 workflow 是优先执行路径，只改 VideoAgent 普通路径不会完整生效。

workflow state/checkpoint 保存策略与版本；恢复时发现不匹配则重新检索，不复用旧充分性或旧证据。所有查询入口都通过同一个 Retriever 与 evidence composer，避免策略行为分叉。

## 12. UI 与依赖装配

`VideoAnalysisViewModel` 新增 contentProfileChanged、semanticUnitsReady、unitAnalyzed、buildStateChanged；信号携带 videoId/buildId 或 ViewModel 在转发前校验，避免旧视频回调写入当前页面。

时间线默认展示当前策略的单元名称：会议议题、课程知识点、教程步骤、叙事事件；保留镜头视图。总结页按对应 schema 展示，并可跳回证据。类型标签从 profile 读取，不再依赖摘要文本前缀正则；旧摘要仍兼容展示。

首期只增加一个“内容类型／构建策略”选择入口、自动识别依据与“按此类型重新构建”。保留原始选择和覆盖记录，不把内部模型／索引字段塞进正常阅读流程。

`KnowledgeViewModel::buildSummary()` 从活动清单读取状态，不能以存在 SceneSummary 判断 L2。ChunkBrowser 增加新类型和 unit 信息；用户清空索引时同时清理清单、快照、单元、关系、QA 和对应帧目录，区别于构建时的增量替换。

`DIContainer` 装配顺序：现有模型与 store → probe/classifier/registry → indexer 与 analysis → coordinator → 将 coordinator 注入 analysis 门面 → retriever / Agent / ViewModel。销毁前先停止 coordinator，取消模型与 worker，再销毁执行器和 store。

当前 CMake 使用 src 的 GLOB_RECURSE CONFIGURE_DEPENDS，新 cpp/header 会被纳入；若引入测试则新增独立测试 target，新增外部模型库不应无条件变成主程序依赖。

## 13. 降级、精度与成本

| 条件 | 行为 |
|---|---|
| 无 Whisper／ASR 失败 | 明确缺少语音证据，会议／课程结果 Partial；可以提供视觉与文字索引，不能伪装完整纪要 |
| 无 CLIP/BGE | 保留原始内容及词面搜索；报告向量能力缺失 |
| 无专用 OCR | 使用 VLM 可见文字，记录来源／不清晰项 |
| 无说话人分离 | 发言可检索，说话人未知，不能完成身份归因 |
| 分类超时／JSON 不合法 | 有界重试后 Generic，保存失败原因，可后续纠正 |
| 单元分析部分失败 | 保留原始检索，按 unit/page 续建，摘要注明未分析范围 |
| 无音轨 | 视觉／文字策略构建；不因 audioSummary 为空认为任务失败 |

模型调用预算按单元、时间与 token 计算，优先处理必需产物。长视频分块解码／ASR，避免完整 PCM 常驻；单次 Whisper 运行的取消粒度需明确，现有 API 没有中断回调，第一版可以丢弃过期结果，随后支持分块取消。

镜头采样与 VLM 采样预算分开。现有最多 600／200 帧的全片采样上限会随时长降低分辨率；改为分块扫描与局部预算，并记录精度。TransNetV2 输入应遵循所用模型的连续帧契约，不能把“1 fps”注释当作精确镜头检测保证。

镜头时间范围首尾必须覆盖真实 `[0, durationMs)`，不能继续使用首个采样点为起点、末帧时间 +1000 为视频终点。稀疏采样发现的边界是候选区间，精确定位需要局部复核。

## 14. 实施阶段与验收

### P0：结构和版本基础

修改共享模型、类型枚举、store/migration、原始快照恢复、构建上下文；移除预先全量失效行为。Generic 首先通过新编排器跑通，消除旧 L1 自动启动 L2 的第二入口。

验收：首次、重复打开、重启恢复行为一致；取消／失败不会破坏活动索引；新旧候选不混检索；类型和能力状态可恢复。

### P1：优先解决当前问题，完成语音类型闭环

完成 probe/classifier/router、Lecture 与 Dialogue presets；主题分段、完整分页分析、单元摘要、检索来源展开；更新时间线和 QA 版本校验。

验收：同镜头内多个知识点／议题能分开检索；多镜头同一话题可以合并；长场景后半段的内容进入单元与全局摘要；Meeting 和 Interview 生成不同结构。

### P2：操作教程闭环

实现状态变化候选、步骤取帧、Step schema、前后步骤关联、教程检索。没有采到的点击过程显示证据不足，能够通过 Level 3 补帧。

验收：能检索操作目标、步骤与结果；“下一步”沿 Step 关系展开；准确界面问题使用对应帧／文字证据。

### P3：叙事、局部策略与增强提取器

实现 Narrative 事件合并、混合视频子策略；按实际需求接入 OCR、说话人分离或非语音音频事件模型。每项能力单独建立版本与评测，不要求一次全部上线。

### 必需验证

| 场景 | 检查点 |
|---|---|
| 类型映射及用户覆盖 | 旧标签兼容、未知回退、选择策略先于正式分段 |
| 长静态课程 | 多知识点分段；后半段召回及摘要覆盖；无静默截断 |
| 高频切镜访谈 | 问答跨镜头保留；不自动生成会议决策／待办 |
| 缺失说话人服务 | speaker unknown；不得输出确定身份 |
| 教程与缺失过程帧 | 前后状态可取证；未看到动作不补造 |
| 起止及边界 | 首尾覆盖、绝对 ASR 偏移、半开区间、合法 evidence IDs |
| 超长文本／embedding | 实际 token 限制、子证据拆分和父单元展开 |
| 数据库迁移／重启 | 迁移幂等；帧 PTS、单元、状态及 profile 可恢复 |
| 同类型重建／切换类型／切换视频 | 旧回调丢弃、active build 不污染、QA/checkpoint 失效 |
| 持久化失败 | 事务回滚；内存与 DB 一致；旧 build 保持可读 |
| 多条同时间事实 | 去重不丢失不同 fact；派生证据不重复计作互证 |
| 有／无 ONNX 和 Whisper 宏 | 能编译，缺失能力显示明确降级 |
| workflow 与普通 Agent | 检索计划、证据展开和版本校验一致 |

当前仓库没有独立 tests 目录。实施时为策略路由、分段校验、存储迁移、取消隔离及检索展开增加小规模 QtTest 集成／单元测试；模型返回使用 fixture。端到端用固定样本与标注问题检查真实效果。

对照组：当前流程、仅前移类型并换 Prompt、完整策略流程。各类型分别统计证据 Recall@K、时间定位误差、知识／议题覆盖、来源支持率、未支持结论、构建耗时和模型成本。质量门槛在固定数据集上确定，本文不宣称尚未验证的提升比例。

## 15. 首个可交付范围

第一批应形成一条完整可见链路：

```text
讲课／会议／访谈视频
  → 多位置轻量探测并选择策略
  → ASR 优先提取
  → 知识点／议题／问答分段
  → 完整分页理解与原始引用
  → 单元检索、全局摘要与语义时间线
  → 类型纠正可增量重建，旧结果不混用
```

交付判据是视频类型实质改变构建结果、后半段内容不再因 Scene 限制丢失、回答可以回到原始证据，同时缓存恢复和取消安全。专用 OCR、真实说话人身份识别、动作模型与动态插件注册可以在这条闭环验证后增加。
