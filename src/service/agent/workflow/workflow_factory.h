#pragma once

#include "workflow_graph.h"

#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QMap>
#include <functional>
#include <memory>

class AgentService;
class ToolOrchestrator;
class ToolRegistry;

/**
 * @brief 工作流工厂
 *
 * 从 JSON 配置构建 WorkflowGraph。支持注册自定义节点工厂。
 * 内置节点类型：llm、condition、function、parallel、human、tool、sub_workflow。
 */
class WorkflowFactory
{
public:
    /// 依赖注入上下文
    struct Dependencies {
        AgentService* agentService = nullptr;
        ToolOrchestrator* orchestrator = nullptr;
        ToolRegistry* toolRegistry = nullptr;
    };

    /// 自定义节点创建函数
    using NodeCreator = std::function<std::shared_ptr<IWorkflowNode>(
        const QString& id, const QJsonObject& config, const Dependencies& deps)>;

    WorkflowFactory();

    /// 设置依赖
    void setDependencies(Dependencies deps);

    /**
     * @brief 从 JSON 构建工作流图
     * @param json JSON 配置对象
     * @return 构建好的 WorkflowGraph
     *
     * JSON 格式：
     * {
     *   "name": "workflow_name",
     *   "maxIterations": 20,
     *   "entry": "node_id",
     *   "nodes": [ { "id": "...", "type": "...", ... }, ... ],
     *   "edges": [ { "from": "...", "to": "...", "condition": "..." }, ... ]
     * }
     */
    WorkflowGraph fromJson(const QJsonObject& json);

    /**
     * @brief 从 JSON 文件加载工作流
     * @param filePath JSON 文件路径
     * @return 构建好的 WorkflowGraph
     */
    WorkflowGraph fromFile(const QString& filePath);

    /// 注册自定义节点类型工厂
    void registerNodeFactory(const QString& type, NodeCreator creator);

private:
    std::shared_ptr<IWorkflowNode> createNode(const QJsonObject& nodeDef);

    // 内置节点创建
    std::shared_ptr<IWorkflowNode> createLLMNode(const QString& id, const QJsonObject& config);
    std::shared_ptr<IWorkflowNode> createConditionNode(const QString& id, const QJsonObject& config);
    std::shared_ptr<IWorkflowNode> createFunctionNode(const QString& id, const QJsonObject& config);
    std::shared_ptr<IWorkflowNode> createToolNode(const QString& id, const QJsonObject& config);
    std::shared_ptr<IWorkflowNode> createHumanNode(const QString& id, const QJsonObject& config);

    Dependencies m_deps;
    QMap<QString, NodeCreator> m_customFactories;

    /// 已注册的 function handler（通过 registerFunctionHandler 注入）
    QMap<QString, std::function<void(WorkflowState&, NodeCallback)>> m_functionHandlers;

public:
    /// 注册函数处理器（用于JSON中type="function" 的节点）
    void registerFunctionHandler(const QString& name,
                                 std::function<void(WorkflowState&, NodeCallback)> handler);
};
