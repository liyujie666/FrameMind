# 面试文档二：多模态分析——入库 Pipeline 与带时间戳的多帧 VLM 分析

> 对应简历条目：**"构建取帧、镜头/事件切分、ASR、OCR 和视觉 Embedding Pipeline；针对 Shot 选取带时间戳的多帧网格，通过一次 VLM 调用分析人物、物体、动作、屏幕文字及状态变化，兼顾时序覆盖与推理成本。"**
>
> 涉及代码：`src/service/agent/video_analysis_service.cpp/.h`、`src/service/rag/audio_visual_aligner.cpp/.h`、`src/service/clip_service.cpp`、`src/service/embedding_service.cpp`、`src/service/whisper_service.cpp`、`src/service/rag/speech_segmenter.h`

---

## 一、30 秒电梯话术（背熟）

> 入库 Pipeline 是：批量取帧 → 镜头切分 → 代表帧 CLIP 批量编码 → Whisper 转写并按语义分段 → 按需对场景做多帧 VLM 描述 → 音视频语义门控融合。VLM 分析不是逐帧调用：一个场景的多张带时间戳的代表帧拼成一次调用，让模型同时看到前、中、后状态，理解变化过程——这是在普通 VLM（非时序模型）上成本和时序覆盖的最优折中。另一个核心设计是多模态证据隔离：纯视觉描述、音频摘要、原始台词、可见文字、融合推论各自独立成 chunk 入库，带 evidence_type 标签，绝不互相覆盖——因为 ASR 说"这是小王"不能证明画面里的人就是小王，融合推断更不能冒充视觉事实。

---

## 二、理论基础

### 2.1 多帧网格：把时间序列编码为空间布局

对含 n 张候选帧的片段，三种策略对比：

| 策略 | 成本 | 时序覆盖 | 问题 |
|---|---|---|---|
| 逐帧调 VLM | O(n) | 每次只见局部 | 贵；模型看不到跨帧变化 |
| 只看首帧 | O(1) | 无 | 时序信息全部丢失 |
| **多帧网格** | **≈O(1)** | **前/中/后状态** | 非严格时序建模 |

数学本质：`[f_t1, f_t2, ..., f_tk] ⇒ Temporal Grid`。

每帧烧录时间戳后，VLM 能在一次调用中观察"前—中—后"状态并描述**变化过程**（谁移动了、字幕换了、场景过渡了）。这不是严格的视频时序模型，但对普通 VLM 是成本、覆盖度、实现难度之间最实用的折中——面试要主动承认这个 trade-off，体现工程判断力。

### 2.2 模态不等价原则

不同模态描述的是**不同的事实**：

| 模态 | 表达的事实 |
|---|---|
| 视觉 | 画面中实际看到了什么 |
| ASR | 音轨中说了什么 |
| OCR/可见文字 | 画面上的文字是什么 |
| 融合描述 | 模型综合上述信息后的解释 |

反例（面试好用）：
- ASR 说"这是小王"，不能证明画面中的人就是小王（可能画外音介绍）；
- 字幕出现某句话，不一定代表画面人物说的（可能是旁白）；
- 视觉模型说"一个人在开会"，不代表它看清了屏幕上的 PPT 内容。

因此证据模型应为：

```
E = {E_v, E_a, E_o, E_f}
E_v 视觉事实 / E_a 音频事实 / E_o OCR事实 / E_f 融合推论
```

**推论**：一种模态永远不能覆盖另一种模态的存储——融合描述写入时，纯视觉描述必须原样保留。

### 2.3 Caption 是有损投影

`Caption = g(Frame)`——caption 只保留模型生成时"认为值得描述"的信息，通常丢掉：小目标、精确颜色/数量/姿态、空间位置、细小文字、提问时才重要的细节。

**推论**（衔接第 4 份文档）：检索命中的原始帧必须在生成阶段回注 VLM，caption 只是索引用的投影，不是答案的依据。

### 2.4 中文图文检索的模型选型

- OpenAI CLIP 中文对齐差：中文 query 的"以文搜图"召回质量低；
- 选型结论：视觉图文对齐用中文对齐模型（Chinese-CLIP / 多语 SigLIP）；BGE 只做纯文本语义检索；
- **两者语义空间不同，不能互相替代**（BGE 不能替代 CLIP 做图文检索，反之亦然）。

---

