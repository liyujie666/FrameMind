## 视频分析 AI Agent 深度设计思路（愿景稿）

> **本文档定位**：项目长期愿景与能力清单，记录早期头脑风暴的思路。
>
> ⚠️ **重要**：
> - 落地架构以 [`agent-core-design.md`](./agent-core-design.md) 为准（其 Agent 主循环是 PERCEIVE → REPRESENT → REASON → ACT → REFLECT 五阶段）；
> - MVP 范围以 [`development-plan.md`](./development-plan.md) 1.1/1.2 节为准；
> - 本文中所列的多 Agent 协作、Neo4j 知识图谱、SAM2、Grounding-DINO、pyannote 说话人分离、跨视频关联、实时流分析等能力**均属于 P2（暂不实现）**；
> - 阅读本文档时请把它当作"我们最终想做成什么样"，而不是"现在要做什么"。

---

### 一、Agent 核心架构范式（早期 4 阶段版本）

> 注：此处的"感知-记忆-规划-行动"四阶段是初稿；正式实现采用 `agent-core-design.md` 的 **五阶段** 模型（额外加入 REPRESENT 与 REFLECT），思路一致但更细。

不是简单的"截图 + 问模型"，而是一个具备 **感知-记忆-规划-行动** 闭环的自主智能体：

```plain
┌─────────────────────────────────────────────────────────────┐
│                     Video Analysis Agent                      │
├─────────────────────────────────────────────────────────────┤
│                                                              │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐    ┌───────┐ │
│  │Perception│───▶│  Memory  │───▶│ Planning │───▶│Action │ │
│  │  感知层   │    │  记忆层   │    │  规划层   │    │ 行动层│ │
│  └──────────┘    └──────────┘    └──────────┘    └───────┘ │
│       ▲                                              │       │
│       └──────────────── feedback ─────────────────────┘       │
│                                                              │
├──────────────────── Tool / Plugin Layer ─────────────────────┤
│  PlayerSDK │ VLM │ ASR │ OCR │ Detector │ Tracker │ FFmpeg  │
└─────────────────────────────────────────────────────────────┘
```

---

### 二、各层详细设计
#### 1. 感知层（Perception） — 多模态信息提取
不只是"抽帧"，而是构建**视频的多层次语义表示**：

| 感知维度 | 方法 | 产出 |
| --- | --- | --- |
| **时空采样策略** | 自适应关键帧检测（场景切换检测 + 均匀采样 + 运动幅度触发） | 非均匀帧序列 |
| **视觉语义** | 视频帧送 VLM（Qwen-VL / GPT-4o / InternVL） | 每帧/片段描述 |
| **时序理解** | 多帧联合推理、视频级模型（VideoLLaMA / LLaVA-Video） | 动作识别、事件因果 |
| **空间感知** | 目标检测（YOLO/Grounding-DINO）+ 分割（SAM2） | 物体位置、区域 |
| **音频感知** | Whisper ASR + 说话人分离（pyannote）+ 音事件检测 | 字幕、声纹、环境音 |
| **文字感知** | 视频 OCR（PaddleOCR / TrOCR） | 画面内文字时间轴 |
| **情感/氛围** | 表情识别 + 背景音乐分析 + 色调分析 | 情绪曲线 |


**关键设计点**：感知不是一次性完成的，Agent 应具备 **按需感知** 能力 — 用户问到什么，再深入分析相关维度，避免全量计算浪费。

#### 2. 记忆层（Memory） — 视频知识的结构化存储
这是 Agent 区别于简单工具调用的核心：

```plain
Video Memory Store
├── Short-term Memory（工作记忆）
│   ├── 当前对话上下文
│   ├── 用户关注的时间窗口 / 区域
│   └── 最近分析过的帧缓存
│
├── Long-term Memory（长期记忆）
│   ├── Video Graph（视频知识图谱）
│   │   ├── 实体节点：人物、物体、地点、文字
│   │   ├── 事件节点：动作、交互、状态变化
│   │   └── 时间边：出现/消失/持续时间区间
│   │
│   ├── Timeline Index（时间线索引）
│   │   ├── 场景分割结果 + 每段摘要
│   │   ├── 语音转文字 + 时间对齐
│   │   └── 关键事件时间戳
│   │
│   └── Embedding Store（向量索引）
│       ├── 帧级视觉 embedding（CLIP / SigLIP）
│       ├── 片段级语义 embedding
│       └── 音频/文字 embedding
│
└── Episodic Memory（情节记忆）
    ├── 用户历史问答
    ├── 用户标注的兴趣点
    └── 跨视频关联（同一人物在不同视频中）
```

**设计思想**：视频加载后并不立即全量处理，而是做"粗粒度索引"（场景分割 + 均匀采样 embedding），后续按需深入。类似人看视频，先有个大概印象，被问到才仔细回忆。

#### 3. 规划层（Planning） — 多步推理与任务分解
Agent 面对复杂问题时的推理链：

```python
# 用户: "视频后半段那个穿红衣服的人做了什么？"

Planning Chain:
├── Step 1: 确定"后半段"时间范围 → duration()/2 ~ duration()
├── Step 2: 在该范围内检索"红衣人物" → 视觉检索(embedding相似度 + 颜色过滤)
├── Step 3: 定位到目标帧序列 → 多个时间片段
├── Step 4: 对每个片段做动作分析 → VLM 多帧推理
├── Step 5: 综合生成回答 → LLM 汇总
└── Step 6: 提供跳转建议 → seek() 到关键时刻
```

