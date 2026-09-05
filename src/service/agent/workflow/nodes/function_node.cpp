#include "function_node.h"

FunctionNode::FunctionNode(const QString& id, Handler handler,
                           int timeoutMs, int maxRetries)
    : m_id(id)
    , m_handler(std::move(handler))
    , m_timeoutMs(timeoutMs)
    , m_maxRetries(maxRetries)
{
}

void FunctionNode::execute(WorkflowState& state, NodeCallback done)
{
    if (m_handler) {
        m_handler(state, std::move(done));
    } else {
        // 无处理函数，直接成功通过
        done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
    }
}
