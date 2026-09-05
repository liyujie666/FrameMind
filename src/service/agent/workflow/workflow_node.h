#pragma once

#include <QString>
#include <functional>
#include <memory>

#include "workflow_state.h"

/**
 * @brief 节点执行结果
 */
struct NodeResult {
    QString nextRoute;   ///< 路由标签，空= 走默认边（condition 为空的边）
    bool success = true; ///< 是否成功
    QString error;       ///< 失败原因（success=false 时有效）
};

/**
 * @brief 节点完成回调类型
 *
 * 约定：即使失败也必须调用一次 done()，与 ITool::executeAsync 一致。
 * 约定：回调必须在主线程触发。
 */
using NodeCallback = std::function<void(NodeResult)>;

/**
 * @brief 工作流节点基接口
 *
 * 所有工作流节点（LLM、条件、并行、人工等）都实现此接口。
 * 节点通过异步回调方式完成执行，支持超时和重试配置。
 */
class IWorkflowNode
{
public:
    virtual ~IWorkflowNode() = default;

    /// 节点唯一标识
    virtual QString id() const = 0;

    /// 节点类型名（"llm" / "condition" / "function" / "parallel" / "human" / "tool" / "sub_workflow"）
    virtual QString type() const = 0;

    /**
     * @brief 异步执行节点逻辑
     * @param state 共享状态（可读写）
     * @param done 完成回调，必须在主线程调用且仅调用一次
     */
    virtual void execute(WorkflowState& state, NodeCallback done) = 0;

    /// 节点级超时（毫秒），0 = 无超时（由Executor 层处理）
    virtual int timeoutMs() const { return 0; }

    /// 最大重试次数，0 = 不重试
    virtual int maxRetries() const { return 0; }
};
