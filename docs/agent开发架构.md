如果你说的是 **AI Agent（智能体）开发**，那么目前（2026年）主流 Agent 框架（如 LangGraph、AutoGen、OpenAI Agents SDK、CrewAI 等）虽然实现方式不同，但底层知识体系是相通的。

如果目标是成为一名**Agent开发工程师**，我建议按照下面这条知识路线学习。

---

# 第一阶段：LLM基础（必须掌握）

这是Agent最核心的部分。

## 1. 大语言模型原理

至少需要了解：

* Transformer
* Self-Attention
* KV Cache
* Token
* Context Window
* Position Encoding
* Temperature
* Top-p
* Embedding
* Decoder Only（GPT）
* Encoder（BERT）
* MoE（Mix of Experts）

需要达到：

> 不要求自己训练模型，但必须知道为什么模型会这样回答。

---

## 2. Prompt Engineering

包括：

* Zero-shot
* One-shot
* Few-shot
* Chain of Thought（CoT）
* Self-Consistency
* Tree of Thoughts（ToT）
* ReAct
* Reflection
* Prompt Template

例如：

```
System Prompt

User Prompt

Assistant Prompt
```

以及

```
Role
Goal
Constraints
Output Format
```

这些Prompt设计方式。

---

## 3. Function Calling（Tool Calling）

Agent最重要的能力之一。

例如：

```
用户：

北京天气怎么样？
```

LLM不会自己知道天气。

它会输出：

```
call weather_api(city="北京")
```

程序收到后：

```
weather_api

↓

返回

25℃
晴天
```

再交给LLM：

```
今天北京25℃，晴。
```

所以需要理解：

* Tool Schema
* JSON Schema
* Function Calling
* Structured Output

---

# 第二阶段：RAG（必须掌握）

Agent几乎都会结合RAG。

需要学习：

## 文档解析

PDF

Word

Markdown

HTML

Excel

PPT

OCR

例如：

```
pdf

↓

Chunk

↓

Embedding

↓

Vector DB
```

---

## Chunk

为什么切块？

怎么切？

例如：

```
固定长度

500 token
```

或者

```
Recursive Splitter
```

需要了解：

* overlap
* chunk size
* semantic chunk

---

## Embedding

知道：

一句话

↓

768维向量

↓

向量检索

常见Embedding：

* OpenAI Embedding
* BGE
* Jina
* Nomic

---

## 向量数据库

至少了解一种：

* Milvus
* Chroma
* FAISS
* Qdrant
* pgvector

需要知道：

* TopK
* Cosine Similarity
* HNSW
* IVF

---

# 第三阶段：Agent工作流（重点）

真正的Agent不是：

```
LLM

↓

回答
```

而是：

```
用户

↓

Planner

↓

Task

↓

Tool

↓

Memory

↓

Reflection

↓

Answer
```

需要学习：

---

## Planning

例如：

```
帮我写一篇论文

↓

规划：

①搜索资料

②总结

③写初稿

④润色
```

---

## ReAct

经典Agent模式。

流程：

```
Thought

↓

Action

↓

Observation

↓

Thought

↓

Action

↓

Answer
```

几乎所有Agent都会用。

---

## Reflection

Agent检查自己。

例如：

```
回答完成

↓

检查有没有错误

↓

重新修改
```

Self Reflection

Critic

Judge

都属于这一类。

---

## Multi-Agent

例如：

```
Planner

↓

Researcher

↓

Coder

↓

Reviewer

↓

Final
```

多个Agent协同。

---

# 第四阶段：Memory

Agent最大的特点。

需要理解：

## 短期记忆

就是Context。

例如：

```
最近20条聊天
```

---

## 长期记忆

例如：

```
用户喜欢Qt

↓

存数据库

↓

以后自动读取
```

包括：

Semantic Memory

Episode Memory

User Profile

Preference

---

## Memory Retrieval

什么时候读取？

什么时候写入？

什么时候删除？

这是重点。

---

# 第五阶段：Tool（Agent的手和脚）

Agent最大的能力来自Tool。

例如：

浏览器

搜索

数据库

Python

Shell

Git

Email

Calendar

Slack

Notion

GitHub

MCP

SQL

REST API

GraphQL

Agent开发很多时间都在写Tool。

---

# 第六阶段：Workflow

目前越来越重要。

例如：

```
开始

↓

判断

↓

Tool A

↓

LLM

↓

Tool B

↓

循环

↓

结束
```

需要了解：

State Machine

DAG

Workflow

Checkpoint

Retry

Timeout

Human in Loop

---

# 第七阶段：MCP（Model Context Protocol）

现在几乎已经成为Agent生态的重要协议。

需要知道：

什么是MCP？

为什么需要MCP？

MCP Client

MCP Server

Tool

Resource

Prompt

Transport

例如：

```
ChatGPT

↓

MCP Client

↓

GitHub MCP

↓

GitHub API
```

现在很多工具都有MCP：

* GitHub
* GitLab
* Figma
* Notion
* Slack
* MySQL
* PostgreSQL
* Filesystem

---

# 第八阶段：Agent框架

建议至少掌握一种。

目前比较主流：

| 框架                | 特点                |
| ----------------- | ----------------- |
| OpenAI Agents SDK | 官方SDK，设计简洁，适合生产环境 |
| LangGraph         | 最流行，支持复杂状态机和工作流   |
| CrewAI            | 多Agent协作简单易用      |
| AutoGen           | 微软推出，多Agent能力强    |
| PydanticAI        | 强类型、Python体验优秀    |

重点不是API，而是理解：

* State
* Node
* Edge
* Tool
* Memory
* Router
* Checkpoint

---

# 第九阶段：部署能力

Agent最终都要上线。

需要了解：

Docker

Docker Compose

Kubernetes（基础即可）

Nginx

Redis

PostgreSQL

RabbitMQ / Kafka（可选）

FastAPI

gRPC

WebSocket

SSE（流式输出）

---

# 第十阶段：AI工程能力

真正的Agent开发，更像AI工程。

需要掌握：

* Prompt版本管理
* Token成本控制
* 日志与Tracing
* Agent调试
* 模型路由（Model Routing）
* 缓存（Semantic Cache）
* 限流（Rate Limit）
* 评测（Evaluation）
* A/B测试
* 安全（Prompt Injection、越权调用、工具权限控制）

---

# 如果你是 C++/Qt 开发者（像你的背景）

结合你之前讨论过的 Qt、FFmpeg、Whisper.cpp、播放器和音视频项目，你有很强的系统开发基础，转向 Agent 开发时可以重点补充 AI 应用层知识，而不是重新学习底层编程。

建议路线可以是：

1. **Python基础**（FastAPI、异步、Pydantic）
2. **LLM API调用**（OpenAI兼容接口、本地模型）
3. **Prompt Engineering + Function Calling**
4. **RAG**（Embedding、向量数据库、检索优化）
5. **MCP**（工具集成标准）
6. **LangGraph 或 OpenAI Agents SDK**（建议先精通一种）
7. **多Agent、Memory、Workflow**
8. **部署与监控**（Docker、FastAPI、Redis、PostgreSQL）
9. **Agent性能优化与安全**

你的 C++、多线程、网络、RPC、FFmpeg、ZeroMQ 等经验，在开发需要调用本地工具、实时处理音视频、构建高性能 Agent 系统时会是明显优势；需要重点补足的是 LLM、RAG、Python AI 生态和 Agent 框架这几个方向。
