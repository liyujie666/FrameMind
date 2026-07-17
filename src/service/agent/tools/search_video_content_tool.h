#ifndef FRAMEMIND_SEARCH_VIDEO_CONTENT_TOOL_H
#define FRAMEMIND_SEARCH_VIDEO_CONTENT_TOOL_H

#include "service/agent/tool_base.h"
#include <QPointer>
#include <QString>

class VideoRAGRetriever;

/**
 * Tool: search_video_content
 *
 * 在视频中搜索符合描述的画面，返回匹配的时间点列表。
 */
class SearchVideoContentTool : public ITool {
public:
    explicit SearchVideoContentTool(VideoRAGRetriever* retriever);

    /// 每次调用前设置当前 videoId（由 Orchestrator 从 Agent 上下文注入）
    void setVideoId(const QString& videoId) { m_videoId = videoId; }

    QString name() const override { return QStringLiteral("search_video_content"); }
    QString description() const override;
    QJsonObject parameters() const override;

    void executeAsync(const QString& callId,
                       const QJsonObject& args,
                       std::function<void(const ToolResult&)> done) override;

private:
    QPointer<VideoRAGRetriever> m_retriever;
    QString                     m_videoId;
};

#endif // FRAMEMIND_SEARCH_VIDEO_CONTENT_TOOL_H