## 三、Pipeline 全景

```
视频打开（VideoIndexer.startIndex）
  │
  ├─ FrameExtractor 批量取帧（文档一已详述）
  │
  ├─ SceneDetector 场景切分（直方图 / TransNetV2）
  │
  ├─ [L1] 视觉编码（ClipService）
  │     收集全部场景代表帧 → encodeImages() 批量推理
  │     → frameEmbedding 挂回 (sceneIndex, frameIndex)
  │
  ├─ [L1] 语音（WhisperService + SpeechSegmenter）
  │     FFmpeg 提取 16kHz mono PCM → whisper.cpp 转写
  │     → 语义分段（带 start/end 时间戳）→ BGE 编码
  │
  ├─ [L2 按需] VLM 场景描述（VideoAnalysisService.describeScene）
  │     多张代表帧（带时间戳）+ 同期 ASR → 一次 VLM 调用
  │     → 结构化输出：摘要/实体/动作/可见文字/不确定项
  │
  └─ [L2 按需] 音视频融合（fuseSceneAudio）
        AudioVisualAligner 时间对齐 → 语义门控 → 保守融合
        → 三/四类证据分离写入 RAG Store
```

---

## 四、代码实现详解

### 4.1 L1 视觉编码：批量推理（不是逐帧调用）

```cpp
// video_indexer.cpp buildLevel1（简化）
std::vector<QImage> representativeImages;
QVector<QPair<int,int>> frameReferences; // (sceneIndex, representativeFrameIndex)
for (int sceneIndex = 0; sceneIndex < repr->scenes.size(); ++sceneIndex) {
    // keyframe 兜底：representativeFrames 为空时用旧 keyframe 补位
    if (scene.representativeFrames.isEmpty() && !scene.keyframe.isNull()) { ... }
    for (frameIndex : ...) {
        representativeImages.push_back(frame.image);
        frameReferences.append({sceneIndex, frameIndex});
    }
}
// 一次 ONNX 推理编码全部代表帧（batch）
const auto embeddings = m_clip->encodeImages(representativeImages);
// 向量按 frameReferences 挂回对应场景的对应帧
```

要点：
- **批量编码**：一次推理吃 N 帧，摊薄预处理和调度开销；
- **向后兼容**：旧数据只有单 keyframe 时兜底转成 representativeFrame；
- 每个阶段开头 `isTaskCurrent()` 校验，支持取消。

### 4.2 L2 多帧 VLM 场景描述

`VideoAnalysisService::describeScene` 的输入组装：

- 该场景的**多张代表帧**（首/中/尾，各带时间戳）；
- **同期 ASR 文本**（AudioVisualAligner 对齐，见 4.3）；
- 结构化 prompt，要求输出区分"确定看到的"和"推测的"，实体描述要具体到可辨识（"穿红色外套的短发女性"而非"一个人"），画面文字准确转录，多帧间有变化要描述动态过程。

输出结构（设计文档中的 prompt 约定）：

```json
{
  "summary": "一句话概括片段核心内容",
  "entities": [{"id": "...", "type": "person|object|text", "description": "..."}],
  "actions": ["切菜", "翻炒"],
  "setting": "现代风格厨房，白天，自然光",
  "visibleTexts": ["屏幕上出现的可见文字"],
  "temporal_cues": "帧间变化/事件顺序"
}
```

### 4.3 音视频对齐（`AudioVisualAligner`）

**问题**：Whisper 转写出的语音段时间轴和场景时间轴是两套独立切分，怎么对齐？

```cpp
// overlappingSpeechSegments(): 场景 ↔ 语音段的时间对齐
// 1) 收集所有与场景有重叠的语音段
// 2) 按"重叠时长/语音段时长"降序排序
//    —— 优先保留主要落在本场景内的段，
//       避免一条跨场景的长语音挤占本场景配额
// 3) 配额控制：maxSegments 条数上限 + maxChars 字符上限
//    一条都收不下时截断保留一条（保证不空）
// 4) 恢复时间升序输出
```

**关键词提取（中文 2-gram）**——用于语义门控的字面交集估算：

```cpp
// extractKeywords(): 中文没有空格分词，用 2-gram 近似；
// 英文/数字按连续字母数字切词。
// 配合停用词表（的/了/是/画面/场景/镜头...），避免虚词拉高交集
```

