#pragma once

#include "service/agent/workflow/workflow_node.h"

#include <functional>

/**
 * @brief 通用函数节点
 *
 * 适配器模式，将已有组件（PerceptionStrategy、ReflectionEngine、VideoRAGRetriever 等）
 * 包装为工作流节点。通过传入 Handler 函数实现任意逻辑。
 */
class FunctionNode : public IWorkflowNode
{
public:
    /// 处理函数类型：接收状态和完成回调
    using Handler = std::function<void(WorkflowState&, NodeCallback)>;

    FunctionNode(const QString& id, Handler handler,
                 int timeoutMs = 0, int maxRetries = 0);

    QString id() const override { return m_id; }
    QString type() const override { return QStringLiteral("function"); }

    void execute(WorkflowState& state, NodeCallback done) override;

    int timeoutMs() const override { return m_timeoutMs; }
    int maxRetries() const override { return m_maxRetries; }

private:
    QString m_id;
    Handler m_handler;
    int m_timeoutMs;
    int m_maxRetries;
};
