#include "service/agent/tool_registry.h"

ToolRegistry::ToolRegistry(QObject* parent) : QObject(parent) {}
ToolRegistry::~ToolRegistry() = default;

void ToolRegistry::registerTool(std::unique_ptr<ITool> tool)
{
    if (!tool) return;
    const QString name = tool->name();
    if (name.isEmpty()) return;
    // Replace existing tool with the same name
    if (m_tools.contains(name)) {
        auto it = std::find_if(m_toolOwner.begin(), m_toolOwner.end(),
                               [&](const std::unique_ptr<ITool>& t) {
                                   return t->name() == name;
                               });
        if (it != m_toolOwner.end()) m_toolOwner.erase(it);
    }
    m_tools[name] = tool.get();
    m_toolOwner.push_back(std::move(tool));
}

ITool* ToolRegistry::getTool(const QString& name) const
{
    return m_tools.value(name, nullptr);
}

QJsonArray ToolRegistry::allDefinitions() const
{
    QJsonArray arr;
    for (const auto& tool : m_toolOwner) {
        arr.append(tool->asToolDefinition());
    }
    return arr;
}

void ToolRegistry::clear()
{
    m_tools.clear();
    m_toolOwner.clear();
}