这里可以讲一个务实判断：**不需要真正的分词器**——目的只是估算"人物/物体/地点/动作"层面的字面交集，2-gram + 停用词已经够用，零外部依赖。

**语义门控 + 保守融合**（`gate()` + `fuseSceneAudio()`）：
- 判定同期 ASR 与画面内容是否语义相关（`AudioVisualRelation`：Related / Independent / Unknown）；
- **只有判定相关才融合**；无法判定关系时保守地不融合——宁可少融合，也不错融合；
- 关系写入 chunk metadata（`audio_relation`），供下游检索互证逻辑使用。

### 4.4 证据分离入库（本模块最核心的设计）

```cpp
// video_analysis_service.cpp —— writeSceneEvidence 调用处
// 三类证据分别入库，融合描述不覆盖纯视觉事实
writeSceneEvidence(fusion, repr, VideoChunk::SceneSummary,
                   "visual", fusion.visualDescription);      // 纯视觉
if (!fusion.audioSummary.isEmpty()) {
    writeSceneEvidence(fusion, repr, VideoChunk::SceneAudio,
                       "audio", fusion.audioSummary);         // 音频摘要
}
// 可见文字独立入库，供字幕、招牌、PPT 等精确文字问题走文本检索
writeSceneEvidence(..., "visible_text",
                   scene.visibleTexts.join("\n"));            // OCR/可见文字
// 无音频时融合描述与视觉描述相同，不重复占用一条 chunk
if (fusion.hasAudio() && fusion.fusedDescription != fusion.visualDescription) {
    writeSceneEvidence(fusion, repr, VideoChunk::SceneFused,
                       "fused", fusion.fusedDescription);     // 融合推论
}
```

每条证据的 metadata 携带：`scene_id / keyframe_ms / file_path / evidence_type / audio_relation / embedding_model_id / embedding_version`。

**为什么"可见文字"单独一路**：错误模式不同。可见文字是**精确文本**（标题、型号、数字），视觉描述是语义概览。"招牌上写了什么"需要词面精确匹配（衔接文档三的 lexical 召回），混进 caption 里基本检索不到。当前来源是 VLM 可见文字转录，专用 OCR 模型可作为同类型补充来源接入。

### 4.5 模型身份绑定（与文档三的存储校验呼应）

每条 chunk 的 embedding 都带模型身份：

```cpp
// 写入侧（video_rag_store.cpp upsertEntity 示例）
chunk.metadata.insert("embedding_model_id", "bge_text");
chunk.metadata.insert("embedding_version", "passage_v2");
```

检索侧 Filter 校验 `expectedEmbeddingModelId/Version`——**向量空间由模型身份定义，而非维度**。512 维 CLIP 和 512 维 BGE 不能比余弦。

---

## 五、真实工程问题（war stories）

### 问题 1：融合描述污染纯视觉事实（最重要）

**现象**：早期把 VLM 描述和 ASR 内容拼成一条"场景描述"。台词里的信息（"这是小王"）被当成了画面事实；用户问"画面里有什么"时，答案被音频内容误导。
**根因**：不同模态的证据在存储层被合并，来源信息丢失。
**解决**：拆成 visual / audio / visible_text / fused 独立 chunk，各自带 `evidence_type`；检索阶段按问题类型对证据类型加权（文档三的 `evidenceWeight`），生成阶段证据包带来源标签。
**方法论**：**融合发生在消费侧（检索加权、生成时综合），不发生在存储侧**。

### 问题 2：音视频盲目融合的"张冠李戴"

**现象**：背景音乐歌词、画外旁白被融合进场景描述，产生"画面中的人在唱歌"这类错误关联。
**解决**：
1. 语义门控：关键词交集 + 停用词过滤判定相关性，Unknown/Independent 保持分离；
2. 跨模态互证逻辑**排除同源转写**（两条都来自 whisper 的证据不算互证，`isDerivedFromSameTranscript`）；
3. `audio_relation` 元数据让下游始终知道音频证据的独立性等级。

### 问题 3：VLM oneShot 通道与用户对话互相干扰

**现象**：场景描述生成初期复用了用户对话的 AgentService 实例和会话状态——索引期间用户发消息，系统 prompt 和场景分析上下文串了。
**解决方向**：独立通道（独立 AgentService 实例或直连 NetworkClient）。
**面试话术**：这是我踩坑后明确的改造项——后台批处理和前台交互必须做会话/状态隔离，这个教训让我对所有"共享服务实例"的设计都保持警惕。

