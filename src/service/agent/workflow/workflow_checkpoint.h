#pragma once

#include <QString>
#include <QJsonObject>
#include <optional>

#include "workflow_state.h"

/**
 * @brief 工作流断点持久化管理器
 *
 * 将工作流执行状态保存到 SQLite，支持从断点恢复和过期清理。
 * 使用 DatabaseManager 单例操作 workflow_checkpoints 表。
 */
class WorkflowCheckpoint
{
public:
    WorkflowCheckpoint();

    /// 初始化（建表）
    void initialize();

    /**
     * @brief 保存断点
     * @param workflowId 工作流唯一标识
     * @param state 当前状态
     * @param currentNodeId 当前执行到的节点
     */
    void save(const QString& workflowId, const WorkflowState& state,
              const QString& currentNodeId);

    /**
     * @brief 加载断点
     * @param workflowId 工作流标识
     * @return 断点 JSON（包含 state + currentNode），不存在返回 nullopt
     */
    std::optional<QJsonObject> load(const QString& workflowId);

    /**
     * @brief 删除断点
     * @param workflowId 工作流标识
     */
    void remove(const QString& workflowId);

    /**
     * @brief 清理过期断点
     * @param maxAgeDays 最大保留天数（默认 7 天）
     */
    void cleanup(int maxAgeDays = 7);
};
