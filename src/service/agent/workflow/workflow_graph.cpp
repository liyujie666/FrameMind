#include "workflow_graph.h"

#include <QSet>
#include <QQueue>

// ─── Builder API ─────────────────────────────────────────────────

WorkflowGraph& WorkflowGraph::addNode(std::shared_ptr<IWorkflowNode> node)
{
    if (node) {
        m_nodes[node->id()] = std::move(node);
    }
    return *this;
}

WorkflowGraph& WorkflowGraph::setEntryNode(const QString& nodeId)
{
    m_entryNode = nodeId;
    return *this;
}

WorkflowGraph& WorkflowGraph::addEdge(const QString& from, const QString& to,
                                       const QString& condition, int priority)
{
    m_edges.append({from, to, condition, priority});
    return *this;
}

WorkflowGraph& WorkflowGraph::setMaxIterations(int max)
{
    m_maxIterations = qMax(1, max);
    return *this;
}

// ─── 查询 API ────────────────────────────────────────────────────

IWorkflowNode* WorkflowGraph::node(const QString& id) const
{
    auto it = m_nodes.find(id);
    return (it != m_nodes.end()) ? it.value().get() : nullptr;
}

std::shared_ptr<IWorkflowNode> WorkflowGraph::nodeShared(const QString& id) const
{
    return m_nodes.value(id);
}

QString WorkflowGraph::entryNodeId() const
{
    return m_entryNode;
}

QList<Edge> WorkflowGraph::outEdges(const QString& nodeId) const
{
    QList<Edge> result;
    for (const auto& edge : m_edges) {
        if (edge.fromNode == nodeId) {
            result.append(edge);
        }
    }
    // 按优先级降序排列
    std::sort(result.begin(), result.end(), [](const Edge& a, const Edge& b) {
        return a.priority > b.priority;
    });
    return result;
}

int WorkflowGraph::maxIterations() const
{
    return m_maxIterations;
}

QStringList WorkflowGraph::nodeIds() const
{
    return m_nodes.keys();
}

int WorkflowGraph::nodeCount() const
{
    return m_nodes.size();
}

// ─── 验证 ────────────────────────────────────────────────────────

bool WorkflowGraph::validate(QString* errorOut) const
{
    auto setError = [&](const QString& msg) {
        if (errorOut) *errorOut = msg;
        return false;
    };

    // 1. 入口节点存在
    if (m_entryNode.isEmpty()) {
        return setError("Entry node not set");
    }
    if (!m_nodes.contains(m_entryNode)) {
        return setError(QString("Entry node '%1' does not exist").arg(m_entryNode));
    }

    // 2. 所有边引用的节点存在（__END__ 除外）
    static const QString END_NODE = QStringLiteral("__END__");
    for (const auto& edge : m_edges) {
        if (!m_nodes.contains(edge.fromNode)) {
            return setError(QString("Edge references non-existent source node '%1'")
                               .arg(edge.fromNode));
        }
        if (edge.toNode != END_NODE && !m_nodes.contains(edge.toNode)) {
            return setError(QString("Edge references non-existent target node '%1'")
                               .arg(edge.toNode));
        }
    }

    // 3. 从入口可达所有节点（BFS）
    QSet<QString> reachable;
    QQueue<QString> queue;
    queue.enqueue(m_entryNode);
    reachable.insert(m_entryNode);

    while (!queue.isEmpty()) {
        QString current = queue.dequeue();
        for (const auto& edge : m_edges) {
            if (edge.fromNode == current && edge.toNode != END_NODE
                && !reachable.contains(edge.toNode)) {
                reachable.insert(edge.toNode);
                queue.enqueue(edge.toNode);
            }
        }
    }

    for (const auto& nodeId : m_nodes.keys()) {
        if (!reachable.contains(nodeId)) {
            return setError(QString("Node '%1' is not reachable from entry node '%2'")
                               .arg(nodeId, m_entryNode));
        }
    }

    // 4. 存在至少一条通向 __END__ 的边
    bool hasEndPath = false;
    for (const auto& edge : m_edges) {
        if (edge.toNode == END_NODE) {
            hasEndPath = true;
            break;
        }
    }
    if (!hasEndPath) {
        return setError("No path leads to __END__");
    }

    return true;
}
