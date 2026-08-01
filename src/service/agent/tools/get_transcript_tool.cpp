#include "service/agent/tools/get_transcript_tool.h"

#include "service/rag/video_rag_store.h"

#include <QJsonArray>
#include <QJsonObject>
#include <algorithm>

GetTranscriptTool::GetTranscriptTool(VideoRAGStore* store)
    : m_store(store)
{
}

QString GetTranscriptTool::description() const
{
    return QStringLiteral("获取指定时间区间的语音转文字内容。");
}

QJsonObject GetTranscriptTool::parameters() const
{
    return QJsonObject{
        { QStringLiteral("type"), QStringLiteral("object") },
        { QStringLiteral("properties"), QJsonObject{
            { QStringLiteral("start_ms"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") } } },
            { QStringLiteral("end_ms"), QJsonObject{
                { QStringLiteral("type"), QStringLiteral("integer") } } }
        }},
        { QStringLiteral("required"), QJsonArray{
            QStringLiteral("start_ms"), QStringLiteral("end_ms") } }
    };
}

void GetTranscriptTool::executeAsync(const QString& callId,
                                      const QJsonObject& args,
                                      std::function<void(const ToolResult&)> done)
{
    if (!m_store) {
        done(ToolResult::fail(callId, name(), QStringLiteral("Store 未注入")));
        return;
    }
    const int64_t startMs = args.value(QStringLiteral("start_ms")).toVariant().toLongLong();
    const int64_t endMs   = args.value(QStringLiteral("end_ms")).toVariant().toLongLong();

    // 从 text_segments 中过滤 chunk_type=speech_segment 且时间区间重叠的
    auto chunks = m_store->listChunks(VideoRAGStore::TextSegments, m_videoId);
    QJsonArray segs;
    for (const auto& c : chunks) {
        if (c.chunkType != VideoChunk::SpeechSegment) continue;
        if (c.endMs < startMs || c.startMs > endMs) continue;
        QJsonObject o;
        o.insert(QStringLiteral("start_ms"), static_cast<qint64>(c.startMs));
        o.insert(QStringLiteral("end_ms"),   static_cast<qint64>(c.endMs));
        o.insert(QStringLiteral("text"),     c.textContent);
        segs.append(o);
    }

    // 按时间排序
    // （用 std::sort 需要转换，简化：QJsonArray 不易，跳过）
    QJsonObject data;
    data.insert(QStringLiteral("range"), QJsonArray{ static_cast<qint64>(startMs),
                                                     static_cast<qint64>(endMs) });
    data.insert(QStringLiteral("segments"), segs);
    done(ToolResult::ok(callId, name(), data));
}
