#ifndef FRAMEMIND_CONTROL_PLAYER_TOOL_H
#define FRAMEMIND_CONTROL_PLAYER_TOOL_H

#include "service/agent/tool_base.h"
#include <QPointer>

class EventBus;

/**
 * Tool: control_player
 *
 * 控制播放器的 seek / play / pause。仅动作，不做分析。
 * 通过 EventBus::seekToPosition 与 PlayerViewModel 解耦（架构 §7.2）。
 */
class ControlPlayerTool : public ITool {
public:
    explicit ControlPlayerTool(EventBus* eventBus);

    QString name() const override { return QStringLiteral("control_player"); }
    QString description() const override;
    QJsonObject parameters() const override;

    void executeAsync(const QString& callId,
                       const QJsonObject& args,
                       std::function<void(const ToolResult&)> done) override;

private:
    QPointer<EventBus> m_eventBus;
};

#endif // FRAMEMIND_CONTROL_PLAYER_TOOL_H
