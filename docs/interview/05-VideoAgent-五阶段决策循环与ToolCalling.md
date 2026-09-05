# 面试文档五：Video Agent——五阶段决策循环与 Tool Calling

> 对应简历条目：**"构建证据驱动的 Video Agent，通过 Tool Calling 完成查询规划、视频搜索、时间区间分析和局部复核；明确 Agent 仅负责规划、工具编排和回答，将索引与原始多模态证据作为事实来源。"**
>
> 涉及代码：`src/service/agent/video_agent.cpp/.h`、`src/service/agent/tool_orchestrator.cpp/.h`、`src/service/agent/perception_strategy.cpp/.h`、`src/service/agent/tools/*.cpp`、`src/service/agent/workflow/*`

---

## 一、30 秒电梯话术（背熟）

> Agent 是五阶段循环：PERCEIVE → REPRESENT → REASON → ACT → REFLECT。PERCEIVE 做问题分类（13 种 QuestionType）和采样规划；REPRESENT 走 RAG 检索并受采样计划约束；REASON+ACT 通过 ToolOrchestrator 走多轮 Tool Calling——SSE 流式解析 delta.tool_calls 增量，最多 5 轮、单答案工具总数有上限，超限强制基于现有信息收尾；REFLECT 做四项校验，置信度过低时扩大检索、把反思反馈注入 prompt 重答一次（仅一次，防递归）。核心设计原则：**Agent 只做规划、工具调用和回答，索引和原始多模态证据才是事实来源**——因为回答质量受 min(召回率, 证据保真度, 生成质量) 约束，Agent 的推理变不出没被索引的信息。

---

## 二、理论基础

### 2.1 为什么 Agent 不能是事实源

```
AnswerQuality ≤ min(RetrievalRecall, EvidenceFidelity, GenerationQuality)
```

具体表现：
- 没抽到关键帧 → Agent 无法凭空恢复；
- shot 边界错误 → Agent 无法准确理解事件；
- OCR 没识别到 → Agent 无法可靠回答屏幕内容；
- 检索没召回正确片段 → Agent 思考越多越容易幻觉。

**所以 Agent 的职责收敛为**：
1. 判断是否需要局部复核；
2. 调用 `analyze_time_range` / `seek_and_analyze` 等工具；
3. 组织证据包；
4. 基于证据生成回答；
5. 输出引用和不确定性。

**它不应该**：自己猜测视频事实、代替索引系统保存真相、仅通过多轮反思弥补缺失证据、用答案长度判断置信度。

### 2.2 和朴素 ReAct 的区别

形式上是 ReAct 的强化版，多了三个结构化环节：

| 环节 | 朴素 ReAct | 本项目 |
|---|---|---|
| 前置规划 | 无（直接 Thought） | PERCEIVE：问题分类 + 采样规划 + 信息充分性检查 |
| 证据注入 | 无（模型自由发挥） | REPRESENT：RAG 检索 + 证据包结构化注入 |
| 结果校验 | 无 | REFLECT：四项校验 + 低置信重答闭环 |

### 2.3 受控循环的必要性

不设限的工具循环 = 成本黑洞 + 死循环温床。三层防护：
- 轮次上限（5 轮）；
- 单答案工具总数上限（超限裁剪本轮 calls）；
- 达上限后"基于现有信息强制回答"——用户体验是拿到不完美答案，不是无限等待。

---

## 三、架构总览

```
┌────────────────────────────────────────────────────┐
│ VideoAgent::ask()  —— 顶层协调器                     │
│                                                    │
│  ① 快速路径: QA 缓存命中? 播放器操作? 强制绕过?      │
│  ② Workflow 模式（JSON 预设链路，优先）              │
│  ③ 经典五阶段:                                      │
│     PERCEIVE  → PerceptionStrategy                  │
│                 分类13种QuestionType + 采样规划        │
│     REPRESENT → VideoRAGRetriever                   │
│                 (受采样计划约束的RAG检索)              │
│     REASON+ACT → ToolOrchestrator                   │
│                 SSE流式 + 多轮ToolCalling(≤5轮)       │
│                 6 Tools: seek/analyze/search/...     │
│     REFLECT   → ReflectionEngine                    │
│                 四项校验 + 低置信重答闭环              │
└────────────────────────────────────────────────────┘
```

