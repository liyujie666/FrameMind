#ifndef FRAMEMIND_ANALYZE_TIME_RANGE_TOOL_H
#define FRAMEMIND_ANALYZE_TIME_RANGE_TOOL_H

#include "service/agent/tool_base.h"
#include <QPointer>

class VideoAnalysisService;

/**
 * Tool: analyze_time_range
 *
 * 分析一个时间区间内的视频内容，采样多帧联合推理。
 * 用于理解一段过程 / 动作。
 */
class AnalyzeTimeRangeTool : public ITool {
public:
    explicit AnalyzeTimeRangeTool(VideoAnalysisService* analysis);

    QString name() const override { return QStringLiteral("analyze_time_range"); }
    QString description() const override;
    QJsonObject parameters() const override;

    void executeAsync(const QString& callId,
                       const QJsonObject& args,
                       std::function<void(const ToolResult&)> done) override;

private:
    QPointer<VideoAnalysisService> m_analysis;
};

#endif // FRAMEMIND_ANALYZE_TIME_RANGE_TOOL_H
