# 视频分析智能体核心设计 — 如何让模型真正理解视频

## 一、核心问题：为什么视频理解难？

### 1.1 视频 ≠ 图片集合

视频理解的本质困难在于：

| 维度 | 图片理解 | 视频理解 |
|------|---------|---------|
| 信息量 | 单帧，静态 | 数千~数十万帧，时序动态 |
| 语义 | 空间语义（"有什么"） | 时空语义（"发生了什么"→"为什么"→"接下来会怎样"） |
| 因果 | 无 | 事件因果链（A 导致 B） |
| 上下文窗口 | 1 张图 ≈ 几百 token | 1 分钟视频 ≈ 1800 帧 ≈ 数十万 token（远超任何模型） |
| 冗余度 | 低 | 极高（相邻帧 95%+ 相似） |

**核心矛盾**：模型上下文有限 vs 视频信息量巨大。

### 1.2 解题思路

人类看视频的认知过程：
1. **先扫一遍** → 形成整体印象（结构感知）
2. **按需细看** → 对感兴趣的片段仔细观察（选择性注意）
3. **建立记忆** → 关键事件、人物关系存入脑中（结构化记忆）
4. **被问到时** → 回忆 + 必要时回看确认（检索 + 验证）

智能体应模拟这个过程，而不是暴力把所有帧塞给模型。

---

## 二、视频理解的分层表示体系

### 2.1 三层表示金字塔

```
          ┌─────────────┐
          │  Semantic    │  ← 自然语言描述 + 知识图谱
          │  语义层       │     "男人从桌上拿起杯子递给女人"
          ├─────────────┤
          │  Structural  │  ← 场景/片段/关键帧分割
          │  结构层       │     场景边界 + 时间区间 + 实体列表
          ├─────────────┤
          │  Perceptual  │  ← 原始感知信号
          │  感知层       │     帧像素 + 音频波形 + 光流
          └─────────────┘
```

**设计原则**：智能体日常工作在语义层和结构层，仅在必要时下探到感知层。

### 2.2 各层数据结构

```
感知层 (Perceptual):
├── 原始帧数据 (通过 PlayerSDK 按需获取)
├── 帧级视觉 embedding (CLIP/SigLIP, 768-d vector)
├── 音频 embedding (CLAP/Whisper encoder)
└── 光流/运动向量 (场景切换检测用)

结构层 (Structural):
├── SceneGraph: [{scene_id, start_ms, end_ms, keyframe_indices}]
├── EntityRegistry: [{entity_id, type, first_appear, last_appear, visual_descriptor}]
├── SpeechSegments: [{start_ms, end_ms, speaker_id, text}]
└── TemporalIndex: 时间 → 场景/实体/语音的倒排索引

语义层 (Semantic):
├── VideoSummary: 全视频自然语言摘要
├── SceneDescriptions: [{scene_id, description, key_events}]
├── EventChain: [{event, timestamp, cause, effect, participants}]
├── EntityProfiles: [{entity_id, description, actions, relations}]
└── QA_Cache: 历史问答对 (避免重复分析)
```

---

## 三、智能体核心决策引擎

### 3.1 Agent 决策循环（不是简单的 ReAct）

```
┌──────────────────────────────────────────────────────────────────┐
│                    Agent Decision Engine                          │
│                                                                  │
│  ┌──────────┐    ┌───────────┐    ┌──────────┐    ┌──────────┐ │
│  │  PERCEIVE │───▶│ REPRESENT │───▶│  REASON  │───▶│   ACT    │ │
│  │  感知采集  │    │ 表示构建   │    │  推理决策  │    │  执行动作 │ │
│  └──────────┘    └───────────┘    └──────────┘    └──────────┘ │
│       ▲                                                │         │
│       │              ┌──────────┐                      │         │
│       └──────────────│ REFLECT  │◀─────────────────────┘         │
│                      │ 反思验证  │                                │
│                      └──────────┘                                │
│                                                                  │
│  ┌─── Memory Store ────────────────────────────────────────────┐ │
│  │ Working Memory │ Video Representation │ Conversation History │ │
│  └─────────────────────────────────────────────────────────────┘ │
└──────────────────────────────────────────────────────────────────┘
```

### 3.2 各阶段详细设计

#### PERCEIVE（感知采集）— 决定"看哪里"

这是最关键的一步：**不是看得多就好，而是看得准。**

```python
class PerceptionStrategy:
    """
    感知策略：根据问题类型决定采样方案
    核心思想：先粗后细，按需下探
    """

    def decide_sampling(self, question: str, video_repr: VideoRepresentation) -> SamplingPlan:
        """
        根据用户问题 + 已有视频表示，决定需要额外感知什么
        """
        # Step 1: 问题分类
        q_type = self.classify_question(question)

        # Step 2: 判断已有信息是否足够回答
        sufficiency = self.check_information_sufficiency(question, video_repr)
        if sufficiency.is_enough:
            return SamplingPlan(need_new_perception=False)

        # Step 3: 根据问题类型生成采样计划
        if q_type == QuestionType.GLOBAL_SUMMARY:
            # 全局问题 → 均匀稀疏采样（如果还没有摘要）
            return self.plan_uniform_sampling(video_repr, density="sparse")

        elif q_type == QuestionType.TEMPORAL_LOCALIZATION:
            # "什么时候..." → 先用文本检索定位候选区间，再对候选区间密采
            candidates = self.search_by_text(question, video_repr)
            return self.plan_targeted_sampling(candidates, density="dense")

        elif q_type == QuestionType.ENTITY_QUERY:
            # "那个人/物体..." → 定位实体出现的时间区间
            entity_ref = self.resolve_entity_reference(question, video_repr)
            return self.plan_entity_tracking(entity_ref)

        elif q_type == QuestionType.CAUSAL_REASONING:
            # "为什么..." → 定位事件前后的上下文
            event = self.locate_event(question, video_repr)
            return self.plan_causal_context(event, before_sec=10, after_sec=5)

        elif q_type == QuestionType.CURRENT_FRAME:
            # "当前画面..." → 只需当前帧（或当前帧附近几帧做动作理解）
            return self.plan_current_frame_context(window_sec=3)

        elif q_type == QuestionType.COMPARISON:
            # "A和B有什么区别..." → 定位A和B各自的区间
            segments = self.locate_comparison_targets(question, video_repr)
            return self.plan_comparison_sampling(segments)

        elif q_type == QuestionType.COUNTING:
            # "出现了几次..." → 全量检索 + 去重
            return self.plan_exhaustive_search(question, video_repr)

        else:
            # 兜底：中等密度采样
            return self.plan_uniform_sampling(video_repr, density="medium")
```

**问题类型分类体系**：

```
QuestionType:
├── GLOBAL_SUMMARY        "这个视频讲了什么" "总结一下"
├── TEMPORAL_LOCALIZATION  "什么时候出现了X" "找到X的片段"
├── ENTITY_QUERY          "那个红衣服的人是谁" "桌上有什么"
├── SPATIAL_QUERY         "左边是什么" "画面右上角"
├── ACTION_RECOGNITION    "他在做什么" "发生了什么动作"
├── CAUSAL_REASONING      "为什么..." "是什么导致了..."
├── COUNTERFACTUAL        "如果...会怎样"
├── COMPARISON            "A和B有什么不同"
├── COUNTING              "出现了几次" "有几个人"
├── CURRENT_FRAME         "现在画面里有什么"（依赖播放器当前位置）
├── TEMPORAL_ORDER        "A是在B之前还是之后"
├── DURATION              "这个动作持续了多久"
└── DETAIL_DESCRIPTION    "详细描述一下这个场景"
```

#### REPRESENT（表示构建）— 把视觉信号变成模型能理解的语言

**核心洞察**：大模型本质上是语言模型。即使是多模态模型，其推理能力主要靠语言链。因此，最有效的方式是把视频信息转化为结构化的自然语言表示，再供推理使用。

