#ifndef FRAMEMIND_GET_TRANSCRIPT_TOOL_H
#define FRAMEMIND_GET_TRANSCRIPT_TOOL_H

#include "service/agent/tool_base.h"
#include <QPointer>
#include <QString>

class VideoRAGStore;

/**
 * Tool: get_transcript
 *
 * 获取指定时间区间的语音转文字内容（从 RAG 的 speech_segment chunks 中提取）。
 */
class GetTranscriptTool : public ITool {
public:
    explicit GetTranscriptTool(VideoRAGStore* store);

    void setVideoId(const QString& videoId) { m_videoId = videoId; }

    QString name() const override { return QStringLiteral("get_transcript"); }
    QString description() const override;
    QJsonObject parameters() const override;

    void executeAsync(const QString& callId,
                       const QJsonObject& args,
                       std::function<void(const ToolResult&)> done) override;

private:
    QPointer<VideoRAGStore> m_store;
    QString                 m_videoId;
};

#endif // FRAMEMIND_GET_TRANSCRIPT_TOOL_H
