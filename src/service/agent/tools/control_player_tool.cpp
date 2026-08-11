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
        "控制播放器执行 seek/play/pause 操作。"
        "这是唯一能实际移动播放进度或控制播放状态的方式——"
        "如果不调用此工具，播放器不会有任何响应。"
        "用户要求跳转到某个时间点时必须调用此工具，"
        "timestamp_ms 为目标时间的毫秒数（如第10秒 = 10000）。");
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
        // 部分提供商把数字序列化为字符串，这里按值转换而不是按 JSON 类型拒绝。
        const QJsonValue tsValue = args.value(QStringLiteral("timestamp_ms"));
        bool tsOk = false;
        const int64_t ts = tsValue.toVariant().toLongLong(&tsOk);
        if (tsValue.isUndefined() || tsValue.isNull() || !tsOk) {
            done(ToolResult::fail(callId, name(),
                                  QStringLiteral("seek 需要有效的 timestamp_ms（毫秒）")));
            return;
        }
        if (ts < 0) {
            done(ToolResult::fail(callId, name(),
                                  QStringLiteral("timestamp_ms 不能为负数")));
            return;
        }
        // 保证信号在主线程触发（EventBus 为 QObject，跨线程用 QueuedConnection）
        QMetaObject::invokeMethod(m_eventBus, [bus = m_eventBus.data(), ts] {
            bus->requestSeek(ts);
        }, Qt::QueuedConnection);
        QJsonObject data{{ QStringLiteral("action"), action },
                         { QStringLiteral("timestamp_ms"), static_cast<qint64>(ts) }};
        done(ToolResult::ok(callId, name(), data));
    } else if (action == QLatin1String("play")) {
        QMetaObject::invokeMethod(m_eventBus, [bus = m_eventBus.data()] {
            bus->requestPlay();
        }, Qt::QueuedConnection);
        done(ToolResult::ok(callId, name(),
                            QJsonObject{{QStringLiteral("action"), action}}));
    } else if (action == QLatin1String("pause")) {
        QMetaObject::invokeMethod(m_eventBus, [bus = m_eventBus.data()] {
            bus->requestPause();
        }, Qt::QueuedConnection);
        done(ToolResult::ok(callId, name(),
                            QJsonObject{{QStringLiteral("action"), action}}));
    } else {
        done(ToolResult::fail(callId, name(),
                              QStringLiteral("未知 action: %1").arg(action)));
    }
}
