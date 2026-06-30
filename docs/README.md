# 视频分析 AI Agent — 设计文档索引

本目录是 **`video_ai_agent`**（基于 `player_sdk` 构建的 Qt 客户端）的设计文档集合。
按阅读顺序推荐如下：

| # | 文档 | 定位 | 何时读 |
|---|------|------|--------|
| 1 | [`development-plan.md`](./development-plan.md) | MVP 范围、里程碑（M1~M5）、技术栈与依赖版本 | 入门第一篇，先看做什么、不做什么 |
| 2 | [`architecture-design.md`](./architecture-design.md) | 客户端整体 MVVM 架构、各层类设计、目录结构、数据库 schema | 写代码前必读 |
| 3 | [`client_ui_design.md`](./client_ui_design.md) | UI 布局、主题色值、Qt6.9 实现要点 | 做 View 层时读 |
| 4 | [`agent-core-design.md`](./agent-core-design.md) | **Agent 核心算法**：分层表示、五阶段决策循环、Video RAG、采样策略、Prompt 模板 | 做 Agent / RAG 时读（M3~M4） |
| 5 | [`api-protocol.md`](./api-protocol.md) | 客户端 ↔ LLM 后端的 OpenAI Compatible 协议、SSE 流式、Tool Calling、错误处理 | 做 `AgentService` / `NetworkClient` 时读 |
| 6 | [`agent_design.md`](./agent_design.md) | **愿景稿**（早期头脑风暴，含 P2 不做的能力） | 想了解长期方向时读，**不作为落地依据** |

## 文档一致性约定

为避免文档间矛盾，下列条目以指定文档为「唯一真相源 (SSoT)」，其他文档若涉及必须引用并保持一致：

| 主题 | 真相源 |
|------|--------|
| MVP 范围、里程碑、是否纳入某个能力 | `development-plan.md` |
| 模块/类设计、目录结构、数据库 schema | `architecture-design.md` |
| Agent 决策循环、Prompt、Tool 行为语义 | `agent-core-design.md` |
| Tool 列表（JSON Schema）、HTTP/SSE 协议、错误码 | `api-protocol.md` |
| UI 布局、主题色值 | `client_ui_design.md` |

> 修改文档时请同步更新引用方，并在 PR / commit 信息中说明影响范围。
