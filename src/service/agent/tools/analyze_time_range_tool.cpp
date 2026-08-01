#include "service/agent/tools/analyze_time_range_tool.h"

#include "service/agent/video_analysis_service.h"

#include <QJsonArray>
#include <QJsonObject>

AnalyzeTimeRangeTool::AnalyzeTimeRangeTool(VideoAnalysisService* analysis)
    : m_analysis(analysis)
{
}

QString AnalyzeTimeRangeTool::description() const
{
    return QStringLiteral(
        "分析一个时间区间内的视频内容，采样多帧进行连续画面理解。"
        "用于理解一段过程或动作。");
}

QJsonObject AnalyzeTimeRangeTool::parameters() const
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("start_ms"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") } } },
            { QStringLiteral("end_ms"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") } } },
            { QStringLiteral("sample_count"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("default"), 5 } } },
            { QStringLiteral("focus"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("string") } } }
        }},
        { QStringLiteral("required"), QJsonArray{
            QStringLiteral("start_ms"), QStringLiteral("end_ms") } }
    };
}

void AnalyzeTimeRangeTool::executeAsync(const QString& callId,
                                         const QJsonObject& args,
                                         std::function<void(const ToolResult&)> done)
{
    if (!m_analysis) {
        done(ToolResult::fail(callId, name(), QStringLiteral("依赖未注入")));
        return;
    }
    const int64_t startMs = args.value(QStringLiteral("start_ms")).toVariant().toLongLong();
    const int64_t endMs   = args.value(QStringLiteral("end_ms")).toVariant().toLongLong();
    const int sampleCount = args.value(QStringLiteral("sample_count")).toInt(5);
    const QString focus   = args.value(QStringLiteral("focus")).toString();

    if (endMs <= startMs) {
        done(ToolResult::fail(callId, name(),
                              QStringLiteral("时间区间无效: [%1, %2]").arg(startMs).arg(endMs)));
        return;
    }

    m_analysis->analyzeTimeRange(startMs, endMs, focus, sampleCount,
        [callId, startMs, endMs, done](const QString& desc) {
            QJsonObject data;
            data.insert(QStringLiteral("start_ms"), static_cast<qint64>(startMs));
            data.insert(QStringLiteral("end_ms"),   static_cast<qint64>(endMs));
            data.insert(QStringLiteral("description"), desc);
            done(ToolResult::ok(callId, QStringLiteral("analyze_time_range"), data));
        });
}
