#include "condition_node.h"

ConditionNode::ConditionNode(const QString& id, Evaluator evaluator)
    : m_id(id)
    , m_evaluator(std::move(evaluator))
{
}

void ConditionNode::execute(WorkflowState& state, NodeCallback done)
{
    QString route = m_evaluator(state);
    done(NodeResult{.nextRoute = route, .success = true, .error = {}});
}