**6 个工具**（ToolRegistry 注册，ITool 接口）：

| 工具 | 职责 |
|---|---|
| `seek_and_analyze` | 跳转到指定时间点截帧分析 |
| `analyze_time_range` | 区间多帧连续画面理解 |
| `search_video_content` | 语义搜索画面（走 RAG 检索） |
| `get_transcript` | 获取区间语音转写 |
| `get_scene_info` | 场景详细信息 |
| `control_player` | seek/play/pause（联动播放器） |

---

## 四、代码实现详解

### 4.1 主入口 `VideoAgent::ask()` 的完整流程

```cpp
void VideoAgent::ask(conversationId, question, userFrames, videoCtx,
                     currentPlayerPosMs, onProgress, onDone, onError)
{
    if (m_busy) { onError("Agent 正在处理，请稍后"); return; }  // 防重入
    m_busy = true;

    // === 快速路径：QA 缓存 ===
    // 播放器操作必须每次真正执行，不走缓存
    const bool playerOp = isPlayerOp(question);              // 正则: seek|跳转|播放|暂停...
    const bool forceFresh = requestsFreshAnalysis(question); // 正则: 重新分析|再检查一次...
    if (!playerOp && !forceFresh && m_qaCache) {
        auto cached = m_qaCache->tryAnswer(m_activeVideoId, question);
        if (cached) { /* 返回带"历史分析结论，相似度 X%"标记的答案 */ return; }
    }

    // === PERCEIVE: 感知策略 ===
    questionType = m_perception->classifyQuestion(question);
    samplingPlan = m_perception->decideSampling(question, repr, currentPlayerPosMs);

    // === REPRESENT: RAG 检索 — 受 PerceptionStrategy 约束 ===
    const QueryPlan queryPlan = m_retriever->compileQueryPlan(question, c);
    // 规则1: 问题本身有时间表达时，采样计划不得覆盖它
    if (!queryPlan.hasTimeRange() && !samplingPlan.timeRanges.isEmpty()
        && questionType != QuestionType::GlobalSummary) {
        // 取采样计划时间范围作为检索约束
    }
    // 规则2: 按问题类型调 topK 和路径偏好
    switch (questionType) {
    case CurrentFrame:         topK = 3; break;              // 当前帧问题不需要多检索
    case GlobalSummary:        topK = 8; break;              // 全局问题需要更多证据
    case EntityQuery:          topK = 6; c.preferPath = "entity"; break;
    case TemporalLocalization:
    case CausalReasoning:      topK = 6; break;
    default:                   topK = 5; break;
    }
    m_retrievedEvidence = m_retriever->retrieve(question, c, topK);

    // === REASON + ACT ===
    phaseReasonAndAct(conversationId, question, userFrames, videoCtx);
}
```

**两个约束协调规则**（面试细节，体现系统思维）：
1. **显式时间表达 > 采样计划**："00:30 发生了什么"不能被感知策略的候选范围改掉；
2. **问题类型驱动检索预算**：CurrentFrame 只需 3 条（当前帧上下文），GlobalSummary 要 8 条（覆盖全片）。

### 4.2 PERCEIVE：PerceptionStrategy

**问题分类（13 种 QuestionType，关键词规则表，命中优先级从高到低）**：

```cpp
{ CurrentFrame,         "(当前|现在|这一帧|眼前|画面里现在)" },
{ TemporalLocalization, "(什么时候|何时|多少秒|哪一段|第几分钟)" },
{ Counting,             "(几个|多少个|几次|出现了几)" },
{ CausalReasoning,      "(为什么|为何|是因为|导致|原因)" },
{ Counterfactual,       "(如果|假如|要是)" },
{ Comparison,           "(区别|不同|对比|相比)" },
{ TemporalOrder,        "(之前|之后|先|后|顺序)" },
{ Duration,             "(持续|多久|多长时间)" },
{ EntityQuery,          "(那个|谁|什么人|穿|戴|是谁)" },
{ SpatialQuery,         "(左边|右边|上面|下面|角落)" },
{ ActionRecognition,    "(在做什么|干什么|动作)" },
{ GlobalSummary,        "(讲什么|总结|概括|主题)" },
{ DetailDescription,    "(详细描述|具体描述|细节)" },
```

