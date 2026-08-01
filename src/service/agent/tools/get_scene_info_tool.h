#ifndef FRAMEMIND_GET_SCENE_INFO_TOOL_H
#define FRAMEMIND_GET_SCENE_INFO_TOOL_H

#include "service/agent/tool_base.h"
#include <QSharedPointer>
#include <QPointer>

class VideoAnalysisService;
struct VideoRepresentation;

/**
 * Tool: get_scene_info
 *
 * 获取指定场景或时间点所在场景的详细信息。
 */
class GetSceneInfoTool : public ITool {
public:
    explicit GetSceneInfoTool(VideoAnalysisService* analysis);

    /// 每次对话前由 Orchestrator 注入当前 videoPath
    void setVideoPath(const QString& videoPath) { m_videoPath = videoPath; }

    QString name() const override { return QStringLiteral("get_scene_info"); }
    QString description() const override;
    QJsonObject parameters() const override;

    void executeAsync(const QString& callId,
                       const QJsonObject& args,
                       std::function<void(const ToolResult&)> done) override;

private:
    QPointer<VideoAnalysisService> m_analysis;
    QString                        m_videoPath;
};

#endif // FRAMEMIND_GET_SCENE_INFO_TOOL_H
