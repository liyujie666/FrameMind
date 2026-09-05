#include "service/rag/evidence_composer.h"

#include <QFileInfo>
#include <QJsonObject>
#include <QSet>

namespace {

QString evidenceTypeLabel(const VideoChunk& chunk)
{
    const QString explicitType = chunk.metadata.value(QStringLiteral("evidence_type")).toString();
    if (!explicitType.isEmpty()) return explicitType;

    switch (chunk.chunkType) {
    case VideoChunk::SceneSummary: return QStringLiteral("visual");
    case VideoChunk::SceneAudio: return QStringLiteral("audio");
    case VideoChunk::SceneFused: return QStringLiteral("fused");
    case VideoChunk::SpeechSegment: return QStringLiteral("speech_segment");
    case VideoChunk::FrameDesc: return QStringLiteral("frame");
    default: return QStringLiteral("other");
    }
}

} // namespace

QString EvidenceComposer::formatText(const QVector<RetrievalResult>& evidence,
                                     int maxItems,
                                     int maxCharsPerItem)
{
    if (evidence.isEmpty() || maxItems <= 0 || maxCharsPerItem <= 0) return {};

    QString output;
    const int limit = qMin(maxItems, evidence.size());
    for (int i = 0; i < limit; ++i) {
        const RetrievalResult& result = evidence.at(i);
        const VideoChunk& chunk = result.chunk;
        output += QStringLiteral("## 证据 %1 [%2]\n")
                      .arg(i + 1)
                      .arg(chunk.chunkId);
        output += QStringLiteral("时间范围：%1 - %2\n")
                      .arg(formatMs(chunk.startMs), formatMs(chunk.endMs));
        output += QStringLiteral("来源：%1；证据类型：%2\n")
                      .arg(hitPathLabel(result.hitPath), evidenceTypeLabel(chunk));
        const QStringList corroborating = chunk.metadata
            .value(QStringLiteral("corroborating_modalities")).toStringList();
        if (!corroborating.isEmpty()) {
            output += QStringLiteral("同期互证：%1\n").arg(corroborating.join(QStringLiteral("、")));
        }
        const QString temporalHint = chunk.metadata
            .value(QStringLiteral("query_temporal_hint")).toString();
        if (!temporalHint.isEmpty()) {
            output += QStringLiteral("时间约束：%1\n").arg(temporalHint);
        }
        output += QStringLiteral("相关内容：%1\n\n")
                      .arg(chunk.textContent.left(maxCharsPerItem));
    }
    return output;
}

QList<QImage> EvidenceComposer::mergeFrames(const QList<QImage>& userFrames,
                                             const QVector<RetrievalResult>& evidence,
                                             int maxEvidenceFrames,
                                             int maxFrameEdge)
{
    QList<QImage> frames = userFrames;
    if (maxEvidenceFrames <= 0 || maxFrameEdge <= 0) return frames;

    QSet<QString> seenPaths;
    int appended = 0;
    for (const RetrievalResult& result : evidence) {
        if (appended >= maxEvidenceFrames) break;

        const QString path = QFileInfo(result.chunk.keyframePath).canonicalFilePath();
        if (path.isEmpty() || seenPaths.contains(path)) continue;
        seenPaths.insert(path);

        QImage frame(path);
        if (frame.isNull()) continue;
        if (frame.width() > maxFrameEdge || frame.height() > maxFrameEdge) {
            frame = frame.scaled(maxFrameEdge, maxFrameEdge,
                                 Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
        frames.append(frame);
        ++appended;
    }
    return frames;
}

QJsonArray EvidenceComposer::toJson(const QVector<RetrievalResult>& evidence)
{
    QJsonArray serialized;
    for (const RetrievalResult& result : evidence) {
        const VideoChunk& chunk = result.chunk;
        QJsonObject object;
        object.insert(QStringLiteral("chunk_id"), chunk.chunkId);
        object.insert(QStringLiteral("video_id"), chunk.videoId);
        object.insert(QStringLiteral("start_ms"), static_cast<double>(chunk.startMs));
        object.insert(QStringLiteral("end_ms"), static_cast<double>(chunk.endMs));
        object.insert(QStringLiteral("text"), chunk.textContent);
        object.insert(QStringLiteral("keyframe_path"), chunk.keyframePath);
        object.insert(QStringLiteral("chunk_type"), static_cast<int>(chunk.chunkType));
        object.insert(QStringLiteral("metadata"), QJsonObject::fromVariantMap(chunk.metadata));
        object.insert(QStringLiteral("score"), static_cast<double>(result.score));
        object.insert(QStringLiteral("hit_path"), result.hitPath);
        serialized.append(object);
    }
    return serialized;
}

QVector<RetrievalResult> EvidenceComposer::fromJson(const QJsonArray& json)
{
    QVector<RetrievalResult> evidence;
    evidence.reserve(json.size());
    for (const QJsonValue& value : json) {
        const QJsonObject object = value.toObject();
        if (object.isEmpty()) continue;
        RetrievalResult result;
        VideoChunk& chunk = result.chunk;
        chunk.chunkId = object.value(QStringLiteral("chunk_id")).toString();
        chunk.videoId = object.value(QStringLiteral("video_id")).toString();
        chunk.startMs = static_cast<int64_t>(object.value(QStringLiteral("start_ms")).toDouble());
        chunk.endMs = static_cast<int64_t>(object.value(QStringLiteral("end_ms")).toDouble());
        chunk.textContent = object.value(QStringLiteral("text")).toString();
        chunk.keyframePath = object.value(QStringLiteral("keyframe_path")).toString();
        chunk.chunkType = static_cast<VideoChunk::ChunkType>(
            object.value(QStringLiteral("chunk_type")).toInt());
        chunk.metadata = object.value(QStringLiteral("metadata")).toObject().toVariantMap();
        result.score = static_cast<float>(object.value(QStringLiteral("score")).toDouble());
        result.hitPath = object.value(QStringLiteral("hit_path")).toString();
        if (chunk.isValid()) evidence.append(std::move(result));
    }
    return evidence;
}

QString EvidenceComposer::formatMs(int64_t ms)
{
    const int hours = static_cast<int>(ms / 3600000);
    const int minutes = static_cast<int>((ms % 3600000) / 60000);
    const int seconds = static_cast<int>((ms % 60000) / 1000);
    if (hours > 0) {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar('0'))
            .arg(seconds, 2, 10, QChar('0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10, QChar('0'));
}

QString EvidenceComposer::hitPathLabel(const QString& hitPath)
{
    if (hitPath == QLatin1String("visual")) return QStringLiteral("视觉检索");
    if (hitPath == QLatin1String("text") || hitPath == QLatin1String("text_dense")) {
        return QStringLiteral("文本语义检索");
    }
    if (hitPath == QLatin1String("text_lexical")) return QStringLiteral("文本精确检索");
    if (hitPath == QLatin1String("entity")) return QStringLiteral("实体检索");
    return hitPath.isEmpty() ? QStringLiteral("未知") : hitPath;
}
