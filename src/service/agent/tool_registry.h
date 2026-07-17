#ifndef FRAMEMIND_TOOL_REGISTRY_H
#define FRAMEMIND_TOOL_REGISTRY_H

#include <QObject>
#include <QString>
#include <QHash>
#include <QJsonArray>
#include <memory>

#include "service/agent/tool_base.h"

/**
 * Tool 注册表：管理 Agent 可用的所有工具。
 *
 * - registerTool: 注册工具（同名后注册的覆盖前面的）
 * - getTool: 按名字查找
 * - allDefinitions: 输出全部工具的 OpenAI JSON 定义数组（供 sendMessage 携带）
 */
class ToolRegistry : public QObject {
    Q_OBJECT
public:
    explicit ToolRegistry(QObject* parent = nullptr);
    ~ToolRegistry() override;

    /// 注册工具（转让所有权）
    void registerTool(std::unique_ptr<ITool> tool);

    /// 按名字查找工具（未找到返回 nullptr）
    ITool* getTool(const QString& name) const;

    /// 输出全部工具定义（api-protocol.md §3.1 的 tools 数组）
    QJsonArray allDefinitions() const;

    /// 工具数量
    int size() const { return m_tools.size(); }

    /// 清空
    void clear();

private:
    QHash<QString, std::unique_ptr<ITool>> m_tools;
};

#endif // FRAMEMIND_TOOL_REGISTRY_H