```python
class VideoRepresentationBuilder:
    """
    视频表示构建器：将原始帧转换为多层结构化表示
    """

    def build_initial_representation(self, video_path: str) -> VideoRepresentation:
        """
        视频首次加载时的初始建模（轻量级，秒级完成）
        """
        repr = VideoRepresentation(video_path)

        # Step 1: 元信息（从 PlayerSDK 获取，零开销）
        repr.metadata = self.extract_metadata(video_path)
        # → {duration, fps, resolution, codec, has_audio, file_size}

        # Step 2: 场景分割（基于视觉差异，不需要大模型）
        # 使用帧间直方图差异 / TransNetV2 轻量模型
        repr.scenes = self.detect_scene_boundaries(video_path)
        # → [{scene_id: 0, start_ms: 0, end_ms: 15000}, ...]

        # Step 3: 关键帧提取（每个场景取1-2帧代表帧）
        repr.keyframes = self.extract_keyframes(repr.scenes)
        # → [{scene_id: 0, timestamp_ms: 7500, frame_data: ...}, ...]

        # Step 4: 关键帧 embedding（用于后续检索）
        repr.frame_embeddings = self.encode_frames(repr.keyframes)
        # → CLIP/SigLIP 768-d vectors

        # Step 5: 音频转文字（如果有音频）
        if repr.metadata.has_audio:
            repr.speech_segments = self.transcribe_audio(video_path)
            # → [{start_ms, end_ms, text, speaker_id}, ...]

        return repr

    def build_scene_descriptions(self, repr: VideoRepresentation) -> None:
        """
        为每个场景生成自然语言描述（需要VLM，可异步/按需）
        这是让模型"理解"视频的核心步骤
        """
        for scene in repr.scenes:
            keyframes = repr.get_keyframes_for_scene(scene.id)
            speech = repr.get_speech_for_timerange(scene.start_ms, scene.end_ms)

            # 多帧 + 语音文本 → VLM 生成结构化描述
            scene.description = self.vlm_describe_scene(
                frames=keyframes,
                speech_text=speech,
                prompt=SCENE_DESCRIPTION_PROMPT
            )
            # 输出格式:
            # {
            #   "summary": "一个男人在厨房里做饭，背景有音乐",
            #   "entities": [{"id": "person_1", "desc": "穿蓝色围裙的中年男性"}],
            #   "actions": ["切菜", "翻炒"],
            #   "setting": "现代风格厨房，白天，自然光",
            #   "mood": "轻松愉快",
            #   "objects": ["菜刀", "炒锅", "蔬菜"]
            # }

    def build_video_summary(self, repr: VideoRepresentation) -> str:
        """
        基于所有场景描述生成全视频摘要
        """
        scene_descriptions = [s.description for s in repr.scenes]

        summary = self.llm_summarize(
            scene_descriptions=scene_descriptions,
            metadata=repr.metadata,
            speech_full_text=repr.get_full_transcript(),
            prompt=VIDEO_SUMMARY_PROMPT
        )
        repr.summary = summary
        return summary
```

**关键 Prompt 设计 — SCENE_DESCRIPTION_PROMPT**：

```markdown
你是一个专业的视频内容分析师。请根据提供的视频帧序列和对应的语音文本，
生成该片段的结构化描述。

## 输入
- 帧序列: {N}帧画面，时间跨度: {start_time} - {end_time}
- 语音文本: {speech_text}（如有）

## 输出要求（JSON格式）
{
  "summary": "用一句话概括这个片段的核心内容",
  "entities": [
    {"id": "自动编号", "type": "person|object|text|location", 
     "description": "外观特征描述，确保足以在后续帧中重新识别"}
  ],
  "actions": ["正在发生的动作/事件列表"],
  "setting": "环境/场景描述（地点、时间、光线、氛围）",
  "interactions": ["实体间的交互关系"],
  "camera": "镜头语言描述（近景/远景/运动方向）",
  "temporal_cues": "时间线索（如果能判断事件顺序或因果）"
}

## 注意
1. 描述要客观准确，区分"确定看到的"和"推测的"
2. 实体描述要具体到可辨识（如"穿红色外套的短发女性"而非"一个人"）
3. 如果画面中有文字/字幕/标志，请准确转录
4. 注意帧间变化：如果多帧之间有运动/变化，要描述动态过程
```

#### REASON（推理决策）— Agent 的"大脑"

```python
class ReasoningEngine:
    """
    推理引擎：基于视频表示和用户问题，决定如何作答
    采用 Hierarchical Reasoning（分层推理）
    """

    def reason(self, question: str, video_repr: VideoRepresentation,
               conversation_history: list, perception_results: dict) -> ReasoningResult:
        """
        核心推理流程
        """

        # === Phase 1: 信息整合 ===
        # 将所有已知信息组装为模型可理解的上下文
        context = self.assemble_context(
            question=question,
            video_repr=video_repr,
            perception_results=perception_results,
            conversation_history=conversation_history
        )

        # === Phase 2: 判断是否需要更多信息 ===
        # 让模型自己判断：当前信息是否足以回答？
        sufficiency_check = self.check_sufficiency(context)

        if not sufficiency_check.is_sufficient:
            # 返回需要额外感知的指令
            return ReasoningResult(
                status="need_more_info",
                next_action=sufficiency_check.suggested_action,
                reasoning_trace=sufficiency_check.trace
            )

        # === Phase 3: 生成答案 ===
        answer = self.generate_answer(context)

        # === Phase 4: 置信度评估 ===
        confidence = self.assess_confidence(answer, context)

        if confidence < CONFIDENCE_THRESHOLD:
            # 低置信度 → 尝试验证或补充
            return ReasoningResult(
                status="need_verification",
                preliminary_answer=answer,
                verification_plan=self.plan_verification(answer, context)
            )

        return ReasoningResult(
            status="complete",
            answer=answer,
            confidence=confidence,
            evidence=self.extract_evidence(answer, video_repr)
        )

    def assemble_context(self, question, video_repr, perception_results, 
                         conversation_history) -> str:
        """
        上下文组装 — 这是影响模型理解质量的最关键步骤
        
        原则：
        1. 信息分层：先给概览，再给细节
        2. 相关性排序：与问题最相关的信息排在前面
        3. 控制总量：不超过模型有效注意力范围
        """

        context_parts = []

        # Layer 1: 视频元信息（始终包含）
        context_parts.append(self.format_metadata(video_repr.metadata))
        # → "【视频信息】时长: 12:34, 分辨率: 1920x1080, 帧率: 30fps"

        # Layer 2: 全视频结构概览（始终包含）
        context_parts.append(self.format_structure_overview(video_repr))
        # → "【视频结构】共8个场景: [0:00-0:45 厨房准备食材] [0:45-2:10 烹饪过程] ..."

        # Layer 3: 与问题相关的场景详细描述
        relevant_scenes = self.retrieve_relevant_scenes(question, video_repr)
        context_parts.append(self.format_scene_details(relevant_scenes))
        # → "【相关片段详情】场景3 (0:45-2:10): 男人开始切菜..."

        # Layer 4: 当前感知结果（如果刚做了新的帧分析）
        if perception_results:
            context_parts.append(self.format_perception(perception_results))

        # Layer 5: 语音文本（相关片段的）
        relevant_speech = self.retrieve_relevant_speech(question, video_repr)
        if relevant_speech:
            context_parts.append(self.format_speech(relevant_speech))

        # Layer 6: 对话历史（最近N轮，含之前的分析结论）
        context_parts.append(self.format_conversation(conversation_history, max_turns=5))

        return "\n\n".join(context_parts)
```

**核心 System Prompt 设计**：

```markdown
# 角色
你是一个专业的视频内容分析智能体。你能够理解视频的视觉内容、音频内容、
时间结构和语义关系。

# 能力边界
- 你的视频理解基于结构化的场景描述和关键帧分析，而非直接"看到"所有帧
- 当提供了具体帧图片时，你可以直接分析画面内容
- 当只有文字描述时，你基于这些描述进行推理

# 推理原则
1. **区分确定与推测**：明确标注哪些是直接观察到的事实，哪些是推断
2. **时间意识**：始终关注事件的时间顺序和持续时长
3. **多模态融合**：综合视觉信息和语音信息做判断，注意两者是否一致
4. **主动求证**：如果现有信息不足以确定地回答，说明需要查看哪个时间段的画面

# 回答格式
- 先给出直接答案
- 再给出支撑证据（引用具体时间点或场景）
- 如果不确定，给出置信度和可能需要进一步确认的建议

# 工具使用
当你需要更多信息时，可以请求以下操作：
- SEEK_AND_ANALYZE: 跳转到指定时间点分析画面
- SEARCH_FRAMES: 按描述搜索匹配的帧
- ANALYZE_RANGE: 分析指定时间区间的连续帧
- GET_AUDIO: 获取指定区间的语音转文字
```

#### REFLECT（反思验证）— 保证答案可靠性

