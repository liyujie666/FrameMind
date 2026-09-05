#pragma once

#include "service/agent/workflow/workflow_node.h"

#include <functional>

/**
 * @brief 条件路由节点
 *
 * 同步执行条件判断函数，返回路由标签。
 * 执行器根据返回值匹配对应的出边 condition 进行路由。
 */
class ConditionNode : public IWorkflowNode
{
public:
    /// 条件评估函数：根据当前状态返回路由标签
    using Evaluator = std::function<QString(const WorkflowState&)>;

    ConditionNode(const QString& id, Evaluator evaluator);

    QString id() const override { return m_id; }
    QString type() const override { return QStringLiteral("condition"); }

    void execute(WorkflowState& state, NodeCallback done) override;

    // 条件节点是同步的，无需超时和重试
    int timeoutMs() const override { return 0; }
    int maxRetries() const override { return 0; }

private:
    QString m_id;
    Evaluator m_evaluator;
};