**信息充分性检查**（checkSufficiency）：已有表示够不够？不够建议什么动作？
- GlobalSummary → 摘要已生成则够；
- CurrentFrame → 永远需要新帧（seek_and_analyze）；
- Temporal/Entity/Action → 先试探性 RAG 检索 3 条，命中则够。

**采样规划**（decideSampling → 各类 plan）：

| 问题类型 | 采样计划 |
|---|---|
| GlobalSummary | 均匀稀疏（预算 = max(8, 分钟×3)，上限 50） |
| TemporalLocalization | planTargeted：RAG 检索候选区间 ±2s，密采样 10 帧 |
| Entity/Action | planEntityTracking：同上但更密（12 帧） |
| CausalReasoning | planCausalContext：定位事件，**往前扩 10s 往后扩 5s**（原因通常在前） |
| CurrentFrame | 播放位置 ±3s，精确时间点列表 |
| Counting/Comparison | planExhaustive：全量检索 + 去重 |
| 兜底 | 均匀中等密度 |

**核心思想**：先粗后细、按需下探——不是看得多就好，而是看得准。

### 4.3 REASON+ACT：ToolOrchestrator 多轮工具编排

**SSE 流式 + tool_calls 增量解析**：

```cpp
// 构造时连接 AgentService 的三个信号
connect(m_agent, &AgentService::responseChunk,          // 流式文本增量 → 转发给 UI
        this, &ToolOrchestrator::onAgentChunk);
connect(m_agent, &AgentService::responseFinishedWithTools, // 完成时带 tool_calls 数组
        this, &ToolOrchestrator::onAgentFinished);
connect(m_agent, &AgentService::responseError, this, &ToolOrchestrator::onAgentError);
```

**轮次控制核心逻辑**（`onAgentFinished`）：

```cpp
// 情况1: finishReason == "stop"/"length" 或无 tool_calls → 直接出答案
// 情况2: finishReason == "tool_calls" → 解析并执行
for (const auto& v : toolCalls) {
    c.id   = o.value("id").toString();
    c.name = fn.value("name").toString();
    c.arguments = QJsonDocument::fromJson(argsStr.toUtf8()).object(); // JSON 字符串→对象
}

// 限流：单答案工具数上限
if (m_totalToolCalls + calls.size() > MAX_TOOL_CALLS_PER_ANSWER) {
    const int allow = MAX_TOOL_CALLS_PER_ANSWER - m_totalToolCalls;
    if (allow <= 0) {
        finishWithAnswer("[已达工具调用上限，基于现有信息回答] " + m_streamingText);
        return;
    }
    calls.resize(allow);   // 裁剪而非拒绝
}
```

**工具执行与回填**（`executeToolsThenContinue`）：

```cpp
tool->executeAsync(c.id, c.arguments, [/*...*/](const ToolResult& result) {
    // 组装 role=tool 消息（OpenAI 协议格式）
    toolMsg.insert("role", "tool");
    toolMsg.insert("tool_call_id", c.id);
    toolMsg.insert("name", c.name);
    toolMsg.insert("content", result.success ? result.data : {error});

    if (*executed >= total) {
        if (round + 1 >= MAX_ROUNDS) {
            finishWithAnswer("[已达最大工具轮次] " + m_streamingText);
            return;
        }
        // ★ OpenAI 协议要求：tool 消息之前必须有对应的 assistant tool_calls 消息
        //   否则 400。用缓存的 m_lastAssistantToolCalls 回填。
        QJsonObject assistantEntry;
        assistantEntry.insert("role", "assistant");
        assistantEntry.insert("content", QJsonValue::Null);
        assistantEntry.insert("tool_calls", m_lastAssistantToolCalls);
        // → continueWithToolResults 进入下一轮
    }
});
```

**播放器操作强制 tool_choice**（防止模型绕过工具）：

```cpp
QJsonValue toolChoice = QJsonValue("auto");
if (isPlayerOp(question)) {
    toolChoice = QJsonObject{
        {"type", "function"},
        {"function", QJsonObject{{"name", "control_player"}}}  // 强制指定
    };
}
```

