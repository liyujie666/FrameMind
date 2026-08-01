#include "service/agent/tools/seek_and_analyze_tool.h"

#include "service/playerservice.h"
#include "service/agent/video_analysis_service.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>

SeekAndAnalyzeTool::SeekAndAnalyzeTool(PlayerService* player,
                                        VideoAnalysisService* analysis)
    : m_player(player), m_analysis(analysis)
{
}

QString SeekAndAnalyzeTool::description() const
{
    return QStringLiteral(
        "跳转到视频指定时间点，截取画面并进行视觉分析。"
        "当需要查看特定时间的画面内容时使用。");
}

QJsonObject SeekAndAnalyzeTool::parameters() const
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("timestamp_ms"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("description"), QStringLiteral("目标时间点（毫秒）") } } },
            { QStringLiteral("focus"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("string") },
                { QStringLiteral("description"), QStringLiteral("分析关注点") } } }
        }},
        { QStringLiteral("required"), QJsonArray{ QStringLiteral("timestamp_ms") } }
    };
}

void SeekAndAnalyzeTool::executeAsync(const QString& callId,
                                       const QJsonObject& args,
                                       std::function<void(const ToolResult&)> done)
{
    if (!m_player || !m_analysis) {
        done(ToolResult::fail(callId, name(), QStringLiteral("依赖未注入")));
        return;
    }
    const int64_t ts = args.value(QStringLiteral("timestamp_ms")).toVariant().toLongLong();
    const QString focus = args.value(QStringLiteral("focus")).toString();

    auto future = m_player->captureFrameAt(ts, 3000);
    // 阻塞等待帧（工具执行处于工作线程，主线程不受影响）
    future.waitForFinished();
    QImage frame;
    if (future.resultCount() > 0) frame = future.result();
    if (frame.isNull()) {
        done(ToolResult::fail(callId, name(),
                              QStringLiteral("截取 %1ms 处的帧失败").arg(ts)));
        return;
    }

    // 调用 VLM 描述
    m_analysis->describeFrame(frame, ts, focus,
        [callId, ts, done](const QString& desc) {
            QJsonObject data;
            data.insert(QStringLiteral("timestamp_ms"), static_cast<qint64>(ts));
            data.insert(QStringLiteral("description"), desc);
            done(ToolResult::ok(callId, QStringLiteral("seek_and_analyze"), data));
        });
}
