#include "service/agent/tools/control_player_tool.h"

#include "infrastructure/eventbus.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>

ControlPlayerTool::ControlPlayerTool(EventBus* eventBus)
    : m_eventBus(eventBus)
{
}

QString ControlPlayerTool::description() const
{
    return QStringLiteral(
        "控制播放器（seek/play/pause）。用于向用户展示特定片段，不会触发分析。");
}

QJsonObject ControlPlayerTool::parameters() const
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("action"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("string") },
                { QStringLiteral("enum"), QJsonArray{
                    QStringLiteral("seek"),
                    QStringLiteral("play"),
                    QStringLiteral("pause") } } } },
            { QStringLiteral("timestamp_ms"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("description"),
                  QStringLiteral("action=seek 时的目标时间点") } } }
        }},
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("action") } }
    };
}

void ControlPlayerTool::executeAsync(const QString& callId,
                                      const QJsonObject& args,
                                      std::function<void(const ToolResult&)> done)
{
    if (!m_eventBus) {
        done(ToolResult::fail(callId, name(), QStringLiteral("EventBus 未注入")));
        return;
    }
    const QString action = args.value(QStringLiteral("action")).toString();

    if (action == QLatin1String("seek")) {
        const int64_t ts = args.value(QStringLiteral("timestamp_ms")).toVariant().toLongLong();
        // 保证信号在主线程触发（EventBus 为 QObject，跨线程用 QueuedConnection）
        QMetaObject::invokeMethod(m_eventBus, [bus = m_eventBus.data(), ts] {
            bus->requestSeek(ts);
        }, Qt::QueuedConnection);
        QJsonObject data{{ QStringLiteral("action"), action },
                         { QStringLiteral("timestamp_ms"), static_cast<qint64>(ts) }};
        done(ToolResult::ok(callId, name(), data));
    } else if (action == QLatin1String("play") || action == QLatin1String("pause")) {
        // 当前 EventBus 未定义 play/pause 事件；后续可扩展
        // 暂时也用 seek 到当前位置 + 附带 action 提示
        QJsonObject data{{ QStringLiteral("action"), action },
                         { QStringLiteral("note"),
                           QStringLiteral("play/pause 事件在 EventBus 中未实现，已跳过") }};
        done(ToolResult::ok(callId, name(), data));
    } else {
        done(ToolResult::fail(callId, name(),
                              QStringLiteral("未知 action: %1").arg(action)));
    }
}