```python
class ReflectionEngine:
    """
    反思引擎：验证推理结果的可靠性
    """

    def reflect(self, answer: str, evidence: list, 
                video_repr: VideoRepresentation) -> ReflectionResult:
        """
        对生成的答案进行自我验证
        """

        checks = []

        # Check 1: 事实一致性 — 答案是否与已知视频信息矛盾
        consistency = self.check_consistency(answer, video_repr)
        checks.append(consistency)

        # Check 2: 证据支撑度 — 答案中的每个断言是否有证据
        support = self.check_evidence_support(answer, evidence)
        checks.append(support)

        # Check 3: 时间合理性 — 涉及的时间点/时长是否在视频范围内
        temporal = self.check_temporal_validity(answer, video_repr.metadata)
        checks.append(temporal)

        # Check 4: 幻觉检测 — 是否包含视频中不存在的内容
        hallucination = self.detect_hallucination(answer, video_repr)
        checks.append(hallucination)

        # 综合判断
        if all(c.passed for c in checks):
            return ReflectionResult(valid=True, confidence=0.9)
        else:
            failed = [c for c in checks if not c.passed]
            return ReflectionResult(
                valid=False,
                issues=failed,
                suggestion=self.suggest_fix(failed, video_repr)
            )

    def detect_hallucination(self, answer: str, video_repr: VideoRepresentation):
        """
        幻觉检测：检查答案是否描述了视频中不存在的内容
        
        策略：
        1. 提取答案中的实体/事件声明
        2. 在视频表示中检索是否有支撑
        3. 对无支撑的声明标记为可能幻觉
        """
        claims = self.extract_claims(answer)
        unsupported = []

        for claim in claims:
            if not self.find_evidence_for_claim(claim, video_repr):
                unsupported.append(claim)

        if unsupported:
            return CheckResult(
                passed=False,
                detail=f"以下断言在视频中未找到支撑: {unsupported}"
            )
        return CheckResult(passed=True)
```

#### ACT（执行动作）— 与播放器和外部工具的交互

```python
class ActionExecutor:
    """
    动作执行器：将推理决策转化为对 PlayerSDK 和分析工具的实际调用
    """

    def execute(self, action: Action, player_service, analysis_service) -> ActionResult:

        if action.type == "SEEK_AND_ANALYZE":
            # 跳转到指定时间并分析
            player_service.seek(action.timestamp_ms)
            frame = player_service.capture_current_frame()
            description = analysis_service.describe_frame(frame, action.focus_query)
            return ActionResult(type="frame_analysis", data=description)

        elif action.type == "ANALYZE_RANGE":
            # 分析一个时间区间（采样多帧）
            frames = self.sample_frames_in_range(
                player_service, action.start_ms, action.end_ms,
                sample_count=action.sample_count or 5
            )
            descriptions = []
            for ts, frame in frames:
                desc = analysis_service.describe_frame(frame, action.focus_query)
                descriptions.append({"timestamp_ms": ts, "description": desc})
            return ActionResult(type="range_analysis", data=descriptions)

        elif action.type == "SEARCH_FRAMES":
            # 语义检索匹配帧
            query_embedding = self.encode_text(action.query)
            matches = self.search_similar_frames(query_embedding, video_repr)
            # 对 top-K 匹配帧做细致分析
            results = []
            for match in matches[:action.top_k]:
                frame = player_service.capture_frame_at(match.timestamp_ms)
                verified = analysis_service.verify_match(frame, action.query)
                if verified:
                    results.append(match)
            return ActionResult(type="search_result", data=results)

        elif action.type == "SEEK_PLAYER":
            # 纯播放器跳转（不分析，只是给用户看）
            player_service.seek(action.timestamp_ms)
            return ActionResult(type="player_seeked", data=action.timestamp_ms)

        elif action.type == "GET_AUDIO":
            # 获取指定区间的音频转文字
            transcript = analysis_service.transcribe_range(
                action.start_ms, action.end_ms
            )
            return ActionResult(type="transcript", data=transcript)
```

---

## 四、自适应帧采样策略（核心算法）

### 4.1 为什么均匀采样不够？

```
均匀采样的问题:
  视频: [====静止画面====][快速动作][====静止画面====][对话场景====]
  均匀: x    x    x    x    x    x    x    x    x    x
                       ↑ 
              关键动作只采到1帧，信息丢失严重

自适应采样:
  视频: [====静止画面====][快速动作][====静止画面====][对话场景====]
  采样: x         x      xxxxx    x         x     x   x   x
                         ↑
                 密集采样，捕获完整动作过程
```

### 4.2 多策略融合采样

```python
class AdaptiveSampler:
    """
    自适应采样器：根据视频内容特征动态调整采样密度
    """

    def compute_sampling_plan(self, video_path: str, purpose: str) -> list[SamplePoint]:
        """
        purpose: "initial_overview" | "question_targeted" | "full_analysis"
        """
        duration_ms = self.get_duration(video_path)

        # Strategy 1: 场景边界强制采样
        scene_boundaries = self.detect_scene_changes(video_path)
        # 每个场景的首帧和中间帧必采

        # Strategy 2: 运动量驱动采样
        motion_scores = self.compute_motion_density(video_path)
        # 高运动区间加密采样

        # Strategy 3: 音频事件驱动采样
        audio_events = self.detect_audio_events(video_path)
        # 语音开始/结束、突发声音处采样

        # Strategy 4: 信息熵驱动（帧间变化量）
        entropy_curve = self.compute_frame_entropy(video_path)
        # 高熵（变化大）区间加密

        # 融合策略
        sample_points = self.merge_strategies(
            scene_boundaries,
            motion_scores,
            audio_events,
            entropy_curve,
            target_frame_count=self.compute_budget(duration_ms, purpose)
        )

        return sample_points

    def compute_budget(self, duration_ms: int, purpose: str) -> int:
        """
        计算采样预算（目标帧数）
        
        原则：
        - initial_overview: 每分钟 2-4 帧（快速建模）
        - question_targeted: 目标区间每秒 1-2 帧
        - full_analysis: 每分钟 6-10 帧（精细）
        """
        duration_min = duration_ms / 60000

        budgets = {
            "initial_overview": max(8, int(duration_min * 3)),    # 至少8帧
            "question_targeted": 10,                              # 目标区间固定10帧
            "full_analysis": max(20, int(duration_min * 8)),      # 最少20帧
        }

        # 上限：考虑模型上下文和API成本
        return min(budgets[purpose], 50)  # 单次最多50帧
```

---

## 五、多轮渐进式理解策略

### 5.1 不要一次性全量分析

```
传统方案（不推荐）:
  视频加载 → 全量分析（耗时数分钟）→ 用户等待 → 才能对话
  问题: 慢、贵、很多分析结果用户根本不会问到

渐进式方案（推荐）:
  视频加载 → 秒级初始建模 → 即可对话 → 按需深入
  
  时间线:
  T=0s    加载元信息 + 场景分割（轻量算法，无需大模型）
  T=1s    可以回答: "视频多长？什么格式？大概有几个场景？"
  T=3s    关键帧 embedding 计算完成
          可以回答: "帮我找到有XX的画面" (语义检索)
  T=5-10s 第一批场景描述生成（前3个场景）
          可以回答: "视频开头讲了什么？"
  按需    用户问到某个具体问题时，再对相关片段深入分析
```

### 5.2 信息逐层细化

```python
class ProgressiveUnderstanding:
    """
    渐进式理解：视频知识从粗到细逐步构建
    """

    # Level 0: 即时可用（视频加载后立即完成）
    async def level_0_instant(self, video_path):
        """元信息 + 结构骨架"""
        metadata = await self.extract_metadata(video_path)  # <100ms
        scenes = await self.detect_scenes(video_path)        # ~1-2s
        return Level0Result(metadata, scenes)

    # Level 1: 快速索引（后台异步，几秒完成）
    async def level_1_indexing(self, video_path, scenes):
        """关键帧提取 + embedding + 音频转文字"""
        keyframes = await self.extract_keyframes(scenes)
        embeddings = await self.encode_keyframes(keyframes)  # CLIP, batch
        transcript = await self.transcribe_audio(video_path) # Whisper
        return Level1Result(keyframes, embeddings, transcript)

    # Level 2: 语义理解（按需触发，每场景约2-3s）
    async def level_2_understanding(self, scene_id, keyframes, transcript_segment):
        """单个场景的详细描述"""
        description = await self.vlm_describe(keyframes, transcript_segment)
        entities = self.extract_entities(description)
        return Level2Result(description, entities)

    # Level 3: 深度分析（仅在用户追问时触发）
    async def level_3_deep_analysis(self, time_range, question):
        """高密度采样 + 多帧联合推理"""
        dense_frames = await self.dense_sample(time_range, fps=2)
        deep_analysis = await self.vlm_analyze_sequence(dense_frames, question)
        return Level3Result(deep_analysis)
```

---

## 六、如何让模型"真正理解"视频

### 6.1 多帧联合推理（而非单帧独立描述）

**错误做法**：每帧独立送模型描述，再拼接。
**正确做法**：多帧一起送，让模型理解帧间关系。

