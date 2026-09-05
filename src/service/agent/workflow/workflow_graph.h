#pragma once

#include <QString>
#include <QMap>
#include <QList>
#include <memory>

#include "workflow_node.h"

/**
 * @brief 图中的有向边
 */
struct Edge {
    QString fromNode;      ///< 起始节点 ID
    QString toNode;        ///< 目标节点 ID（"__END__" 表示工作流结束）
    QString condition;     ///< 路由条件标签，空 = 默认无条件边
    int priority = 0;      ///< 多条边匹配时的优先级（越高越优先）
};

/**
 * @brief 工作流有向图定义
 *
 * 使用 Builder 模式构建 DAG，支持条件路由、循环（受maxIterations 保护）。
 * 构建完成后调用 validate() 检测图的合法性。
 */
class WorkflowGraph
{
public:
    WorkflowGraph() = default;

    // ─── Builder API ─────────────────────────────────────────────

    /// 添加节点
    WorkflowGraph& addNode(std::shared_ptr<IWorkflowNode> node);

    /// 设置入口节点
    WorkflowGraph& setEntryNode(const QString& nodeId);

    /// 添加有向边
    WorkflowGraph& addEdge(const QString& from, const QString& to,
                           const QString& condition = {},
                           int priority = 0);

    /// 设置最大迭代次数（防无限循环）
    WorkflowGraph& setMaxIterations(int max);

    // ─── 查询 API ───────────────────────────────────────────────

    /// 获取节点（不存在返回 nullptr）
    IWorkflowNode* node(const QString& id) const;

    /// 获取节点的shared_ptr
    std::shared_ptr<IWorkflowNode> nodeShared(const QString& id) const;

    /// 入口节点 ID
    QString entryNodeId() const;

    /// 获取某节点的所有出边
    QList<Edge> outEdges(const QString& nodeId) const;

    /// 最大迭代次数
    int maxIterations() const;

    /// 所有节点 ID 列表
    QStringList nodeIds() const;

    /// 节点总数
    int nodeCount() const;

    // ─── 验证 ───────────────────────────────────────────────────

    /**
     * @brief 验证图的合法性
     * 检查：
     * 1. 入口节点存在
     * 2. 所有边引用的节点存在（toNode="__END__" 除外）
     * 3. 从入口可达所有节点（BFS）
     * 4. 存在至少一条通向__END__ 的路径
     *
     * @param errorOut 如果验证失败，写入错误描述
     * @return true = 合法
     */
    bool validate(QString* errorOut = nullptr) const;

private:
    QMap<QString, std::shared_ptr<IWorkflowNode>> m_nodes;
    QList<Edge> m_edges;
    QString m_entryNode;
    int m_maxIterations = 20;
};
