#ifndef FRAMEMIND_TOOL_TYPES_H
#define FRAMEMIND_TOOL_TYPES_H

#include <QString>
#include <QJsonObject>
#include <QMetaType>

/**
 * Tool Calling 相关类型（对齐 api-protocol.md §3 与 agent-core-design.md §7）。
 */

/// LLM 决定要调用的工具（从 SSE tool_calls 增量解析而来）
struct ToolCall {
    QString id;                // "call_abc123"
    QString name;              // "seek_and_analyze" 等
    QJsonObject arguments;     // 完整 JSON 参数
    QString validationError;   // 协议或参数解析错误，执行器将其作为 tool 失败结果回填

    bool isValid() const
    {
        return !id.isEmpty() && !name.isEmpty() && validationError.isEmpty();
    }
};

/// 工具执行结果
struct ToolResult {
    QString     toolCallId;    // 对应的 ToolCall::id
    QString     toolName;
    bool        success = false;
    QJsonObject data;          // 结构化结果，会被序列化为 tool role 消息 content
    QString     error;         // success=false 时的错误说明

    static ToolResult ok(const QString& callId, const QString& name, const QJsonObject& data)
    {
        ToolResult r;
        r.toolCallId = callId;
        r.toolName   = name;
        r.success    = true;
        r.data       = data;
        return r;
    }

    static ToolResult fail(const QString& callId, const QString& name, const QString& err)
    {
        ToolResult r;
        r.toolCallId = callId;
        r.toolName   = name;
        r.success    = false;
        r.error      = err;
        return r;
    }
};

Q_DECLARE_METATYPE(ToolCall)
Q_DECLARE_METATYPE(ToolResult)

#endif // FRAMEMIND_TOOL_TYPES_H