```python
def vlm_analyze_sequence(self, frames: list[tuple[int, QImage]], question: str) -> str:
    """
    多帧联合推理 — 让模型在一次调用中看到多帧并理解时序关系
    """
    messages = [
        {"role": "system", "content": SEQUENCE_ANALYSIS_PROMPT},
        {"role": "user", "content": [
            {"type": "text", "text": f"以下是视频中按时间顺序排列的{len(frames)}帧画面"
                                     f"（时间标注在每帧下方）。\n\n问题: {question}"},
            # 按时间顺序排列帧
            *[
                {"type": "image_url", "image_url": {"url": f"data:image/jpeg;base64,{encode(frame)}"}},
                {"type": "text", "text": f"[{format_time(ts_ms)}]"}
                for ts_ms, frame in frames
            ]
        ]}
    ]
    return self.call_vlm(messages)
```

**SEQUENCE_ANALYSIS_PROMPT**：

```markdown
你正在分析一段视频的连续帧序列。这些帧按时间顺序排列，每帧下方标注了时间戳。

请注意：
1. 这些帧来自同一段连续视频，请理解帧之间的时间关系和运动变化
2. 关注：什么在移动？谁在做什么动作？场景如何变化？
3. 如果帧间有明显变化，描述这个变化过程
4. 如果帧间几乎没有变化，说明是静止/缓慢变化的场景
5. 综合所有帧回答问题，而不是逐帧独立描述
```

### 6.2 视觉-语言-时间三模态对齐

```python
def build_multimodal_context(self, scene, keyframes, speech_segments):
    """
    构建多模态对齐上下文：视觉+语言+时间 三位一体

    关键：将同一时间点的视觉和语音对齐呈现给模型
    """
    context = f"## 片段 [{format_time(scene.start_ms)} - {format_time(scene.end_ms)}]\n\n"

    # 按时间顺序交错排列视觉和语音信息
    timeline = []

    for kf in keyframes:
        timeline.append({
            "time": kf.timestamp_ms,
            "type": "visual",
            "data": kf
        })

    for seg in speech_segments:
        timeline.append({
            "time": seg.start_ms,
            "type": "speech",
            "data": seg
        })

    # 按时间排序
    timeline.sort(key=lambda x: x["time"])

    # 生成对齐的上下文
    for item in timeline:
        t = format_time(item["time"])
        if item["type"] == "visual":
            context += f"[{t}] 📷 画面: {item['data'].description}\n"
        elif item["type"] == "speech":
            speaker = item['data'].speaker or "未知"
            context += f"[{t}] 🎤 {speaker}: \"{item['data'].text}\"\n"

    return context
```

### 6.3 实体追踪与共指消解

让模型理解"这个人"在不同帧/场景中是同一个实体：

```python
class EntityTracker:
    """
    实体追踪：在整个视频中保持实体身份的一致性
    
    解决问题：场景1的"穿红衣服的男人" = 场景3的"厨师" = 用户问的"那个人"
    """

    def __init__(self):
        self.entity_registry = {}  # entity_id → EntityProfile
        self.coreference_map = {}  # 别名 → 主 entity_id

    def register_entity(self, entity_desc: str, scene_id: int, 
                        timestamp_ms: int, bbox: tuple = None) -> str:
        """
        注册新实体或与已有实体匹配
        """
        # 尝试与已有实体匹配
        match = self.find_matching_entity(entity_desc)
        if match:
            # 更新已有实体的出现记录
            self.entity_registry[match.id].appearances.append({
                "scene_id": scene_id,
                "timestamp_ms": timestamp_ms,
                "description": entity_desc,
                "bbox": bbox
            })
            return match.id
        else:
            # 注册为新实体
            entity_id = self.generate_id()
            self.entity_registry[entity_id] = EntityProfile(
                id=entity_id,
                primary_description=entity_desc,
                appearances=[{
                    "scene_id": scene_id,
                    "timestamp_ms": timestamp_ms,
                    "description": entity_desc
                }]
            )
            return entity_id

    def resolve_reference(self, user_reference: str) -> Optional[EntityProfile]:
        """
        解析用户的指代表达
        "那个人" "刚才的红衣服" "视频开头出现的女孩" → 具体实体
        """
        # 1. 精确匹配别名
        if user_reference in self.coreference_map:
            return self.entity_registry[self.coreference_map[user_reference]]

        # 2. 语义相似度匹配
        candidates = []
        for eid, profile in self.entity_registry.items():
            sim = self.compute_similarity(user_reference, profile.primary_description)
            candidates.append((eid, sim))

        candidates.sort(key=lambda x: x[1], reverse=True)

        if candidates and candidates[0][1] > 0.7:
            return self.entity_registry[candidates[0][0]]

        # 3. 时间线索解析
        # "刚才的" → 最近出现的实体
        # "开头的" → 视频前10%出现的实体
        temporal_entity = self.resolve_temporal_reference(user_reference)
        if temporal_entity:
            return temporal_entity

        return None
```

### 6.4 上下文窗口管理（最重要的工程问题）

```python
class ContextWindowManager:
    """
    上下文窗口管理器
    
    核心问题：模型有效注意力有限（即使128K上下文，注意力在中段会衰减）
    策略：把最关键的信息放在开头和结尾，中间放支撑性信息
    """

    MAX_CONTEXT_TOKENS = 8000  # 为视频上下文预留的 token 预算
    # （总上下文可能 32K-128K，但留给推理和回复的空间也要充足）

    def compose_final_context(self, question, video_repr, 
                              relevant_info, conversation_history) -> str:
        """
        组装最终送入模型的上下文
        
        布局策略（注意力U形曲线优化）：
        ┌─────────────────────────┐  ← 注意力高
        │ System Prompt + 角色定义  │
        │ 视频元信息（必备背景）     │
        ├─────────────────────────┤
        │ 视频全局结构概览           │  ← 注意力中等
        │ 相关场景详细描述           │
        │ 语音文本                  │
        ├─────────────────────────┤
        │ 对话历史（中间轮次）       │  ← 注意力较低（可压缩）
        ├─────────────────────────┤
        │ 最新一轮对话              │  ← 注意力高
        │ 当前帧/刚分析的结果       │
        │ 用户问题（重复放在最后）    │
        └─────────────────────────┘  ← 注意力高
        """

        parts = []
        token_budget = self.MAX_CONTEXT_TOKENS

        # === 必须包含（不可压缩）===
        # 元信息（~100 tokens）
        meta = self.format_metadata(video_repr.metadata)
        parts.append(("meta", meta, self.count_tokens(meta)))
        token_budget -= self.count_tokens(meta)

        # 结构概览（~200-500 tokens）
        overview = self.format_structure_overview(video_repr, max_scenes=15)
        parts.append(("overview", overview, self.count_tokens(overview)))
        token_budget -= self.count_tokens(overview)

        # === 按相关性排序的详细信息 ===
        # 检索最相关的场景描述
        relevant_scenes = self.rank_by_relevance(question, video_repr.scene_descriptions)

        for scene in relevant_scenes:
            scene_text = self.format_scene_detail(scene)
            scene_tokens = self.count_tokens(scene_text)
            if token_budget - scene_tokens < 500:  # 保留500给其他
                break
            parts.append(("scene", scene_text, scene_tokens))
            token_budget -= scene_tokens

        # === 语音文本（如果相关）===
        if self.is_speech_relevant(question):
            speech = self.format_relevant_speech(question, video_repr, max_tokens=token_budget//3)
            parts.append(("speech", speech, self.count_tokens(speech)))
            token_budget -= self.count_tokens(speech)

        # === 对话历史（可压缩）===
        history = self.compress_history(conversation_history, max_tokens=min(1000, token_budget//2))
        parts.append(("history", history, self.count_tokens(history)))

        return self.assemble_parts(parts)

    def compress_history(self, history: list, max_tokens: int) -> str:
        """
        对话历史压缩策略：
        - 最近2轮完整保留
        - 更早的轮次只保留摘要
        """
        if len(history) <= 4:  # 2轮 = 4条消息
            return self.format_full_history(history)

        recent = history[-4:]       # 最近2轮完整
        earlier = history[:-4]      # 更早的压缩

        summary = f"[之前的对话摘要: 用户询问了关于{self.summarize_topics(earlier)}的问题，" \
                  f"AI提供了相关分析]"

        return summary + "\n\n" + self.format_full_history(recent)
```

---

## 七、Agent Tool Use 设计（Function Calling）

### 7.1 工具定义

> 本节定义的工具集与 `api-protocol.md` §3.1 完全一致，共 **6 个工具**：
> `seek_and_analyze`、`analyze_time_range`、`search_video_content`、`get_transcript`、
> `get_scene_info`、`control_player`。
> 任何工具的新增 / 修改必须同步两处文档以及 `development-plan.md` M4 任务列表。