### 4.4 REFLECT：反思闭环（video_agent.cpp）

```cpp
const auto rr = m_reflection->reflect(answer, m_retrievedEvidence, repr);
result.confidence = rr.confidence;

// 反思闭环: 严重问题(置信度<0.5)且未重试 → 补充检索并重新推理
if (!rr.valid && rr.confidence < 0.5f
    && m_reflectionRetries < kMaxReflectionRetries && !isPlayerOp(...)) {
    ++m_reflectionRetries;

    // 1) 补充检索: 去掉时间约束, topK 扩大到 10, 合并去重
    auto expanded = m_retriever->retrieve(question, expandedC, 10);

    // 2) 在证据中附加反思反馈，告知 LLM 上次答案的问题
    retryCtx.retrievalEvidence += "\n# 反思反馈（上次回答存在以下问题，请修正）\n"
                                   + rr.fixSuggestion;

    // 3) 重新走 REASON+ACT
    m_orchestrator->runQuery(convId, question, retryFrames, retryCtx, ...,
        [/*...*/](retryAnswer, retryTrace, retryRounds) {
            // 重新反思(仅评估, 不再递归重试) —— 最多1次, 防循环
        });
}
```

### 4.5 Workflow 模式（第二套执行路径）

基于 JSON 预设（`workflow/presets/video_qa.json`）的工作流执行器：
- LLM 节点 + Function 节点组合成有向链路；
- checkpoint 断点续跑（证据经 EvidenceComposer.toJson 序列化）；
- **定位**：把固定的问答链路从自由 Agent 循环里固化出来——确定性场景不冒 Agent 的不确定性风险。

---

## 五、真实工程问题（war stories）

### 问题 1：OpenAI 协议 400——tool 消息前缺 assistant tool_calls 消息（必讲）

**现象**：回填工具结果时只发了 `role=tool` 消息，OpenAI 兼容 API 直接返回 400。
**根因**：OpenAI 协议规定 `role=tool` 消息必须紧跟产生它的 `assistant tool_calls` 消息（模型需要上下文知道工具结果对应哪个调用）。
**解决**：缓存 `m_lastAssistantToolCalls`，下一轮回填时先补 assistant 消息再跟 tool 消息。
**金句**：这是所有自己从协议层撸 Tool Calling 的人都会撞的坑——用 SDK 的人永远不知道这个约束的存在。

### 问题 2：模型绕过工具直接回答播放器操作

**现象**："跳到 2 分钟"这类指令，模型经常直接回"好的，我帮你跳转了"而不真的调工具——用户以为跳了，实际播放器没动。
**解决**：识别播放器操作意图后把 `tool_choice` 从 `"auto"` 改为强制指定 `control_player`。配套规则：播放器操作不走 QA 缓存（必须每次真正执行）。

### 问题 3：工具调用死循环与成本失控

**现象**：某些模型会反复调用同一个搜索工具，参数几乎不变，5 轮、10 轮不收敛。
**解决**：三层防护（轮次上限 5 / 单答案工具总数上限 / 达限强制收尾）。**判断依据**：视频问答绝大多数 1~2 轮足够（一次检索 + 一次区间复核）；超过 5 轮说明检索没召回该召回的——正确动作是反思扩大检索，不是让模型空转。

### 问题 4：并发重入

**现象**：用户连发两条消息，第二个 ask 覆盖第一个的成员状态（m_retrievedEvidence、回调指针），第一个请求的回调打到第二个的上下文。
**解决**：`m_busy` 防重入，忙时直接报错。**坦承**：更彻底的方案是每请求一个独立会话对象（per-request state object），这是已知改进点——主动讲这个体现对状态管理的敏感。

### 问题 5：反思重试的递归风险

**现象设计期就预判**：反思失败→重答→再反思失败→再重答……无限套娃烧钱。
**解决**：`m_reflectionRetries < kMaxReflectionRetries` 硬限制一次；重答后只评估不再触发重试。

### 问题 6：工具上下文的 videoId 注入

