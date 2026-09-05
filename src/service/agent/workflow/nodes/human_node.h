#pragma once

#include "service/agent/workflow/workflow_node.h"

/**
 * @brief 人工介入节点
 *
 * 执行时挂起工作流，通过 WorkflowExecutor 发出humanInputRequired 信号，
 * 等待外部调用 receiveInput() 提供人工输入后继续。
 */
class HumanNode : public IWorkflowNode
{
public:
    /**
     * @param id 节点 ID
     * @param prompt 显示给用户的提示信息
     * @param inputKey 人工输入写入 state 的 key
     */
    HumanNode(const QString& id, const QString& prompt,
              const QString& inputKey = QStringLiteral("human_input"));

    QString id() const override { return m_id; }
    QString type() const override { return QStringLiteral("human"); }

    void execute(WorkflowState& state, NodeCallback done) override;

    ///5 分钟等待人工输入
    int timeoutMs() const override { return 300000; }
    int maxRetries() const override { return 0; }

    /// 接收外部提供的人工输入
    void receiveInput(const QVariant& input);

    /// 获取提示信息
    QString prompt() const { return m_prompt; }

private:
    QString m_id;
    QString m_prompt;
    QString m_inputKey;
    NodeCallback m_pendingCallback;
    WorkflowState* m_pendingState = nullptr;
};
