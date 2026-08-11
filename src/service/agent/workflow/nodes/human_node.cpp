#include "human_node.h"

HumanNode::HumanNode(const QString& id, const QString& prompt, const QString& inputKey)
    : m_id(id)
    , m_prompt(prompt)
    , m_inputKey(inputKey)
{
}

void HumanNode::execute(WorkflowState& state, NodeCallback done)
{
    // 保存回调和状态指针，等待外部提供输入
    m_pendingCallback = std::move(done);
    m_pendingState = &state;

    // 将 prompt 写入 state，供Executor 信号转发给 UI
    state.set("__human_prompt__", m_prompt);
    state.set("__human_node_id__", m_id);
}

void HumanNode::receiveInput(const QVariant& input)
{
    if (!m_pendingCallback || !m_pendingState) return;

    // 写入人工输入
    m_pendingState->set(m_inputKey, input);

    // 清除待处理标记
    m_pendingState->remove("__human_prompt__");
    m_pendingState->remove("__human_node_id__");

    auto callback = std::move(m_pendingCallback);
    m_pendingState = nullptr;

    callback(NodeResult{.nextRoute = {}, .success = true, .error = {}});
}