```json
{
  "tools": [
    {
      "name": "seek_and_analyze",
      "description": "跳转到视频指定时间点，截取画面并进行视觉分析。当需要查看特定时间的画面内容时使用。",
      "parameters": {
        "timestamp_ms": {"type": "integer", "description": "目标时间点（毫秒）"},
        "focus": {"type": "string", "description": "分析关注点，如'关注画面中的人物动作'"}
      }
    },
    {
      "name": "analyze_time_range",
      "description": "分析一个时间区间内的视频内容。采样多帧进行连续画面理解。用于理解一段过程或动作。",
      "parameters": {
        "start_ms": {"type": "integer"},
        "end_ms": {"type": "integer"},
        "sample_count": {"type": "integer", "default": 5, "description": "采样帧数"},
        "focus": {"type": "string", "description": "分析关注点"}
      }
    },
    {
      "name": "search_video_content",
      "description": "在视频中搜索符合描述的画面。返回匹配的时间点列表。",
      "parameters": {
        "query": {"type": "string", "description": "搜索描述，如'有红色汽车的画面'"},
        "top_k": {"type": "integer", "default": 5}
      }
    },
    {
      "name": "get_transcript",
      "description": "获取指定时间区间的语音转文字内容。",
      "parameters": {
        "start_ms": {"type": "integer"},
        "end_ms": {"type": "integer"}
      }
    },
    {
      "name": "get_scene_info",
      "description": "获取指定场景或时间点所在场景的详细信息。",
      "parameters": {
        "scene_id": {"type": "integer", "description": "场景ID，或为null则根据timestamp定位"},
        "timestamp_ms": {"type": "integer", "description": "时间点，用于定位所在场景"}
      }
    },
    {
      "name": "control_player",
      "description": "控制播放器动作（跳转/暂停/播放），用于向用户展示特定片段。",
      "parameters": {
        "action": {"type": "string", "enum": ["seek", "play", "pause"]},
        "timestamp_ms": {"type": "integer", "description": "seek时的目标时间"}
      }
    }
  ]
}
```

### 7.2 多步工具调用编排

```python
class ToolOrchestrator:
    """
    工具调用编排器：管理 Agent 的多步工具调用过程
    
    设计原则：
    1. 最多 5 步工具调用（防止无限循环）
    2. 每步调用后评估：是否已获得足够信息？
    3. 并行调用：互不依赖的工具调用并行执行
    """

    MAX_TOOL_ROUNDS = 5

    async def execute_agent_loop(self, question, video_repr, conversation_history):
        """
        Agent 主循环
        """
        accumulated_context = []
        
        for round_idx in range(self.MAX_TOOL_ROUNDS):
            # 让模型决定下一步动作
            response = await self.call_llm(
                system=AGENT_SYSTEM_PROMPT,
                context=self.build_context(video_repr, accumulated_context, conversation_history),
                question=question,
                tools=TOOL_DEFINITIONS,
                tool_results=accumulated_context  # 之前的工具调用结果
            )

            # 如果模型直接生成了文本回复（不调用工具）→ 结束
            if response.has_text_content and not response.tool_calls:
                return AgentResponse(
                    answer=response.text,
                    tool_trace=accumulated_context,
                    rounds=round_idx + 1
                )

            # 执行工具调用
            tool_results = await self.execute_tool_calls(response.tool_calls)
            accumulated_context.extend(tool_results)

            # 安全检查：如果所有工具返回空/失败，不要继续循环
            if all(r.is_empty for r in tool_results):
                break

        # 超过最大轮次，强制生成回答
        return await self.force_generate_answer(question, accumulated_context)
```

---

## 八、端到端示例：一个问题的完整处理流程

### 用户问："视频后半部分那个穿蓝色衣服的人做了什么？"

```
Step 1: 问题分析
├── 类型识别: ENTITY_QUERY + ACTION_RECOGNITION
├── 时间约束: "后半部分" → duration/2 ~ duration
├── 实体指代: "穿蓝色衣服的人" → 需要解析
└── 信息需求: 需要该实体的动作序列

Step 2: 信息充分性检查
├── 检查已有视频表示中是否已有"蓝色衣服人物"的信息
├── 检查后半段场景是否已有描述
└── 结果: 场景描述已有，但未明确包含"蓝色衣服"的精确追踪
         → 需要进一步感知

Step 3: 感知采集
├── 策略: 先在后半段的关键帧 embedding 中检索"蓝色衣服的人"
│   └── Tool Call: search_video_content("穿蓝色衣服的人", time_range=[duration/2, duration])
│   └── 结果: 在 3:20, 4:15, 5:02, 6:30 处检测到匹配
│
├── 对匹配时间点做细致分析
│   └── Tool Call: analyze_time_range(start=3:15, end=6:35, sample_count=8, focus="蓝色衣服人物的动作")
│   └── 结果: 
│       - 3:20 该人物走进房间
│       - 4:15 坐在桌前打电话
│       - 5:02 站起来走向窗户
│       - 6:30 离开房间
└── 信息已充分

Step 4: 推理生成
├── 组装上下文: 元信息 + 结构概览 + 后半段场景描述 + 上述分析结果
├── 生成回答: "视频后半段（3:20-6:30），穿蓝色衣服的男性依次做了以下动作：
│              1. [3:20] 走进一间办公室
│              2. [4:15] 坐在桌前接了一个电话
│              3. [5:02] 通话结束后站起来走到窗前
│              4. [6:30] 离开了房间
│              从动作来看，他似乎是接到了一个重要电话后匆忙离开。"
└── 附带: 可点击的时间戳，点击后播放器跳转到对应时刻

Step 5: 反思验证
├── 一致性检查: ✓ 时间点都在后半段范围内
├── 证据支撑: ✓ 每个断言都有对应的帧分析支撑
├── 幻觉检测: ⚠️ "似乎是接到了一个重要电话后匆忙离开" 是推测
│   → 在回答中已用"似乎"标记为推测，可接受
└── 最终置信度: 0.85 → 通过
```

---

## 九、Video RAG 系统设计

### 9.1 为什么需要 RAG

| 没有 RAG 的问题 | RAG 解决的方式 |
|----------------|---------------|
| 每次提问重新分析帧（慢、贵） | 分析结果向量化存储，相似问题直接检索复用 |
| 长视频信息塞不进上下文 | 只检索与问题相关的片段送入 LLM |
| 多轮对话早期结论丢失 | 历史问答持久化，可被后续问题检索到 |
| 跨视频知识无法关联 | 统一向量索引，支持跨视频实体/事件检索 |
| 实体指代消解缺乏上下文 | 实体 Profile 索引化，"那个人"可检索到完整出现记录 |

**本质**：RAG 是前面"记忆层"的工程落地，是让 Agent 能处理长视频的**必要机制**，不是可选优化。

### 9.2 索引架构

