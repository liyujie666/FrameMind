我先探索一下当前项目的智能体与大模型对话相关实现。
子代理报告被截断，我直接读关键文件。
我已经把 LLM 对话链路完整读完了。**你的判断是对的，但根因比"没做记忆系统"更严重——是几个具体的实现 Bug 让已有的记忆机制彻底失效了。**

---

## 一、根因诊断（按严重度）

### P0-1 Tool Calling 路径下 assistant 回复**从未写入历史**（核心元凶）

`AgentService` 有两条路径：

- `sendMessage()` → onDone 里有 `m_histories[...].append({role:assistant, content})` ✅
- `sendStreamWithTools()` → onDone **只 emit 信号，没有 append 历史** ❌

```327:395:src/service/agentservice.cpp
        // onDone
        [this]() {
            m_streaming = false;
            emit responseFinishedWithTools(m_currentConvId,
                                            m_pendingToolCalls,
                                            m_pendingFinishReason,
                                            m_accumulated);
        },
```

而只要打开了视频，`ChatViewModel::doSend` 就走 `VideoAgent → ToolOrchestrator → sendMessageWithTools`，**即主路径**。结果：

| 轮次 | 实际发给模型的 messages |
|---|---|
| 第1轮 | `[system, user1]` |
| 第2轮 | `[system, user1, user2]` ← 模型看不到自己第1轮说了什么 |
| 第3轮 | `[system, user1, user2, user3]` |

这就是"每次对话都没有记忆"的直接原因。

### P0-2 RAG 检索结果压根没进 Prompt

`VideoAgent::ask` 做了检索，但结果只喂给了 reflection 和返回值：

```99:107:src/service/agent/video_agent.cpp
    if (m_retriever && !m_activeVideoId.isEmpty()) {
        VideoRAGRetriever::Constraints c;
        c.videoId = m_activeVideoId;
        m_retrievedEvidence = m_retriever->retrieve(question, c, 5);
    }
    // === REASON + ACT ===
    phaseReasonAndAct(conversationId, question, userFrames, videoCtx);  // ← evidence 没传
```

整个 video_rag 的检索能力**对模型不可见**，全靠模型自己去调 `search_video_content` 工具，等于白做一遍向量检索。

### P0-3 `sceneOverview` 被 System Prompt 丢弃

`buildVideoContext` 辛苦拼了全部场景时间轴，但 `buildSystemPrompt` 只用了 `videoSummary`：

```68:78:src/service/agentservice.cpp
    if (!ctx.isEmpty()) {
        // 只用了 fileName / durationMs / 分辨率 / videoSummary
        // ctx.sceneOverview 完全没用！
    }
```

模型因此不知道视频有哪些场景、发生在什么时间，也就无法做时间定位类回答。

### P0-4 单例 `NetworkClient` + 单例流状态 → 上下文串台

`NetworkClient` 只有一个 `m_activeStream`，且每次发起都先 `cancelStream()`；`AgentService` 只有一份 `m_currentConvId` / `m_accumulated`。

后台 `VideoAnalysisService::describeAllScenes` 会持续用 `oneShotVLM` 发请求（假 convId `__vlm_oneshot_*`）。用户在索引期间提问 →

1. 用户的流被 cancel（或反之），`m_currentConvId` 被覆写
2. `m_accumulated` 被清空
3. assistant 回复被 append 到**错误的 conversationId 历史**里
4. `oneShotVLM` 的 aggregator 永不触发 → `m_activeTasks` 不递减 → **索引队列永久卡死**

### P1-5 流式文本被重复追加两次

`ChatViewModel` 和 `ToolOrchestrator` **都**连了 `AgentService::responseChunk`：

- 路径A：`ChatViewModel` 直连 → `appendDeltaSilent`
- 路径B：`ToolOrchestrator::onAgentChunk` → `m_onProgress` → `VideoAgent` → `ChatViewModel` 的 onProgress 回调 → `appendDeltaSilent`

同一个 delta 进两次，回答文本会重复。

### P1-6 历史无限增长 + base64 图片永久驻留

