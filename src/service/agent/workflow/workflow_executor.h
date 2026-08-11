#pragma once

#include <QObject>
#include <QTimer>
#include <QMap>
#include <QJsonObject>
#include <functional>

#include "workflow_graph.h"
#include "workflow_state.h"

class WorkflowCheckpoint;

/**
 * @brief 工作流执行引擎
 *
 *驱动节点按Graph 定义执行，处理路由分支、超时保护、指数退避重试、取消控制。
 * 使用 Qt 事件循环调度（QTimer::singleShot），避免深层递归。
 */
class WorkflowExecutor : public QObject
{
    Q_OBJECT

public:
    explicit WorkflowExecutor(QObject* parent = nullptr);
    ~WorkflowExecutor() override;

    /// 设置 Checkpoint 管理器（可选，为nullptr 则不做断点保存）
    void setCheckpoint(WorkflowCheckpoint* checkpoint);

    /// 设置工作流 ID（用于 checkpoint 标识）
    void setWorkflowId(const QString& id);

    /// 启动工作流执行
    void run(const WorkflowGraph& graph, WorkflowState initialState);

    /// 从 Checkpoint 恢复执行
    void resume(const WorkflowGraph& graph, const QJsonObject& checkpoint);

    /// 取消执行
    void cancel();

    /// 是否正在运行
    bool isRunning() const;

signals:
    /// 进入某节点
    void nodeEntered(const QString& nodeId);

    /// 节点执行完成
    void nodeCompleted(const QString& nodeId, const NodeResult& result);

    /// 工作流正常结束
    void workflowCompleted(const WorkflowState& finalState);

    /// 工作流执行失败
    void workflowFailed(const QString& error);

    /// 人工介入节点需要输入
    void humanInputRequired(const QString& nodeId, const QString& prompt);

    /// LLM 流式输出块
    void streamingChunk(const QString& chunk);

public slots:
    /// 提供人工输入（响应 humanInputRequired）
    void provideHumanInput(const QString& nodeId, const QVariant& input);

private:
    void executeNode(const QString& nodeId);
    void route(const QString& fromNode, const NodeResult& result);
    void handleTimeout(const QString& nodeId);
    void handleRetry(const QString& nodeId, int attempt);
    void saveCheckpoint();
    void finish();
    void fail(const QString& error);

    WorkflowGraph m_graph;
    WorkflowState m_state;
    bool m_running = false;

    // 超时定时器
    QTimer* m_timeoutTimer = nullptr;
    QString m_currentTimedNode;

    // 重试计数器 nodeId →已重试次数
    QMap<QString, int> m_retryCount;

    // 全局节点执行计数（防无限循环）
    int m_totalNodeExecutions = 0;

    // Checkpoint
    WorkflowCheckpoint* m_checkpoint = nullptr;
    QString m_workflowId;
};
