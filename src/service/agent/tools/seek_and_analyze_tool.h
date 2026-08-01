#ifndef FRAMEMIND_SEEK_AND_ANALYZE_TOOL_H
#define FRAMEMIND_SEEK_AND_ANALYZE_TOOL_H

#include "service/agent/tool_base.h"
#include <QPointer>

class PlayerService;
class VideoAnalysisService;

/**
 * Tool: seek_and_analyze
 *
 * 跳转到视频指定时间点，截取画面并进行视觉分析。
 * 用于用户/Agent 想查看某个具体时间点画面时。
 *
 * 参数：
 *   - timestamp_ms (int, required) 目标时间点
 *   - focus (string) 关注点描述
 */
class SeekAndAnalyzeTool : public ITool {
public:
    SeekAndAnalyzeTool(PlayerService* player,
                        VideoAnalysisService* analysis);

    QString name() const override { return QStringLiteral("seek_and_analyze"); }
    QString description() const override;
    QJsonObject parameters() const override;

    void executeAsync(const QString& callId,
                       const QJsonObject& args,
                       std::function<void(const ToolResult&)> done) override;

private:
    QPointer<PlayerService>        m_player;
    QPointer<VideoAnalysisService> m_analysis;
};

#endif // FRAMEMIND_SEEK_AND_ANALYZE_TOOL_H
