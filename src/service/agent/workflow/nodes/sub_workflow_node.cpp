#include "sub_workflow_node.h"
#include "service/agent/workflow/workflow_executor.h"

SubWorkflowNode::SubWorkflowNode(const QString& id, WorkflowGraph subGraph,
                                 QStringList mergeKeys)
    : m_id(id)
    , m_subGraph(std::move(subGraph))
    , m_mergeKeys(std::move(mergeKeys))
{
}

void SubWorkflowNode::execute(WorkflowState& state, NodeCallback done)
{
    // 创建子 state（继承父state 的数据）
    WorkflowState subState;
    for (const auto& key : state.keys()) {
        subState.set(key, state.get(key));
    }
    for (const auto& msg : state.messages()) {
        subState.addMessage(msg);
    }

    // 创建子执行器
    auto* subExecutor = new WorkflowExecutor();

    // 连接完成信号
    QObject::connect(subExecutor, &WorkflowExecutor::workflowCompleted,
                     subExecutor, [this, &state, done, subExecutor](const WorkflowState& subFinalState) {
        // 合并 artifacts
        state.mergeArtifacts(subFinalState.allArtifacts());

        // 合并指定 keys
        for (const auto& key : m_mergeKeys) {
            if (subFinalState.contains(key)) {
                state.set(key, subFinalState.get(key));
            }
        }

        subExecutor->deleteLater();
        done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
    });

    QObject::connect(subExecutor, &WorkflowExecutor::workflowFailed,
                     subExecutor, [done, subExecutor](const QString& error) {
        subExecutor->deleteLater();
        done(NodeResult{.nextRoute = {}, .success = false, .error = error});
    });

    // 启动子工作流
    subExecutor->run(m_subGraph, std::move(subState));
}
