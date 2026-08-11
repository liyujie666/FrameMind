#pragma once

#include "service/agent/workflow/workflow_node.h"
#include "service/agent/tool_base.h"
#include "service/agent/tool_registry.h"

/**
 * @brief 单工具直接执行节点
 *
 * 绕过 LLM 决策，直接调用指定 ITool。
 * 从 WorkflowState 取参数，执行后将结果写入 artifacts。
 */
class ToolNode : public IWorkflowNode
{
public:
    /**
     * @param id 节点 ID
     * @param toolName 要执行的工具名称
     * @param registry 工具注册表（用于查找工具实例）
     * @param argsKey state 中存放工具参数的 key（默认 "tool_args"）
     * @param resultKey 结果写入 artifacts 的 key（默认同toolName）
     */
    ToolNode(const QString& id, const QString& toolName,
             ToolRegistry* registry,
             const QString& argsKey = QStringLiteral("tool_args"),
             const QString& resultKey = {});

    QString id() const override { return m_id; }
    QString type() const override { return QStringLiteral("tool"); }

    void execute(WorkflowState& state, NodeCallback done) override;

    int timeoutMs() const override { return 60000; }  // 1 分钟
    int maxRetries() const override { return 1; }

private:
    QString m_id;
    QString m_toolName;
    ToolRegistry* m_registry;
    QString m_argsKey;
    QString m_resultKey;
};