### 问题 4：中文 CLIP 召回质量差

**现象**：中文 query 的以文搜图命中率明显低于英文。
**定位**：OpenAI CLIP 训练数据以英文为主，中文对齐弱。
**方案**：中文场景换 Chinese-CLIP / 多语 SigLIP 做图文对齐；BGE 专职文本语义检索。**同时强调**：换模型必须同步更新 embedding 的 model_id/version 元数据，否则新旧向量混在一个索引里会静默产生错误相似度（这正是文档三模型身份校验存在的原因——一次踩坑催生一个正确性机制，这种叙事在面试里非常加分）。

### 问题 5：Whisper 全量转写慢

**缓解**：whisper.cpp + small/base 模型 + INT8 量化；后台线程执行不阻塞 L0/L1 可用性；转写结果持久化，二次打开零成本。

---

## 六、面试官可能问的问题

| 问题 | 回答要点 |
|---|---|
| **多帧网格和真时序模型（VideoLLaMA 等）的差距？** | 网格非严格时序建模：帧间精确运动、快速动作可能丢。但普通 VLM 上是成本/覆盖最优折中；上时序模型时取帧和分层索引底座不变，只换 L2 分析器——架构预留了替换点。 |
| **多帧是几张？怎么选？** | 场景内首/中/尾 ≤3 张（当前）；理论建议 4~9 张覆盖更细。选择策略：均匀、场景边界、OCR 变化处、运动剧烈处加密——当前选了最稳的首中尾，加密采样是演进方向。 |
| **为什么 OCR 单独一路？** | 错误模式和用途不同：精确文本需要词面匹配（lexical 召回），语义概览需要向量。混进 caption 检索不到。 |
| **音视频怎么对齐的？** | 时间区间重叠对齐，按重叠占比排序配额控制；语义门控决定是否融合；关系元数据贯穿到检索互证。 |
| **语义门控怎么实现的？** | 2-gram 关键词交集 + 停用词表估算字面相关性；判定不了就保守不融合。坦诚：这是启发式，生产版可换 NLI 模型或 embedding 相似度。 |
| **为什么 CLIP 和 BGE 不能混用？** | 不同语义空间：维度相同不代表坐标轴含义一致，余弦相似度无意义。存储层用 model_id + version 做硬校验。 |
| **VLM 调用成本怎么控制？** | 三层：L2 按需触发（不问不分析）；多帧合并单次调用；QA 缓存复用历史结论（0.88 阈值）。 |
| **描述质量不稳定怎么办？** | 结构化 prompt 约束（区分确定/推测、实体可辨识、文字转录准确）；不确定项显式输出；反思阶段把无证据支撑断言标为疑似幻觉。 |
| **Whisper 时间戳误差？** | 语义分段而非逐句独立检索；检索约束用区间 Overlaps 模式容忍边界抖动。 |

---

## 七、白板演练：一个场景的完整入库

```
场景 7：[02:10 - 02:35]，代表帧 3 张（02:10 / 02:22 / 02:34）

[L1]
  CLIP encodeImages([f1,f2,f3]) → 3 个 512 维向量
  Whisper 转写 → 语音段 [02:12-02:20]"接下来看这个配置文件"
                        [02:25-02:30]"注意第三行"

[L2 按需触发]
  describeScene:
    输入 = 3 帧（带时间戳）+ 同期 ASR
    输出 = {
      summary: "演讲者打开配置文件并讲解",
      visibleTexts: ["server.port=8080"],
      entities: [{type:person, desc:"穿蓝衬衫的男性"}],
      actions: ["指向屏幕", "翻页"]
    }

  fuseSceneAudio:
    对齐: 两条语音段均与场景重叠
    门控: "配置文件/第三行" 与画面内容相关 → Related
    融合: fusedDescription 生成

  入库（4 条独立 chunk）:
    SceneSummary   evidence_type=visual        "演讲者打开配置文件..."
    SceneAudio     evidence_type=audio         "音频摘要: 讲解配置..."
    (可见文字)      evidence_type=visible_text  "server.port=8080"
    SceneFused     evidence_type=fused         融合描述
    每条都带: scene_id=7 / 时间区间 / embedding_model_id
```
