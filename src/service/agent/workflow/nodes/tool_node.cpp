#include "tool_node.h"

#include <QJsonDocument>
#include <QUuid>

ToolNode::ToolNode(const QString& id, const QString& toolName,
                   ToolRegistry* registry,
                   const QString& argsKey, const QString& resultKey)
    : m_id(id)
    , m_toolName(toolName)
    , m_registry(registry)
    , m_argsKey(argsKey)
    , m_resultKey(resultKey.isEmpty() ? toolName : resultKey)
{
}

void ToolNode::execute(WorkflowState& state, NodeCallback done)
{
    if (!m_registry) {
        done(NodeResult{.nextRoute = {}, .success = false,
                        .error = "ToolRegistry is null"});
        return;
    }

    ITool* tool = m_registry->tool(m_toolName);
    if (!tool) {
        done(NodeResult{.nextRoute = {}, .success = false,
                        .error = QString("Tool '%1' not found").arg(m_toolName)});
        return;
    }

    // 从 state 获取工具参数
    QJsonObject args;
    QVariant argsVar = state.get(m_argsKey);
    if (argsVar.canConvert<QJsonObject>()) {
        args = argsVar.toJsonObject();
    } else if (argsVar.typeId() == QMetaType::QString) {
        args = QJsonDocument::fromJson(argsVar.toString().toUtf8()).object();
    }

    QString callId = QUuid::createUuid().toString(QUuid::WithoutBraces);

    tool->executeAsync(callId, args, [this, &state, done](const ToolResult& result) {
        if (result.success) {
            state.addArtifact(m_resultKey, result.data);
            done(NodeResult{.nextRoute = {}, .success = true, .error = {}});
        } else {
            done(NodeResult{.nextRoute = {}, .success = false, .error = result.error});
        }
    });
}