```
┌─────────────────────────────────────────────────────────────────┐
│                     Video RAG Index Architecture                  │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  ┌─── Indexing Pipeline (异步构建) ──────────────────────────┐  │
│  │                                                            │  │
│  │  Video Load                                                │  │
│  │    ├── Level 0: metadata + scene_split (即时)              │  │
│  │    ├── Level 1: keyframe_embedding + ASR (秒级)            │  │
│  │    ├── Level 2: scene_description + entity_register (按需) │  │
│  │    └── Level 3: deep_analysis → QA_pair (对话触发)         │  │
│  │                                                            │  │
│  └────────────────── ↓ 写入 ──────────────────────────────────┘  │
│                                                                  │
│  ┌─── Vector Store ──────────────────────────────────────────┐  │
│  │                                                            │  │
│  │  Collection: visual_frames                                 │  │
│  │  ├── vector: CLIP embedding (768-d)                        │  │
│  │  ├── payload: {video_id, timestamp_ms, scene_id,           │  │
│  │  │             keyframe_path, resolution}                   │  │
│  │  └── 用途: 以文搜图、以图搜图                               │  │
│  │                                                            │  │
│  │  Collection: text_segments                                 │  │
│  │  ├── vector: text embedding (BGE/text-embedding-3, 1024-d) │  │
│  │  ├── payload: {video_id, start_ms, end_ms, scene_id,       │  │
│  │  │             segment_type, speaker, entities}             │  │
│  │  ├── segment_type: "speech" | "scene_desc" | "event"       │  │
│  │  └── 用途: 语义文本检索（字幕/描述/事件）                    │  │
│  │                                                            │  │
│  │  Collection: entity_profiles                               │  │
│  │  ├── vector: entity description embedding                  │  │
│  │  ├── payload: {entity_id, video_id, type, description,     │  │
│  │  │             appearances: [{scene_id, timestamp_ms}]}     │  │
│  │  └── 用途: 实体检索、共指消解                               │  │
│  │                                                            │  │
│  │  Collection: qa_cache                                      │  │
│  │  ├── vector: question embedding                            │  │
│  │  ├── payload: {video_id, question, answer, timestamp,      │  │
│  │  │             evidence_scenes, confidence}                 │  │
│  │  └── 用途: 相似问题复用、对话记忆                           │  │
│  │                                                            │  │
│  └────────────────────────────────────────────────────────────┘  │
│                                                                  │
│  ┌─── Metadata Store (SQLite) ───────────────────────────────┐  │
│  │  videos: {id, path, duration, scenes_count, indexed_level} │  │
│  │  scenes: {id, video_id, start_ms, end_ms, description}     │  │
│  │  entities: {id, video_id, type, primary_desc, aliases}     │  │
│  │  conversations: {id, video_id, created_at}                 │  │
│  │  messages: {id, conv_id, role, content, frames}            │  │
│  └────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

### 9.3 索引单元（Chunk）设计

```python
class VideoChunk:
    """
    视频 RAG 的基本检索单元
    
    设计原则：
    1. 每个 chunk 必须能独立提供有意义的信息
    2. 携带足够的元数据支持过滤
    3. 双 embedding（文本 + 视觉）支持多路检索
    """
    chunk_id: str
    video_id: str

    # 时间定位（每个 chunk 必须有明确的时间区间）
    start_ms: int
    end_ms: int

    # 文本表示（必有，用于文本检索路径）
    text_content: str           # 该片段的文本化表示
    text_embedding: list[float] # 文本 embedding

    # 视觉表示（可选，帧级 chunk 有）
    frame_embedding: list[float]  # CLIP embedding
    keyframe_path: str            # 关键帧缩略图存储路径

    # 类型标记
    chunk_type: str  # "scene_summary" | "speech_segment" | "event" | "frame_desc"

    # 结构化元数据（用于 metadata filtering）
    metadata: dict
    # {
    #   "scene_id": 3,
    #   "entities": ["person_1", "person_2"],
    #   "actions": ["walking", "talking"],
    #   "has_speech": true,
    #   "motion_level": "high",      # low/medium/high
    #   "duration_sec": 15.0
    # }
```

**分 chunk 策略对比**：

| 策略 | 适用场景 | 本项目选择 |
|------|---------|-----------|
| 固定时长切分（如每 10s） | 简单暴力，适合均匀内容 | ❌ 不用 |
| 场景驱动切分 | 自然语义边界，信息完整 | ✅ **主方案** |
| 语音句子级切分 | 对话/讲解类视频 | ✅ 辅助（speech 类 chunk） |
| 事件级切分 | 动作/交互密集 | ✅ 辅助（event 类 chunk） |

### 9.4 多路检索与融合

```python
class VideoRAGRetriever:
    """
    多路召回 + 融合排序检索器
    
    为什么不能只用单路检索？
    - 纯文本检索：找不到"红色汽车"（视觉特征）
    - 纯视觉检索：找不到"讨论预算的地方"（语音内容）
    - 纯时间过滤：找不到"最精彩的部分"（语义判断）
    
    → 必须多路并行，融合排序
    """

    def retrieve(self, query: str, video_id: str, 
                 constraints: dict = None, top_k: int = 10) -> list[VideoChunk]:
        """
        Parameters:
            query: 用户问题或 Agent 的检索意图
            video_id: 当前视频 ID
            constraints: 额外约束 {time_range, entity_filter, chunk_type, ...}
            top_k: 返回结果数
        """

        # === Step 1: Query Analysis（查询分析）===
        query_intent = self.analyze_query(query)
        # 判断应该侧重哪条路径
        # e.g. "画面里有什么" → 侧重视觉
        #      "他说了什么" → 侧重语音文本
        #      "穿红衣服的人做了什么" → 视觉+文本混合

        # === Step 2: Multi-Path Retrieval（多路召回）===
        results_per_path = {}

        # Path A: 文本语义检索（场景描述 + 语音文本）
        if query_intent.needs_text_search:
            query_emb = self.text_encoder.encode(query)
            results_per_path["text"] = self.vector_store.search(
                collection="text_segments",
                vector=query_emb,
                filter=self._build_filter(video_id, constraints),
                top_k=top_k * 2  # 多召回，后面 rerank 筛
            )

        # Path B: 视觉语义检索（关键帧匹配）
        if query_intent.needs_visual_search:
            # 用 CLIP text encoder 将文本查询编码为视觉空间向量
            clip_emb = self.clip_text_encoder.encode(query)
            results_per_path["visual"] = self.vector_store.search(
                collection="visual_frames",
                vector=clip_emb,
                filter=self._build_filter(video_id, constraints),
                top_k=top_k * 2
            )

        # Path C: 实体检索
        if query_intent.has_entity_reference:
            entity_emb = self.text_encoder.encode(query_intent.entity_desc)
            results_per_path["entity"] = self.vector_store.search(
                collection="entity_profiles",
                vector=entity_emb,
                filter={"video_id": video_id},
                top_k=5
            )

        # Path D: QA 缓存（相似历史问题）
        qa_emb = self.text_encoder.encode(query)
        results_per_path["qa_cache"] = self.vector_store.search(
            collection="qa_cache",
            vector=qa_emb,
            filter={"video_id": video_id},
            top_k=3,
            score_threshold=0.85  # 高阈值，只要非常相似的
        )

        # === Step 3: Metadata Filtering（元数据过滤）===
        if constraints:
            for path_name, results in results_per_path.items():
                results_per_path[path_name] = self._apply_constraints(results, constraints)
                # 如 time_range=[60000, 180000] → 只保留该时间区间的结果

        # === Step 4: Reciprocal Rank Fusion（融合排序）===
        # 不同路径的结果按 RRF 算法融合
        weights = self._compute_path_weights(query_intent)
        # e.g. 视觉问题 → visual:0.5, text:0.3, entity:0.2
        #      语音问题 → text:0.6, visual:0.2, entity:0.2

        fused = self.reciprocal_rank_fusion(results_per_path, weights, k=60)

        # === Step 5: Rerank（精排，可选）===
        if self.reranker:
            reranked = self.reranker.rerank(query, fused[:top_k * 2])
            return reranked[:top_k]

        return fused[:top_k]

    def reciprocal_rank_fusion(self, results_per_path: dict, 
                                weights: dict, k: int = 60) -> list:
        """
        RRF 融合算法：
        score(doc) = Σ weight_i * (1 / (k + rank_i(doc)))
        
        优点：不依赖原始分数的尺度，不同路径的结果可以公平比较
        """
        doc_scores = {}

        for path_name, results in results_per_path.items():
            weight = weights.get(path_name, 1.0)
            for rank, doc in enumerate(results):
                doc_id = doc.chunk_id
                if doc_id not in doc_scores:
                    doc_scores[doc_id] = {"score": 0.0, "doc": doc}
                doc_scores[doc_id]["score"] += weight * (1.0 / (k + rank + 1))

        # 按融合分数排序
        sorted_docs = sorted(doc_scores.values(), key=lambda x: x["score"], reverse=True)
        return [item["doc"] for item in sorted_docs]

    def _build_filter(self, video_id: str, constraints: dict = None) -> dict:
        """构建向量数据库的 metadata filter"""
        f = {"video_id": video_id}
        if constraints:
            if "time_range" in constraints:
                f["start_ms"] = {"$gte": constraints["time_range"][0]}
                f["end_ms"] = {"$lte": constraints["time_range"][1]}
            if "chunk_type" in constraints:
                f["chunk_type"] = constraints["chunk_type"]
            if "entity_id" in constraints:
                f["metadata.entities"] = {"$contains": constraints["entity_id"]}
        return f
```

### 9.5 检索结果 → Agent 上下文的转换

```python
class RAGContextAssembler:
    """
    将 RAG 检索结果转换为 Agent 可用的上下文文本
    
    关键：不是简单拼接，而是按时间排序、去重、压缩
    """

    def assemble(self, retrieved_chunks: list[VideoChunk], 
                 query: str, token_budget: int = 4000) -> str:
        """
        将检索到的 chunks 组装为结构化上下文
        """
        # Step 1: 去重（同一时间区间可能被多路检索命中）
        unique_chunks = self.deduplicate(retrieved_chunks)

        # Step 2: 按时间排序（保持叙事顺序）
        sorted_chunks = sorted(unique_chunks, key=lambda c: c.start_ms)

        # Step 3: 合并相邻/重叠 chunk（避免碎片化）
        merged = self.merge_adjacent(sorted_chunks, gap_threshold_ms=3000)

        # Step 4: Token 预算控制（截断低相关性的 chunk）
        selected = self.fit_budget(merged, token_budget)

        # Step 5: 格式化输出
        context = "【检索到的相关视频片段】\n\n"
        for chunk in selected:
            time_str = f"[{self.format_time(chunk.start_ms)} - {self.format_time(chunk.end_ms)}]"
            context += f"--- {time_str} (场景{chunk.metadata.get('scene_id', '?')}) ---\n"
            context += f"{chunk.text_content}\n\n"

        return context

    def deduplicate(self, chunks: list[VideoChunk]) -> list[VideoChunk]:
        """基于时间重叠度去重：重叠 >70% 只保留得分最高的"""
        result = []
        for chunk in chunks:
            overlap_found = False
            for existing in result:
                if self.compute_overlap(chunk, existing) > 0.7:
                    overlap_found = True
                    break
            if not overlap_found:
                result.append(chunk)
        return result
