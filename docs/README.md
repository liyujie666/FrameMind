# 视频分析 AI Agent — 设计文档索引

本目录是 **`FrameMind`**（基于 `player_sdk` 构建的 Qt 客户端，当前仓库根目录为 `Frame_Mind/`）的设计文档集合。
按阅读顺序推荐如下：

| # | 文档 | 定位 | 何时读 |
|---|------|------|--------|
| 1 | [`development-plan.md`](./development-plan.md) | MVP 范围、里程碑（M1~M5）、技术栈与依赖版本 | 入门第一篇，先看做什么、不做什么 |
| 2 | [`dev-tasks.md`](./dev-tasks.md) | M1~M5 的可执行任务卡（含输入/输出/步骤/验收/依赖），动手时按它推进 | 写代码当天打开，按卡片勾选 |
| 3 | [`architecture-design.md`](./architecture-design.md) | 客户端整体 MVVM 架构、各层类设计、目录结构、数据库 schema | 写代码前必读 |
| 4 | [`client_ui_design.md`](./client_ui_design.md) | UI 布局、主题色值、Qt6.9 实现要点 | 做 View 层时读 |
| 5 | [`agent-core-design.md`](./agent-core-design.md) | **Agent 核心算法**：分层表示、五阶段决策循环、Video RAG、采样策略、Prompt 模板 | 做 Agent / RAG 时读（M3~M4） |
| 6 | [`api-protocol.md`](./api-protocol.md) | 客户端 ↔ LLM 后端的 OpenAI Compatible 协议、SSE 流式、Tool Calling、错误处理 | 做 `AgentService` / `NetworkClient` 时读 |
| 7 | [`agent_design.md`](./agent_design.md) | **愿景稿**（早期头脑风暴，含 P2 不做的能力） | 想了解长期方向时读，**不作为落地依据** |
| 8 | [`video-rag-type-strategy-design.md`](./video-rag-type-strategy-design.md) | **类型策略设计**：按视频类型路由 RAG 构建策略，涵盖语义单元、接口、缓存迁移、线程与验收 | 实施多类型视频 RAG 改造时读；当前行为以源码为准 |
| 9 | [`video-rag-implementation-status.md`](./video-rag-implementation-status.md) | 已实现接口、实际默认流程、测试矩阵及尚待真实素材验收的边界 | 验证本次改造或继续效果评测时读 |
| 10 | [`video-rag-unit-understanding-performance-plan.md`](./video-rag-unit-understanding-performance-plan.md) | 单元并发、滚动上下文和网格的实施阶段、依赖与验收 | 推进理解阶段性能优化时读 |
| 11 | [`video-rag-unit-analysis-contract.md`](./video-rag-unit-analysis-contract.md) | 请求身份、工作池、取消、carry 和失败合同 | 修改单元理解接口或夹具时读 |
| 12 | [`video-rag-unit-analysis-implementation-status.md`](./video-rag-unit-analysis-implementation-status.md) | P0～P3 基线、代码实现、离线测试及未验收边界 | 核查本次实现或继续网格／真实模型验收时读 |
| 13 | [`video-rag-grid-code-review.md`](./video-rag-grid-code-review.md) | P4～P5 网格、编码、兼容和诊断代码审查；本轮未运行测试 | 核查当前网格实现和待运行验收边界时读 |
| 14 | [`video-analysis-content-quality-design.md`](./video-analysis-content-quality-design.md) | 基于当前并发／carry／网格链路的内容章节、全局概览、类型总结与交互合同 | 实施视频内容质量和展示改造时读 |
| 15 | [`video-analysis-content-quality-plan.md`](./video-analysis-content-quality-plan.md) | P0～P9 实施任务、依赖、阶段出口、工作量与完整交付验收 | 按设计推进开发和核查完成范围时读 |
| 16 | [`video-analysis-content-quality-implementation-status.md`](./video-analysis-content-quality-implementation-status.md) | P0 基线、P1 编码与代码审查、实际执行记录及未验证边界 | 核查本次实施结果和继续后续阶段时读 |

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
