#include "parallel_node.h"

#include <QMutex>
#include <QMutexLocker>

ParallelNode::ParallelNode(const QString& id,
                           QList<std::shared_ptr<IWorkflowNode>> branches,
                           MergeStrategy strategy)
    : m_id(id)
    , m_branches(std::move(branches))
    , m_strategy(strategy)
{
}

void ParallelNode::execute(WorkflowState& state, NodeCallback done)
{
    if (m_branches.isEmpty()) {
        done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
        return;
    }

    int totalBranches = m_branches.size();
    int targetCount = 0;

    switch (m_strategy) {
    case MergeStrategy::WaitAll:
        targetCount = totalBranches;
        break;
    case MergeStrategy::WaitAny:
        targetCount = 1;
        break;
    case MergeStrategy::WaitMajority:
        targetCount = (totalBranches / 2) + 1;
        break;
    }

    // 共享计数器和互斥锁
    auto completedCount = std::make_shared<QAtomicInt>(0);
    auto callbackFired = std::make_shared<QAtomicInt>(0);
    auto mutex = std::make_shared<QMutex>();
    auto mergedArtifacts = std::make_shared<QJsonObject>();
    auto errors = std::make_shared<QStringList>();

    for (int i = 0; i < totalBranches; ++i) {
        auto& branch = m_branches[i];

        // 每个分支获得独立state副本（共享artifacts引用通过合并处理）
        WorkflowState branchState;
        // 复制 data keys
        for (const auto& key : state.keys()) {
            branchState.set(key, state.get(key));
        }
        // 复制 messages
        for (const auto& msg : state.messages()) {
            branchState.addMessage(msg);
        }

        branch->execute(branchState, [=, &state](NodeResult result) {
            QMutexLocker locker(mutex.get());

            if (result.success) {
                // 合并分支 artifacts
                QJsonObject branchArtifacts = branchState.allArtifacts();
                for (auto it = branchArtifacts.begin(); it != branchArtifacts.end(); ++it) {
                    (*mergedArtifacts)[it.key()] = it.value();
                }
            } else {
                errors->append(result.error);
            }

            int completed = completedCount->fetchAndAddRelaxed(1) + 1;

            // 检查是否达到合并条件
            if (completed >= targetCount && callbackFired->testAndSetRelaxed(0, 1)) {
                // 将合并后的 artifacts 写回主 state
                state.mergeArtifacts(*mergedArtifacts);

                bool success = errors->isEmpty() ||
                               (m_strategy != MergeStrategy::WaitAll);

                done(NodeResult{
                    .nextRoute = {},
                    .success = success,
                    .error = errors->isEmpty() ? QString() : errors->join("; ")
                });
            }
        });
    }
}