```

### 9.6 QA 缓存（对话记忆 RAG）

```python
class QACacheManager:
    """
    问答缓存：将历史分析结论作为 RAG 的一部分
    
    场景：
    1. 用户5分钟前问了"视频里有几个人"，现在问"他们分别穿什么颜色"
       → 之前的回答包含了人物描述，可以直接复用
    2. 用户第二次打开同一个视频问类似的问题
       → 之前的分析结论仍然有效
    """

    def cache_qa_pair(self, video_id: str, question: str, 
                      answer: str, evidence_scenes: list, confidence: float):
        """
        将一次问答缓存到向量索引
        """
        chunk = VideoChunk(
            chunk_id=generate_id(),
            video_id=video_id,
            start_ms=min(s.start_ms for s in evidence_scenes) if evidence_scenes else 0,
            end_ms=max(s.end_ms for s in evidence_scenes) if evidence_scenes else 0,
            text_content=f"Q: {question}\nA: {answer}",
            text_embedding=self.text_encoder.encode(question),
            chunk_type="qa_cache",
            metadata={
                "question": question,
                "answer": answer,
                "confidence": confidence,
                "timestamp": datetime.now().isoformat(),
                "evidence_scene_ids": [s.scene_id for s in evidence_scenes]
            }
        )
        self.vector_store.insert("qa_cache", chunk)

    def try_answer_from_cache(self, video_id: str, question: str, 
                               threshold: float = 0.88) -> Optional[str]:
        """
        尝试从缓存中直接回答（无需重新分析）
        
        高阈值(0.88)确保只复用非常相似的问题的答案，
        避免用不相关的历史回答误导模型
        """
        query_emb = self.text_encoder.encode(question)
        results = self.vector_store.search(
            collection="qa_cache",
            vector=query_emb,
            filter={"video_id": video_id},
            top_k=1,
            score_threshold=threshold
        )

        if results:
            cached = results[0]
            # 附加标记：这是缓存回答，模型可以选择补充/修正
            return f"[历史分析结论，置信度{cached.metadata['confidence']:.0%}]\n" \
                   f"{cached.metadata['answer']}"
        return None
```

### 9.7 索引生命周期管理

```python
class IndexLifecycleManager:
    """
    索引生命周期：何时创建、更新、失效、清理
    """

    def on_video_opened(self, video_path: str):
        """视频打开时"""
        video_id = self.compute_video_id(video_path)  # 基于文件hash

        # 检查是否已有索引
        existing = self.metadata_store.get_video(video_id)

        if existing and existing.file_hash == self.compute_hash(video_path):
            # 视频未修改，索引仍然有效 → 直接复用
            return existing

        if existing:
            # 视频文件变了（重新编辑过）→ 索引失效，重建
            self.invalidate_index(video_id)

        # 新视频或需重建 → 启动异步索引流水线
        self.start_indexing_pipeline(video_id, video_path)

    def invalidate_index(self, video_id: str):
        """索引失效：删除该视频的所有向量和元数据"""
        self.vector_store.delete(filter={"video_id": video_id})
        self.metadata_store.delete_video(video_id)

    def cleanup_stale(self, max_age_days: int = 30):
        """清理超过 N 天未访问的视频索引（节省存储）"""
        stale_videos = self.metadata_store.get_stale_videos(max_age_days)
        for video in stale_videos:
            self.invalidate_index(video.id)
```

### 9.8 技术选型

```
本项目推荐方案（本地优先，轻量级）:

┌─────────────────────────────────────────────────┐
│ 组件          │ 选型               │ 理由        │
├───────────────┼───────────────────┼────────────┤
│ 向量存储      │ FAISS (内存)       │ 无外部依赖, │
│               │ + SQLite (持久化)  │ 单视频数据量小 │
├───────────────┼───────────────────┼────────────┤
│ 文本 Embedding│ BGE-M3 (本地)     │ 多语言,1024-d│
│               │ 或 text-embedding  │ 精度高      │
│               │ -3-small (API)    │            │
├───────────────┼───────────────────┼────────────┤
│ 视觉 Embedding│ CLIP-ViT-L/14    │ 图文对齐标准 │
│               │ (本地 ONNX)       │ 推理快      │
├───────────────┼───────────────────┼────────────┤
│ Reranker      │ BGE-Reranker-v2-m3│ 可选,精排用 │
│ (可选)        │ (本地)            │            │
├───────────────┼───────────────────┼────────────┤
│ 元数据存储    │ SQLite            │ 已有,零额外 │
│               │                   │ 依赖        │
└─────────────────────────────────────────────────┘

单视频索引规模估算（10分钟视频）:
- 场景数: ~15-30 个
- Chunks 总数: ~80-200 个
- 向量维度: 768-1024
- 向量存储: ~1-3 MB
- 元数据: ~100 KB
- 关键帧缩略图: ~5-15 MB

→ 完全可以本地内存驻留，无需分布式方案
```

### 9.9 RAG 与 Agent 决策引擎的集成点

```
Agent 决策循环中 RAG 参与的位置:

PERCEIVE 阶段:
  └── 问题分析后，先走 RAG 检索已有信息
      └── 如果 QA 缓存命中 → 直接跳到 REASON
      └── 如果检索到相关 chunk → 省去重新抽帧分析
      └── 如果检索无结果 → 才触发新的感知采集

REPRESENT 阶段:
  └── 新的分析结果产出后，写入 RAG 索引
      └── 新场景描述 → 写入 text_segments
      └── 新关键帧 → 写入 visual_frames
      └── 新实体 → 写入 entity_profiles

REASON 阶段:
  └── 上下文组装时，RAG 检索结果作为主要信息来源
      └── ContextWindowManager 从 RAG 结果中选取最相关的 chunk

REFLECT 阶段:
  └── 验证通过后，将本轮 QA 写入缓存
      └── 下次类似问题可直接复用

完整流程:
User Question → RAG Retrieve → 够了? → Yes → Reason → Answer
                                    → No  → Perceive → 新结果写入RAG → Reason → Answer
```

---

## 十、错误处理与降级策略

### 10.1 感知失败降级

```python
class GracefulDegradation:
    """
    当某个环节失败时的降级策略，保证系统仍能给出有价值的回答
    """

    def handle_perception_failure(self, failure_type: str, context: dict) -> FallbackPlan:

        if failure_type == "vlm_timeout":
            # VLM 超时 → 降级为纯文本推理（基于已有场景描述）
            return FallbackPlan(
                strategy="text_only_reasoning",
                message="视觉分析暂时不可用，基于已有信息回答",
                use_cached_descriptions=True
            )

        elif failure_type == "frame_capture_failed":
            # 帧截取失败 → 使用最近缓存帧或相邻关键帧
            return FallbackPlan(
                strategy="use_nearest_keyframe",
                fallback_timestamp=self.find_nearest_keyframe(context["target_ms"])
            )

        elif failure_type == "embedding_service_down":
            # Embedding 服务不可用 → 降级为关键词检索
            return FallbackPlan(
                strategy="keyword_search",
                message="语义检索暂时不可用，使用关键词匹配"
            )

        elif failure_type == "asr_failed":
            # 语音转文字失败 → 纯视觉分析，告知用户缺少音频信息
            return FallbackPlan(
                strategy="visual_only",
                message="音频分析不可用，仅基于画面内容回答"
            )

    def handle_reasoning_failure(self, failure_type: str) -> FallbackPlan:

        if failure_type == "low_confidence":
            # 置信度低 → 坦诚告知 + 给出最佳猜测 + 建议验证方式
            return FallbackPlan(
                strategy="honest_uncertainty",
                template="根据现有信息，我的判断是{answer}，但置信度不高({conf:.0%})。"
                         "建议您查看 [{time}] 处的画面确认。"
            )

        elif failure_type == "context_overflow":
            # 上下文过长 → 压缩历史 + 只保留最相关信息
            return FallbackPlan(
                strategy="context_compression",
                actions=["compress_history", "reduce_scene_details", "drop_low_relevance"]
            )

        elif failure_type == "tool_loop_exceeded":
            # 工具调用超过最大轮次 → 基于已有信息强制回答
            return FallbackPlan(
                strategy="force_answer",
                message="分析已进行多轮，基于已收集的信息给出回答"
            )