`buildRequestPayload` 把含 `image_url` base64 的完整 user 消息 append 进 `m_histories`：

```136:138:src/service/agentservice.cpp
    const QJsonObject userMsg = makeUserMessage(text, frames);
    messages.append(userMsg);
    history.append(userMsg);   // ← 含多张 1024px base64 图，永久留存
```

`continueWithToolResults` 又把 tool_calls + 大段 tool JSON 结果永久写进历史。3~4 轮后 payload 就会数 MB，直接触发 token 上限或超时。且历史里会留下 `assistant(tool_calls) → tool → user` 这种**缺少收尾 assistant 文本**的畸形序列，很多 provider 会 400。

### P1-7 `stopGeneration` 在 VideoAgent 路径下会锁死 Agent

```331:336:src/viewmodel/chatviewmodel.cpp
void ChatViewModel::stopGeneration()
{
    if (m_agentService) m_agentService->stopGeneration();  // 没有调 m_videoAgent->cancel()
}
```

`ToolOrchestrator::m_running` 永远保持 `true` → 之后所有提问都被 `"Agent 正在执行，请稍后"` 拒绝，用户看起来就像"智能体失忆/失联"。

### P2-8 QA 缓存对指代性追问会误命中

`QACacheManager::tryAnswer` 只按问题文本相似度匹配，不看对话上下文。追问「那他后来呢」「再详细点」会命中上一条不相关的缓存，直接返回旧答案并跳过整个推理链。

---

## 二、优化方案

### 阶段 1：修复记忆链路（P0，改动小、收益最大）

**1.1 引入统一的 `ConversationMemory` 类**（新增 `src/service/agent/conversation_memory.h/.cpp`）

把散落在 `AgentService::m_histories` 的裸 `QJsonArray` 收敛为一个有职责的组件：

```cpp
class ConversationMemory {
public:
    void appendUser(const QString& convId, const QString& text,
                    const QList<QImage>& frames);   // 图片只在当轮保留
    void appendAssistant(const QString& convId, const QString& text);
    void appendToolRound(const QString& convId,
                         const QJsonArray& toolCalls,
                         const QJsonArray& toolResults);  // 标记为可折叠
    /// 输出用于请求的 messages（已做窗口裁剪 + 图片剥离 + 摘要压缩）
    QJsonArray buildMessages(const QString& convId,
                             const QString& systemPrompt) const;
    void seed(const QString& convId, const QList<ChatMessage>&);
    void clear(const QString& convId);
private:
    struct Turn { QString role; QJsonValue content; QJsonArray toolCalls;
                  bool volatileImages = false; bool toolNoise = false; int tokens = 0; };
    QHash<QString, QList<Turn>> m_turns;
    QHash<QString, QString>     m_summaries;   // 滚动摘要
};
```

核心规则：
- **图片只保留最近 1~2 轮**，更早的 user 图片消息降级为 `[已发送 N 张视频帧: 时间点 xx:xx]` 文本
- **tool 轮次只保留最近 1 轮**，历史 tool 结果压缩成 `[已调用 search_video_content, 返回 3 条证据]`
- 按 token 预算（粗估 `len/2`）从旧到新淘汰，超阈值时触发滚动摘要

**1.2 补上 assistant 落历史**

`sendStreamWithTools` 的 onDone 中，`finish_reason == "stop"` 时写入 assistant 文本；`== "tool_calls"` 时写入 tool_calls 轮次。彻底解决 P0-1。

**1.3 把 evidence 注入 Prompt**

`VideoAgent::phaseReasonAndAct` 增加 `retrievedEvidence` 参数，`ToolOrchestrator::runQuery` 透传，最终在 `messages` 中以**独立的 system 补充块**注入（不污染 user 消息）：

```
# 已检索到的相关片段（RAG 证据，优先据此回答）
1. [01:23-01:45] (visual, 相似度 0.87) 红衣男子走向柜台...
2. [03:10-03:22] (text, 0.79) 语音："这个方案我们下周再定"
若证据不足以回答，再调用工具补充。
```

