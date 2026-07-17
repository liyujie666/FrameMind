#include "service/agent/tool_registry.h"

ToolRegistry::ToolRegistry(QObject* parent) : QObject(parent) {}
ToolRegistry::~ToolRegistry() = default;

void ToolRegistry::registerTool(std::unique_ptr<ITool> tool)
{
    if (!tool) return;
    const QString name = tool->name();
    if (name.isEmpty()) return;
    m_tools[name] = std::move(tool);
}

ITool* ToolRegistry::getTool(const QString& name) const
{
    const auto it = m_tools.constFind(name);
    return (it == m_tools.constEnd()) ? nullptr : it.value().get();
}

QJsonArray ToolRegistry::allDefinitions() const
{
    QJsonArray arr;
    for (auto it = m_tools.constBegin(); it != m_tools.constEnd(); ++it) {
        arr.append(it.value()->asToolDefinition());
    }
    return arr;
}

void ToolRegistry::clear()
{
    m_tools.clear();
}