**现象**：Agent 切换视频后，工具还在用旧 videoId 检索。
**解决**：`setActiveVideo` 时通过 `ToolOrchestrator::setActiveVideoContext` 分发给需要视频上下文的工具（`search_video_content` / `get_transcript` / `get_scene_info` 各自 setVideoId/setVideoPath）。

---

## 六、面试官可能问的问题

| 问题 | 回答要点 |
|---|---|
| **你的 Agent 和 ReAct 的区别？** | ReAct 强化版：前置问题分类+采样规划（PERCEIVE）、结构化证据注入（REPRESENT）、带证据校验的反思闭环（失败扩大检索带反馈重答），而非裸的 Thought→Action→Observation。 |
| **为什么限 5 轮？** | 经验值：视频问答绝大多数 1~2 轮够（一次检索+一次区间复核）；超 5 轮说明检索没召回该召回的，正确动作是反思扩大检索而非空转。 |
| **工具并行执行吗？** | 当前顺序执行（代码注释注明"后续可并行独立工具"）；多数场景单轮单调用；并行化需要依赖分析+结果聚合，是明确优化项。 |
| **Agent 记忆怎么做？** | 三层：对话历史（SQLite 持久化+压缩）、QA 缓存（向量检索相似问题，0.88 阈值）、实体档案（跨问题实体身份一致性，"那个人"指代）。 |
| **SSE 流式和工具调用怎么共存？** | responseChunk 信号持续转发文本增量给 UI；finishReason=tool_calls 时暂停用户可见输出，执行工具后 continueWithToolResults 开下一轮；用户全程看到流式过程。 |
| **REFLECT 失败怎么恢复？** | 置信度<0.5：去时间约束扩大检索 topK=10、合并新证据、反思 issue 作为显式反馈注入 prompt 重答一次；再失败带不确定性说明返回。 |
| **检索完全没命中怎么办？** | 反思判"具体结论但无可追溯证据"；Agent 触发 seek_and_analyze/analyze_time_range 局部复核（工具层按需取帧分析）——"感知不足→下探采样"的设计意图。 |
| **Workflow 模式和 Agent 模式什么关系？** | 双轨：固定链路用 Workflow（确定性、可断点续跑），开放问题用 Agent 循环（灵活性）；互为补充而非替代。 |
| **为什么用 C++ 自研整套而不是 LangChain？** | 桌面产品 + 本地 ONNX 推理 + 播放器深度联动（seek/截帧/播放控制是工具的一等公民）；RAG/编排核心逻辑不依赖 Python 生态，自研换来对每一层的完全掌控（例：能修 tool 消息协议级 bug）。 |
| **这个 Agent 最大的局限？** | 反思引擎仍是启发式（非 VLM 帧级复核）；工具顺序执行；请求级状态隔离未做。每个都有明确的改进方案——主动讲局限+方案比掩盖更加分。 |

---

## 七、白板演练：一次完整问答的时间线

```
用户问："后半部分那个穿蓝衣服的人做了什么？"（10 分钟视频）

T0   VideoAgent::ask
     ├─ QA缓存未命中 / 非播放器操作
     ├─ PERCEIVE: EntityQuery + ActionRecognition
     │    planEntityTracking → 密采样12帧
     └─ REPRESENT: compileQueryPlan("后半") → [300s, 600s]
          retrieve(topK=6, preferPath=entity)
          → 命中场景 12/15/17 的证据

T1   REASON+ACT (ToolOrchestrator)
     ├─ 第1轮: LLM 决定调 search_video_content("穿蓝衣服的人")
     │    → 工具走 RAG 检索 → 返回 [5:02][6:30] 匹配
     ├─ 第2轮: LLM 调 analyze_time_range(4:55-6:40, focus="蓝衣人物动作")
     │    → 区间多帧分析 → 动作序列
     └─ 第3轮: finishReason=stop → 流式输出答案

T2   REFLECT
     ├─ 时间戳合法性 ✓ / 证据覆盖 ✓ / 3模态+互证
     ├─ confidence = 0.85 → 通过
     └─ AgentAnswer { answer, evidence(6条), toolCallsTrace
                     [search, analyze], rounds=3, confidence=0.85 }

T3   UI: 时间戳 [5:02] 可点击 → control_player seek → 播放器跳转
     QA cache: confidence≥0.7 且有证据 → 写入缓存
```
