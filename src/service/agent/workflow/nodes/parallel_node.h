#pragma once

#include "service/agent/workflow/workflow_node.h"

#include <QList>
#include <QAtomicInt>
#include <memory>

/**
 * @brief 并行执行节点
 *
 * 同时执行多个分支节点，根据合并策略决定何时完成。
 * 每个分支获得独立的 state 副本，完成后将 artifacts 合并回主state。
 */
class ParallelNode : public IWorkflowNode
{
public:
    enum class MergeStrategy {
        WaitAll,       ///< 等全部分支完成
        WaitAny,       ///< 任一分支完成即可
        WaitMajority   ///< 多数分支完成
    };

    ParallelNode(const QString& id,
                 QList<std::shared_ptr<IWorkflowNode>> branches,
                 MergeStrategy strategy = MergeStrategy::WaitAll);

    QString id() const override { return m_id; }
    QString type() const override { return QStringLiteral("parallel"); }

    void execute(WorkflowState& state, NodeCallback done) override;

    int timeoutMs() const override { return 180000; }  // 3 分钟
    int maxRetries() const override { return 0; }

private:
    QString m_id;
    QList<std::shared_ptr<IWorkflowNode>> m_branches;
    MergeStrategy m_strategy;
};
