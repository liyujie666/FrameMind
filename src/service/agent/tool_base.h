#ifndef FRAMEMIND_TOOL_BASE_H
#define FRAMEMIND_TOOL_BASE_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <functional>

#include "model/tool_types.h"

/**
 * Agent 工具接口（api-protocol.md §3 / agent-core-design.md §7）。
 *
 * 每个 Tool 提供：
 *   - name / description / parameters JSON Schema（供 LLM 使用）
 *   - executeAsync：异步执行，通过回调返回 ToolResult
 *
 * 具体 Tool 实现：
 *   - SeekAndAnalyzeTool
 *   - AnalyzeTimeRangeTool
 *   - SearchVideoContentTool
 *   - GetTranscriptTool
 *   - GetSceneInfoTool
 *   - ControlPlayerTool
 */
class ITool {
public:
    virtual ~ITool() = default;

    /// Tool 名称（唯一，对应 LLM function.name）
    virtual QString name() const = 0;

    /// 描述（用于 LLM 决策）
    virtual QString description() const = 0;

    /// JSON Schema 格式的 parameters（OpenAI Compatible）
    virtual QJsonObject parameters() const = 0;

    /**
     * 异步执行工具。
     * @param callId    LLM 分配的 tool_call_id
     * @param args      解析好的 JSON 参数
     * @param done      完成回调（在主线程触发）
     *
     * 实现约束：即使失败也必须调用 done 一次（用 ToolResult::fail）。
     */
    virtual void executeAsync(const QString& callId,
                               const QJsonObject& args,
                               std::function<void(const ToolResult&)> done) = 0;

    /// 组装 OpenAI 格式的 tool 定义（供 ToolRegistry 使用）
    QJsonObject asToolDefinition() const
    {
        QJsonObject function;
        function.insert(QStringLiteral("name"), name());
        function.insert(QStringLiteral("description"), description());
        function.insert(QStringLiteral("parameters"), parameters());

        QJsonObject tool;
        tool.insert(QStringLiteral("type"), QStringLiteral("function"));
        tool.insert(QStringLiteral("function"), function);
        return tool;
    }
};

#endif // FRAMEMIND_TOOL_BASE_H
