#include "workflow_executor.h"
#include "workflow_checkpoint.h"
#include "nodes/llm_node.h"

#include <QMetaObject>
#include <QDebug>

static const QString END_NODE = QStringLiteral("__END__");

WorkflowExecutor::WorkflowExecutor(QObject* parent)
    : QObject(parent)
{
    m_timeoutTimer = new QTimer(this);
    m_timeoutTimer->setSingleShot(true);
    connect(m_timeoutTimer, &QTimer::timeout, this, [this]() {
        handleTimeout(m_currentTimedNode);
    });
}

WorkflowExecutor::~WorkflowExecutor()
{
    cancel();
}

void WorkflowExecutor::setCheckpoint(WorkflowCheckpoint* checkpoint)
{
    m_checkpoint = checkpoint;
}

void WorkflowExecutor::setWorkflowId(const QString& id)
{
    m_workflowId = id;
}

void WorkflowExecutor::run(const WorkflowGraph& graph, WorkflowState initialState)
{
    if (m_running) {
        qWarning() << "[WorkflowExecutor] Already running, call cancel() first";
        return;
    }

    // 验证图
    QString validateError;
    if (!graph.validate(&validateError)) {
        QTimer::singleShot(0, this, [this, validateError]() {
            fail(QString("Graph validation failed: %1").arg(validateError));
        });
        return;
    }

    m_graph = graph;
    m_state = std::move(initialState);
    m_running = true;
    m_totalNodeExecutions = 0;
    m_retryCount.clear();
    m_state.resetCancel();

    // 通过事件循环启动，避免在 run() 调用栈中同步执行
    QTimer::singleShot(0, this, [this]() {
        executeNode(m_graph.entryNodeId());
    });
}

void WorkflowExecutor::resume(const WorkflowGraph& graph, const QJsonObject& checkpoint)
{
    if (m_running) {
        qWarning() << "[WorkflowExecutor] Already running, call cancel() first";
        return;
    }

    m_graph = graph;
    m_state = WorkflowState::deserialize(checkpoint["state"].toObject());
    m_running = true;
    m_totalNodeExecutions = 0;
    m_retryCount.clear();
    m_state.resetCancel();

    QString resumeNode = checkpoint["currentNode"].toString();
    if (resumeNode.isEmpty() || !m_graph.node(resumeNode)) {
        fail("Invalid checkpoint: missing or unknown currentNode");
        return;
    }

    QTimer::singleShot(0, this, [this, resumeNode]() {
        executeNode(resumeNode);
    });
}

void WorkflowExecutor::cancel()
{
    if (!m_running) return;

    m_state.cancel();
    m_timeoutTimer->stop();
    m_running = false;

    emit workflowFailed("Workflow cancelled by user");
}

bool WorkflowExecutor::isRunning() const
{
    return m_running;
}

void WorkflowExecutor::provideHumanInput(const QString& nodeId, const QVariant& input)
{
    Q_UNUSED(nodeId)
    m_state.set("__human_input__", input);
}

// ─── 内部实现 ────────────────────────────────────────────────────