**核心能力**：

+ **ReAct 模式**：观察 → 思考 → 行动 → 再观察，循环直到回答用户
+ **自适应深度**：简单问题一步到位，复杂问题多轮工具调用
+ **主动探索**：如果初次分析信息不足，Agent 自主决定看更多帧、换个角度看
+ **反思纠错**：检测到答案可信度低时，自主重新分析验证

#### 4. 行动层（Action） — Tool Use 设计
Agent 可调度的工具集合：

```yaml
tools:
  # === 播放器控制（基于你的 player_sdk）===
  player.seek:       跳转到指定时间
  player.screenshot: 对当前帧/指定时间截图
  player.get_frame:  获取指定时间的帧数据（onVideoFrame）
  player.get_info:   获取媒体元信息
  player.get_range:  获取指定区间的帧序列

  # === 视觉分析 ===
  vision.describe:     描述单帧/多帧内容（VLM）
  vision.detect:       目标检测 + 定位
  vision.track:        跨帧目标追踪
  vision.segment:      语义分割/实例分割
  vision.compare:      帧间对比/变化检测
  vision.search_by_image:  以图搜帧（相似帧检索）

  # === 时序分析 ===
  temporal.scene_split:     场景切割
  temporal.action_recognize: 动作识别
  temporal.event_detect:    事件检测（异常、转折点）
  temporal.search_by_text:  文本搜视频片段

  # === 音频分析 ===
  audio.transcribe:   语音转文字
  audio.diarize:      说话人分离
  audio.classify:     环境音/音乐分类

  # === 知识操作 ===
  memory.query:       查询已建立的视频知识
  memory.store:       存储新的分析结果
  memory.relate:      建立实体间关联
```

---

### 三、高阶 Agent 能力（区别于简单问答工具）
#### 1. 主动分析（Proactive Analysis）
不只是被动等用户提问，而是主动产出洞察：

+ **视频加载时**：自动生成结构化摘要 + 目录
+ **播放过程中**：实时标注关键事件、弹幕式信息提示
+ **发现异常时**：主动提醒（如监控场景检测到异常行为）

#### 2. 多粒度交互（Multi-granularity Interaction）
```plain
视频级:  "这个视频讲了什么？"  → 全局摘要
片段级:  "第2分钟到第5分钟发生了什么？" → 片段分析  
帧级:    "当前这一帧里有什么？" → 单帧理解
区域级:  "左上角那个东西是什么？" → ROI 区域分析 + 裁剪送模型
跨视频:  "这个人在上一个视频里也出现过吗？" → 跨视频关联
```

#### 3. 因果推理与反事实（Causal & Counterfactual）
+ "为什么这个人突然跑了？" → 回溯前文找原因
+ "如果去掉背景音乐，这段视频表达的情绪会变吗？" → 多模态拆解

#### 4. 长视频处理策略（Long Video Reasoning）
针对几十分钟甚至几小时的视频：

```plain
分层处理架构:
Level 0: 全视频 → 场景分割 → N 个片段（秒级开销）
Level 1: 每片段 → 关键帧 + 简短描述（分钟级，可异步）
Level 2: 按需深入 → 目标片段精细分析（仅在用户提问时触发）
Level 3: 跨片段推理 → 全局知识图谱上做 Graph RAG
```

#### 5. 多 Agent 协作（Multi-Agent）
复杂任务拆分给专业子 Agent：

```plain
Orchestrator Agent（编排者）
├── Visual Agent     — 负责视觉理解
├── Audio Agent      — 负责音频理解  
├── Temporal Agent   — 负责时序推理
├── Knowledge Agent  — 负责记忆检索与更新
└── Narration Agent  — 负责最终答案组织与表达
```

---

### 四、领域场景深化
| 场景 | 核心能力 | 独特需求 |
| --- | --- | --- |
| **教育** | 知识点提取、难点定位、自动出题 | 与教材知识库对齐 |
| **安防** | 异常检测、行为识别、事件回溯 | 实时流 + 低延迟告警 |
| **体育** | 战术分析、球员追踪、精彩集锦 | 高速运动目标追踪 |
| **医疗** | 手术过程分析、影像标注 | 专业术语 + 合规 |
| **会议** | 议题分割、决议提取、TODO 生成 | 多说话人 + 屏幕内容 |
| **电商直播** | 商品识别、卖点提取、违规检测 | 实时流 + 商品库匹配 |
| **影视创作** | 镜头语言分析、情感曲线、剪辑建议 | 美学评价 + 叙事结构 |


---

### 五、技术选型建议
```plain
视觉大模型:  Qwen-VL-Max / GPT-4o / InternVL2.5 (多帧支持)
视频理解:   VideoLLaMA2 / LLaVA-Video / Qwen2.5-VL (原生视频输入)
目标检测:   Grounding-DINO + SAM2 (开放词汇检测+分割)
音频:       Whisper-large-v3 + pyannote-audio
向量检索:   CLIP/SigLIP embedding + FAISS/Milvus
知识图谱:   Neo4j / 内存图结构
Agent框架:  LangGraph / AutoGen / 自研 ReAct Loop
播放器:     你的 SmartPlayer SDK（帧回调 + 控制）
```