```

### 10.2 网络与 API 降级

```
降级链:
GPT-4o (API) 不可用
  → 降级到 Qwen-Max (API)
    → 降级到 本地 Qwen2.5-VL-7B
      → 降级到 纯文本推理（不分析新帧）
        → 降级到 基于缓存的模板回答

Embedding 服务不可用
  → 本地 ONNX 模型兜底
    → 关键词匹配兜底
```

---

## 十一、评估与质量保障

### 11.1 视频理解质量评估维度

```
┌────────────────────────────────────────────────────┐
│ 评估维度           │ 指标                │ 目标     │
├───────────────────┼────────────────────┼─────────┤
│ 事实准确性         │ 事实正确率           │ > 90%   │
│ 时间定位准确性     │ 时间偏差 (秒)        │ < 3s    │
│ 实体识别           │ 实体召回/精确率      │ > 80%   │
│ 幻觉率            │ 无依据断言占比        │ < 10%   │
│ 检索相关性         │ Recall@10           │ > 85%   │
│ 回答完整性         │ 信息覆盖度           │ > 85%   │
│ 响应时延           │ 首字输出时间         │ < 3s    │
│ 成本效率           │ Token/问答           │ < 15K   │
└────────────────────────────────────────────────────┘
```

### 11.2 自动化评估流水线

```python
class VideoQAEvaluator:
    """
    自动化评估：对标注的测试集运行 Agent，计算各维度指标
    """

    def evaluate(self, test_set: list[VideoQAPair]) -> EvalReport:
        """
        test_set: [{video_path, question, ground_truth_answer, 
                    ground_truth_timestamps, ground_truth_entities}]
        """
        results = []

        for sample in test_set:
            # 运行 Agent
            agent_answer = self.agent.answer(sample.video_path, sample.question)

            # 评估各维度
            factual_score = self.judge_factual_accuracy(
                agent_answer.text, sample.ground_truth_answer
            )
            temporal_error = self.compute_temporal_error(
                agent_answer.referenced_timestamps, sample.ground_truth_timestamps
            )
            hallucination_rate = self.detect_hallucinations(
                agent_answer.text, sample.ground_truth_answer
            )
            entity_recall = self.compute_entity_recall(
                agent_answer.mentioned_entities, sample.ground_truth_entities
            )

            results.append(EvalResult(
                factual=factual_score,
                temporal_error=temporal_error,
                hallucination=hallucination_rate,
                entity_recall=entity_recall,
                latency=agent_answer.latency_ms,
                token_cost=agent_answer.total_tokens
            ))

        return self.aggregate(results)
```

---

## 十二、安全与隐私

### 12.1 视频内容安全

```python
class ContentSafetyGuard:
    """
    内容安全防护：防止 Agent 被利用分析/传播不当内容
    """

    def pre_check(self, video_path: str) -> SafetyResult:
        """视频加载前的安全预检"""
        # 1. 文件格式验证（防止恶意文件）
        if not self.is_valid_media_format(video_path):
            return SafetyResult(safe=False, reason="不支持的文件格式")

        # 2. 文件大小限制
        if self.get_file_size(video_path) > MAX_VIDEO_SIZE:
            return SafetyResult(safe=False, reason="文件过大")

        return SafetyResult(safe=True)

    def check_frame_safety(self, frame: QImage) -> SafetyResult:
        """帧级内容安全检测（在送入模型前）"""
        # NSFW 检测、暴力内容检测等
        # 可用轻量本地模型或 API
        pass

    def check_output_safety(self, answer: str) -> SafetyResult:
        """输出安全检测（回复用户前）"""
        # 敏感信息泄露检测（如个人隐私）
        # 有害内容检测
        pass
```

### 12.2 数据隐私

```
隐私设计原则:
1. 视频数据本地处理优先（帧不上传，只上传描述文本或压缩小图）
2. 向量索引本地存储（不离开用户设备）
3. 与 LLM API 通信时：
   - 帧图片压缩到最低可用分辨率
   - 不传原始文件路径（隐去个人信息）
   - 支持本地模型作为完全离线方案
4. 对话历史加密存储
5. 用户可一键清除某视频的所有分析数据和缓存
```

---

## 十三、工程实现建议

### 13.1 模型选择策略

```
任务分级，用不同模型：

Tier 1 (轻量，高频调用):
├── 场景切换检测: TransNetV2 (本地，<100ms/帧)
├── 帧 embedding: CLIP-ViT-L/14 (本地 ONNX，~50ms/帧)
├── 文本 embedding: BGE-M3 (本地 ONNX，~10ms/句)
├── 语音转文字: Whisper-base/small (本地)
└── 问题分类: 规则 + 小模型

Tier 2 (中等，VLM 帧描述):
├── 单帧/多帧描述: Qwen2.5-VL-7B / InternVL2-8B (本地 or API)
└── 实体识别/匹配: 同上

Tier 3 (重量，核心推理):
├── Agent 推理/规划/回答: GPT-4o / Claude / Qwen-Max (API)
└── 复杂多帧时序推理: 同上

原则：
- 能本地跑的不调API（降延迟 + 降成本 + 保隐私）
- 核心推理用最强模型（保证质量）
- 感知类任务可以用中等模型（性价比）
```

### 13.2 缓存与增量更新

```python
class AnalysisCache:
    """
    分析缓存：避免重复计算
    
    缓存策略：
    1. 帧 embedding → 持久化（视频不变则永久有效）
    2. 场景分割 → 持久化
    3. 场景描述 → 持久化（同一帧+同一模型=同一结果）
    4. 用户问答 → 持久化（同一视频相似问题可复用）
    5. 实时分析 → 会话级缓存（退出后可选保留）
    
    失效条件：
    - 视频文件 hash 变化 → 全部失效
    - 模型版本变化 → 描述类缓存失效（embedding 可能兼容）
    - 用户手动清除
    """

    def get_or_compute(self, key: str, compute_fn, ttl=None):
        cached = self.cache.get(key)
        if cached and (ttl is None or not cached.is_expired(ttl)):
            return cached.value
        result = compute_fn()
        self.cache.set(key, result, ttl)
        return result
```

### 13.3 成本控制

```
单次问答的 token 消耗估算:

┌──────────────────────────────────────────┐
│ 组件               │ 输入Token │ 输出Token │
├────────────────────┼──────────┼──────────┤
│ System Prompt      │    500   │     -    │
│ RAG 检索结果       │   2000   │     -    │
│ 对话历史           │   1000   │     -    │
│ 帧图片(5帧,512px)  │   2500   │     -    │
│ 工具调用+结果       │   1500   │    500   │
│ 最终回答           │     -    │    300   │
├────────────────────┼──────────┼──────────┤
│ 合计               │  ~7500   │   ~800   │
└──────────────────────────────────────────┘

优化手段:
1. RAG 命中时跳过帧图片（纯文本推理，token 减半）
2. QA 缓存命中时直接返回（接近零成本）
3. 帧图片压缩到 512px + low detail 模式
4. 对话历史超过3轮时压缩早期轮次
5. 批量采样帧合并为一次 VLM 调用（减少请求数）
```

---

## 十四、总结：核心设计理念

1. **分层表示，按需深入** — 不暴力全量分析，模拟人类"先扫后看"的认知
2. **结构化中间表示** — 将视频转化为文本化的结构表示，让语言模型在其最擅长的领域（语言推理）发力
3. **Video RAG 作为记忆骨架** — 多模态多路检索，实现"看过就记住，问到能想起"
4. **自适应感知** — 根据问题类型动态决定看什么、看多少、看多细
5. **多模态对齐** — 视觉和语音信息在时间轴上对齐呈现，而非割裂
6. **实体一致性** — 在整个视频中维护实体身份的连续性
7. **反思验证** — 生成答案后自我验证，检测幻觉和不一致
8. **渐进式构建** — 视频知识从粗到细逐步积累，用户无需等待
9. **优雅降级** — 任何环节失败都有兜底方案，不让用户看到空白
10. **成本可控** — 分级使用模型，RAG 缓存复用，QA 缓存避免重复分析
11. **安全优先** — 内容安全检测 + 数据隐私保护 + 本地处理优先

> 注：模型选择策略、缓存策略、成本估算的完整内容请见上文 §13「工程实现建议」（§13.1 / §13.2 / §13.3）。