void WorkflowExecutor::executeNode(const QString& nodeId)
{
    if (!m_running || m_state.isCancelled()) {
        fail("Workflow cancelled");
        return;
    }

    // 防无限循环
    ++m_totalNodeExecutions;
    int maxExecutions = m_graph.maxIterations() * m_graph.nodeCount();
    if (m_totalNodeExecutions > maxExecutions) {
        fail(QString("Exceeded max executions (%1). Possible infinite loop.")
                .arg(maxExecutions));
        return;
    }

    auto* node = m_graph.node(nodeId);
    if (!node) {
        fail(QString("Node '%1' not found in graph").arg(nodeId));
        return;
    }

    m_state.setCurrentNode(nodeId);
    m_state.incrementIteration();
    emit nodeEntered(nodeId);

    // 保存断点
    saveCheckpoint();

    // 如果是 LLMNode，设置流式输出回调
    if (node->type() == QStringLiteral("llm")) {
        auto* llmNode = dynamic_cast<LLMNode*>(node);
        if (llmNode) {
            llmNode->setStreamingCallback([this](const QString& chunk) {
                emit streamingChunk(chunk);
            });
        }
    }

    // 启动超时定时器
    if (node->timeoutMs() > 0) {
        m_currentTimedNode = nodeId;
        m_timeoutTimer->start(node->timeoutMs());
    }

    // 异步执行节点
    node->execute(m_state, [this, nodeId](NodeResult result) {
        // 确保在主线程处理
        QMetaObject::invokeMethod(this, [this, nodeId, result]() {
            if (!m_running) return;

            m_timeoutTimer->stop();

            if (!result.success) {
                auto* node = m_graph.node(nodeId);
                int maxRetry = node ? node->maxRetries() : 0;
                int currentRetry = m_retryCount.value(nodeId, 0);

                if (currentRetry < maxRetry) {
                    handleRetry(nodeId, currentRetry + 1);
                    return;
                }

                // 重试耗尽，工作流失败
                fail(QString("Node '%1' failed after %2 retries: %3")
                         .arg(nodeId)
                         .arg(currentRetry)
                         .arg(result.error));
                return;
            }

            emit nodeCompleted(nodeId, result);
            route(nodeId, result);
        }, Qt::QueuedConnection);
    });
}

void WorkflowExecutor::route(const QString& fromNode, const NodeResult& result)
{
    QList<Edge> edges = m_graph.outEdges(fromNode);

    if (edges.isEmpty()) {
        // 没有出边，视为结束
        finish();
        return;
    }

    // 路由匹配逻辑：
    // 1. 如果 result.nextRoute 非空，寻找 condition 匹配的边
    // 2. 否则走默认边（condition 为空的边）
    QString targetNode;

    if (!result.nextRoute.isEmpty()) {
        // 精确匹配条件边
        for (const auto& edge : edges) {
            if (edge.condition == result.nextRoute) {
                targetNode = edge.toNode;
                break;
            }
        }
    }

    // 如果没匹配到条件边，走默认边
    if (targetNode.isEmpty()) {
        for (const auto& edge : edges) {
            if (edge.condition.isEmpty()) {
                targetNode = edge.toNode;
                break;
            }
        }
    }

    // 都没匹配到，取第一条边
    if (targetNode.isEmpty()) {
        targetNode = edges.first().toNode;
    }

    // 检查是否到终点
    if (targetNode == END_NODE) {
        finish();
        return;
    }

    // 通过事件循环跳转到下一节点，避免栈溢出
    QTimer::singleShot(0, this, [this, targetNode]() {
        executeNode(targetNode);
    });
}

void WorkflowExecutor::handleTimeout(const QString& nodeId)
{
    if (!m_running) return;

    auto* node = m_graph.node(nodeId);
    int maxRetry = node ? node->maxRetries() : 0;
    int currentRetry = m_retryCount.value(nodeId, 0);

    if (currentRetry < maxRetry) {
        handleRetry(nodeId, currentRetry + 1);
    } else {
        fail(QString("Node '%1' timed out after %2ms (retries exhausted)")
                 .arg(nodeId)
                 .arg(node ? node->timeoutMs() : 0));
    }
}

void WorkflowExecutor::handleRetry(const QString& nodeId, int attempt)
{
    m_retryCount[nodeId] = attempt;

    // 指数退避：1s, 2s, 4s, 8s...
    int delayMs = 1000 * (1 << (attempt - 1));
    delayMs = qMin(delayMs, 30000);  // 最大 30s

    qDebug() << "[WorkflowExecutor] Retrying node" << nodeId
             << "attempt" << attempt << "delay" << delayMs << "ms";

    QTimer::singleShot(delayMs, this, [this, nodeId]() {
        executeNode(nodeId);
    });
}

void WorkflowExecutor::saveCheckpoint()
{
    if (!m_checkpoint || m_workflowId.isEmpty()) return;

    m_checkpoint->save(m_workflowId, m_state, m_state.currentNodeId());
}

void WorkflowExecutor::finish()
{
    m_running = false;
    m_timeoutTimer->stop();
    emit workflowCompleted(m_state);
}

void WorkflowExecutor::fail(const QString& error)
{
    m_running = false;
    m_timeoutTimer->stop();
    emit workflowFailed(error);
}
