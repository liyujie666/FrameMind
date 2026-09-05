#pragma once

#include "service/agent/workflow/workflow_node.h"
#include "service/agent/workflow/workflow_graph.h"

class WorkflowExecutor;

/**
 * @brief 子工作流嵌套节点
 *
 * 持有一个子WorkflowGraph，执行时创建子WorkflowExecutor 运行子图。
 * 子图完成后将子state 的 artifacts 和指定keys 合并回父 state。
 */
class SubWorkflowNode : public IWorkflowNode
{
public:
    /**
     * @param id 节点 ID
     * @param subGraph 子工作流图
     * @param mergeKeys 子state 完成后需要合并回父 state 的 data keys（空=仅合并 artifacts）
     */
    SubWorkflowNode(const QString& id, WorkflowGraph subGraph,
                    QStringList mergeKeys = {});

    QString id() const override { return m_id; }
    QString type() const override { return QStringLiteral("sub_workflow"); }

    void execute(WorkflowState& state, NodeCallback done) override;

    int timeoutMs() const override { return 300000; }  // 5 分钟
    int maxRetries() const override { return 0; }

private:
    QString m_id;
    WorkflowGraph m_subGraph;
    QStringList m_mergeKeys;
};
