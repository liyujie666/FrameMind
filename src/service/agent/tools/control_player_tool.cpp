#include "service/agent/tools/control_player_tool.h"

#include "infrastructure/eventbus.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QMetaObject>
#include <QTimer>
#include <QUuid>
#include <QSharedPointer>

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
    if (action != QLatin1String("seek") && action != QLatin1String("play")
        && action != QLatin1String("pause")) {
        done(ToolResult::fail(callId, name(),
                              QStringLiteral("未知 action: %1").arg(action)));
        return;
    }
    if (action == QLatin1String("seek") && !args.contains(QStringLiteral("timestamp_ms"))) {
        done(ToolResult::fail(callId, name(),
                              QStringLiteral("seek 缺少 timestamp_ms")));
        return;
    }

    const int64_t ts = args.value(QStringLiteral("timestamp_ms")).toVariant().toLongLong();
    if (action == QLatin1String("seek") && ts < 0) {
        done(ToolResult::fail(callId, name(), QStringLiteral("timestamp_ms 不能为负数")));
        return;
    }

    const QString requestId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    auto completed = QSharedPointer<bool>::create(false);
    auto connection = QSharedPointer<QMetaObject::Connection>::create();
    *connection = QObject::connect(
        m_eventBus, &EventBus::playerActionCompleted, m_eventBus,
        [callId, action, ts, requestId, completed, connection, done](
            const QString& completedAction, const QString& id, bool success,
            int64_t actual, const QString& error) {
            if (id != requestId || completedAction != action || *completed) return;
            *completed = true;
            QObject::disconnect(*connection);
            if (!success) {
                done(ToolResult::fail(callId, QStringLiteral("control_player"), error));
                return;
            }
            QJsonObject data{{QStringLiteral("action"), action},
                             {QStringLiteral("actual_timestamp_ms"), static_cast<qint64>(actual)}};
            if (action == QLatin1String("seek")) {
                data.insert(QStringLiteral("requested_timestamp_ms"), static_cast<qint64>(ts));
            }
            done(ToolResult::ok(callId, QStringLiteral("control_player"), data));
        });
    QTimer::singleShot(5000, m_eventBus, [callId, action, completed, connection, done] {
        if (*completed) return;
        *completed = true;
        QObject::disconnect(*connection);
        done(ToolResult::fail(callId, QStringLiteral("control_player"),
                              QStringLiteral("播放器操作超时，未收到确认")));
    });
    QMetaObject::invokeMethod(m_eventBus, [bus = m_eventBus.data(), action, requestId, ts] {
        if (action == QLatin1String("seek")) bus->requestSeekWithResult(ts, requestId);
        else bus->requestPlayerActionWithResult(action, requestId);
    }, Qt::QueuedConnection);
}
