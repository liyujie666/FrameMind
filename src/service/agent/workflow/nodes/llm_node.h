#pragma once

#include "service/agent/workflow/workflow_node.h"

#include <QStringList>
#include <functional>

class AgentService;
class ToolOrchestrator;

/**
 * @brief LLM 推理节点
 *
 * 调用 LLM 进行推理，可选启用 Tool Calling（通过 ToolOrchestrator多轮循环）。
 * 结果写入 state 的 "answer" key和 artifacts 的 "tool_trace"。
 */
class LLMNode : public IWorkflowNode
{
public:
    struct Config {
        QString systemPrompt;             ///< 系统提示（覆盖默认）
        QString modelOverride;            ///< 指定模型（空=使用默认）
        bool enableToolCalling = false;   ///< 是否启用 Tool Calling
        int maxToolRounds = 5;            ///< Tool Calling 最大轮次
        QStringList allowedTools;         ///< 允许的工具列表（空=全部）
        QString questionKey = "question"; ///< 从 state 获取问题的 key
        QString answerKey = "answer";     ///< 写入答案的 state key
        int timeout = 120000;             ///< 超时 ms（默认 2 分钟）
    };

    LLMNode(const QString& id, Config config,
            AgentService* agentService,
            ToolOrchestrator* orchestrator = nullptr);

    QString id() const override { return m_id; }
    QString type() const override { return QStringLiteral("llm"); }

    void execute(WorkflowState& state, NodeCallback done) override;

    int timeoutMs() const override { return m_config.timeout; }
    int maxRetries() const override { return 1; }
    
    /// 设置流式输出回调（由 WorkflowExecutor 调用）
    void setStreamingCallback(std::function<void(const QString&)> callback) {
        m_streamingCallback = std::move(callback);
    }

private:
    void executeWithTools(WorkflowState& state, NodeCallback done);
    void executeDirectLLM(WorkflowState& state, NodeCallback done);

    QString m_id;
    Config m_config;
    AgentService* m_agentService;
    ToolOrchestrator* m_orchestrator;
    std::function<void(const QString&)> m_streamingCallback;
};
