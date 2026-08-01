#include "service/agent/tools/get_scene_info_tool.h"

#include "service/agent/video_analysis_service.h"
#include "model/video_representation.h"

#include <QJsonArray>
#include <QJsonObject>

GetSceneInfoTool::GetSceneInfoTool(VideoAnalysisService* analysis)
    : m_analysis(analysis)
{
}

QString GetSceneInfoTool::description() const
{
    return QStringLiteral("获取指定场景或时间点所在场景的详细信息。");
}

QJsonObject GetSceneInfoTool::parameters() const
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("scene_id"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") },
                { QStringLiteral("description"),
                  QStringLiteral("场景 ID；未提供则按 timestamp_ms 定位") } } },
            { QStringLiteral("timestamp_ms"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") } } }
        }}
    };
}

void GetSceneInfoTool::executeAsync(const QString& callId,
                                      const QJsonObject& args,
                                      std::function<void(const ToolResult&)> done)
{
    if (!m_analysis) {
        done(ToolResult::fail(callId, name(), QStringLiteral("依赖未注入")));
        return;
    }
    auto repr = m_analysis->representation(m_videoPath);
    if (!repr) {
        done(ToolResult::fail(callId, name(), QStringLiteral("视频表示未就绪")));
        return;
    }

    int sceneId = args.value(QStringLiteral("scene_id")).toInt(-1);
    if (sceneId < 0) {
        const int64_t ts = args.value(QStringLiteral("timestamp_ms")).toVariant().toLongLong();
        sceneId = repr->sceneIdAt(ts);
    }
    if (sceneId < 0 || sceneId >= repr->scenes.size()) {
        done(ToolResult::fail(callId, name(), QStringLiteral("未定位到场景")));
        return;
    }

    const Scene& s = repr->scenes[sceneId];
    QJsonObject data;
    data.insert(QStringLiteral("scene_id"),   s.id);
    data.insert(QStringLiteral("start_ms"),   static_cast<qint64>(s.startMs));
    data.insert(QStringLiteral("end_ms"),     static_cast<qint64>(s.endMs));
    data.insert(QStringLiteral("keyframe_ms"),static_cast<qint64>(s.keyframeMs));
    data.insert(QStringLiteral("description"),
                repr->sceneDescriptions.value(sceneId));

    // 三类证据分别返回，便于 LM 按问题类型选择依据，
    // 并明确知道哪些结论只有音频支撑、哪些是已确认的视觉事实
    const QString visual = repr->sceneVisualDescriptions.value(sceneId);
    if (!visual.isEmpty())
        data.insert(QStringLiteral("visual_description"), visual);

    const auto fusionIt = repr->sceneFusions.constFind(sceneId);
    if (fusionIt != repr->sceneFusions.constEnd()) {
        const SceneFusion& f = fusionIt.value();
        if (!f.audioSummary.isEmpty())
            data.insert(QStringLiteral("audio_summary"), f.audioSummary);
        if (!f.fusedDescription.isEmpty())
            data.insert(QStringLiteral("fused_description"), f.fusedDescription);
        data.insert(QStringLiteral("audio_relation"),
                    SceneFusion::relationToString(f.relation));
        data.insert(QStringLiteral("relation_confidence"), f.confidence);
        data.insert(QStringLiteral("audio_type"),
                    SceneFusion::audioTypeToString(f.audioType));

        QJsonArray speech;
        for (const SpeechSegment& seg : f.speechSegments) {
            speech.append(QJsonObject{
                { QStringLiteral("start_ms"), static_cast<qint64>(seg.startMs) },
                { QStringLiteral("end_ms"),   static_cast<qint64>(seg.endMs) },
                { QStringLiteral("text"),     seg.text }
            });
        }
        if (!speech.isEmpty())
            data.insert(QStringLiteral("speech_segments"), speech);
    }

    done(ToolResult::ok(callId, name(), data));
}