**1.4 补全 System Prompt**

`buildSystemPrompt` 加入 `sceneOverview`（并把 ms 转成 `mm:ss`）、`hasAudio`、`fps`，以及当前播放位置：

```
# 场景时间轴
- [00:00-00:12] 场景1: 会议室全景，5人就座
- [00:12-00:35] 场景2: 特写发言人...

# 当前播放位置
00:47
```

同时把 `VideoContext` 扩一个 `currentPositionMs` 字段——现在 `currentPlayerPosMs` 传到 `VideoAgent` 后只用于采样计划，模型不知道"现在"是什么时候，无法回答"画面里这是谁"。

### 阶段 2：隔离并发流（P0-4）

`NetworkClient` 由单流改为**多流会话对象**：

```cpp
class LLMStream : public QObject {   // 每次请求一个实例，自管 buffer/callbacks
    ...
};
class NetworkClient {
    LLMStream* openStream(const QUrl&, const QJsonObject&, ...);  // 返回句柄
    void cancelAll();
};
```

`AgentService` 相应地把 `m_currentConvId/m_accumulated/m_pendingToolCalls` 收进 per-request 的 `RequestCtx` 结构，`QHash<QString, RequestCtx> m_active`。

另外给后台分析请求（`oneShotVLM`）单独一条低优先级通道 + 并发闸门，避免和用户交互抢流。

### 阶段 3：结构化记忆增强（P1~P2）

**3.1 落库 tool 轨迹**，新增表，让重启后能恢复完整推理上下文：

```sql
CREATE TABLE IF NOT EXISTS message_meta (
  message_id TEXT PRIMARY KEY,
  tool_calls TEXT,          -- JSON
  evidence   TEXT,          -- JSON: chunkId/时间区间/score
  confidence REAL,
  video_id   TEXT,
  FOREIGN KEY (message_id) REFERENCES messages(id) ON DELETE CASCADE
);
```

**3.2 会话级长期记忆**：`conversation_facts` 表存这轮对话中确认过的结论（"红衣人 = 张经理"、"用户关注的是 03:10 那段"），下轮作为 `# 已确认事实` 注入。这块可以直接复用现成的 `EntityTracker`。

**3.3 指代消解 + 缓存 Key 修正**：`QACacheManager::tryAnswer` 的 key 改为 `最近1轮问答摘要 + 当前问题`，并对含指代词（他/那个/刚才/再/继续）的问题**直接跳过缓存**。

**3.4 修 `stopGeneration` / 重复追加**：
- `ChatViewModel::stopGeneration` 增加 `m_videoAgent->cancel()`
- `ToolOrchestrator` 加看门狗 timer，超时强制释放 `m_running`
- 去掉 ChatViewModel 对 `responseChunk` 的直连（VideoAgent 路径下只用 onProgress 回调），或在连接处按 `m_videoAgent` 是否活跃互斥

---

## 三、优先级与预期收益

| 项 | 工作量 | 收益 |
|---|---|---|
| 1.2 assistant 落历史 | 极小（~20 行） | **直接解决"没有记忆"** |
| 1.4 System Prompt 补全 | 小（~40 行） | 时间定位类问题可用 |
| 1.3 evidence 注入 | 小（~60 行） | RAG 能力真正生效 |
| 3.4 stopGeneration / 重复追加 | 小 | 消除"卡死/文本重复" |
| 1.1 ConversationMemory | 中（~300 行） | 多轮稳定、不爆 token |
| 2 多流隔离 | 中（~200 行） | 消除串台与索引卡死 |
| 3.1~3.3 结构化记忆 | 中大 | 跨会话/长视频体验 |

---

建议**先做 1.2 + 1.4 + 1.3 + 3.4**，这四项加起来约 150 行改动，就能让智能体真正"记得住上文 + 看得见视频结构 + 用得上 RAG"。之后再上 `ConversationMemory` 和多流隔离。

要我现在按这个顺序动手实施吗？我可以先出这四项的完整补丁，编译验证后再推进阶段 1.1 和阶段 2。