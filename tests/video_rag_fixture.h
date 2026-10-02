#pragma once
#include "service/agent/video_rag_build_coordinator.h"
#include <QJsonDocument>
#include <QTimer>

class FixtureBackend final : public VideoRAGBuildBackend {
  public:
    AvailableCapabilities caps{true, false, false};
    VideoRepresentation published;
    QString framePath;
    int extractions = 0, probes = 0;
    int longSpeechChars = 0;
    bool hasAudio = true;
    bool failExtraction = false;
    QVector<std::function<void()>> delayed;
    bool delayExtraction = false;
    AvailableCapabilities capabilities() const override { return caps; }
    QJsonObject modelVersions() const override {
        return {{"whisper", "fixture_whisper"}, {"bge", "fixture_bge"}, {"clip", "fixture_clip"}};
    }
    void extract(const VideoBuildContext &context, const VideoRAGBuildPlan &, bool probe,
                 std::function<void(VideoEvidenceExtractionResult)> done) override {
        if (probe)
            ++probes;
        else
            ++extractions;
        VideoEvidenceExtractionResult result;
        auto &r = result.representation;
        r.videoId = context.videoId;
        r.metadata.filePath = context.filePath;
        r.metadata.fileName = "fixture";
        r.metadata.durationMs = 120000;
        r.metadata.hasAudio = hasAudio;
        Scene shot;
        shot.id = 0;
        shot.endMs = 120000;
        shot.keyframePath = framePath;
        shot.keyframeMs = 0;
        SceneFrame f;
        f.ptsMs = 0;
        f.imagePath = framePath;
        shot.representativeFrames << f;
        r.scenes << shot;
        for (int i = 0; i < 4 && caps.asr && hasAudio; ++i) {
            VideoChunk c;
            c.videoId = context.videoId;
            c.chunkId = context.rawSnapshotId + QString(":speech%1").arg(i);
            c.startMs = i * 30000;
            c.endMs = (i + 1) * 30000;
            c.chunkType = VideoChunk::SpeechSegment;
            c.textContent = i == 2 ? QStringLiteral("接下来讨论预算，后半段批准42万元")
                                   : QStringLiteral("项目会议讨论安排");
            c.metadata = {{"raw_snapshot_id", context.rawSnapshotId}, {"source", "whisper"}};
            if (longSpeechChars > 0)
                c.textContent = QString(longSpeechChars, QChar(0x4e2d)) + c.textContent;
            result.chunks << c;
            SpeechSegment segment;
            segment.startMs = c.startMs;
            segment.endMs = c.endMs;
            segment.text = c.textContent;
            r.speechSegments << segment;
        }
        VideoChunk image;
        image.videoId = context.videoId;
        image.chunkId = context.rawSnapshotId + ":frame";
        image.startMs = 0;
        image.endMs = 1;
        image.chunkType = VideoChunk::FrameDesc;
        image.keyframePath = framePath;
        image.metadata = {{"raw_snapshot_id", context.rawSnapshotId}};
        result.chunks << image;
        if (!hasAudio)
            for (int64_t pts = 30000; pts < 120000; pts += 30000) {
                auto extra = image;
                extra.chunkId = context.rawSnapshotId + QString(":frame:%1").arg(pts);
                extra.startMs = pts;
                extra.endMs = pts + 1;
                result.chunks << extra;
            }
        if (failExtraction) {
            result.state = ArtifactState::Failed;
            result.diagnostics << "fixture_failure";
        }
        auto call = [done, result] { done(result); };
        if (delayExtraction)
            delayed << call;
        else
            QTimer::singleShot(0, call);
    }
    void encodeChunks(const VideoBuildContext &, QVector<VideoChunk> chunks,
                      std::function<void(QVector<VideoChunk>)> done) override {
        QTimer::singleShot(0, [done, chunks] { done(chunks); });
    }
    void setPublished(const VideoRepresentation &value) override { published = value; }
    void extractUnitFrames(const VideoBuildContext &context, const QVector<SemanticUnit> &units,
                           const VideoRAGBuildPlan &,
                           std::function<void(VideoEvidenceExtractionResult)> done) override {
        VideoEvidenceExtractionResult result;
        for (const auto &unit : units)
            for (auto pts : QVector<int64_t>{unit.startMs, (unit.startMs + unit.endMs) / 2, unit.endMs - 1}) {
                VideoChunk c;
                c.videoId = context.videoId;
                c.chunkId = context.rawSnapshotId + QString(":supplement:%1").arg(pts);
                c.startMs = pts;
                c.endMs = pts + 1;
                c.chunkType = VideoChunk::FrameDesc;
                c.keyframePath = framePath;
                c.metadata = {{"raw_snapshot_id", context.rawSnapshotId}, {"requested_ms", qlonglong(pts)}};
                result.chunks << c;
            }
        QTimer::singleShot(0, [done, result] { done(result); });
    }
    static QString modelReply(const QString &system, const QString &text) {
        const auto input = SemanticUnitBuilder::parseObject(text);
        if (input.contains("local_candidates"))
            return QString::fromUtf8(QJsonDocument(QJsonObject{{"units", input["local_candidates"]}})
                                         .toJson(QJsonDocument::Compact));
        if (input.contains("core_evidence")) {
            QString summary;
            QJsonArray refs;
            for (auto v : input["core_evidence"].toArray()) {
                const auto ev = v.toObject();
                summary += ev["text"].toString();
                refs.append(ev["source_id"]);
            }
            if (summary.isEmpty())
                summary = QStringLiteral("画面为固定蓝色演示页");
            const QString kind = system.contains("decision")    ? "decision"
                                 : system.contains("concept")   ? "concept"
                                 : system.contains("operation") ? "result"
                                 : system.contains("answer")    ? "answer"
                                                                : "topic";
            return QString::fromUtf8(
                QJsonDocument(QJsonObject{{"title", QStringLiteral("会议预算")},
                                          {"visual_description", QStringLiteral("蓝色演示页")},
                                          {"audio_summary", summary},
                                          {"summary", summary},
                                          {"carry_context", QJsonObject{
                                              {"topic", "fixture topic"},
                                              {"current_state", QJsonObject{{"text", ""}, {"fact_refs", QJsonArray{}}}},
                                              {"key_fact_refs", QJsonArray{}},
                                              {"pending_threads", QJsonArray{}}, {"uncertainties", QJsonArray{}}}},
                                          {"facts", QJsonArray{QJsonObject{{"kind", kind},
                                                                           {"text", summary},
                                                                           {"source_chunk_ids", refs}}}}})
                    .toJson(QJsonDocument::Compact));
        }
        if (system.contains("分类"))
            return QString::fromUtf8(QJsonDocument(QJsonObject{{"type", "meeting"},
                                                               {"probe_evidence_ids", QJsonArray{}},
                                                               {"reasoning", "fixture"}})
                                         .toJson(QJsonDocument::Compact));
        return QString::fromUtf8(
            QJsonDocument(QJsonObject{{"summary", text.left(4000)}}).toJson(QJsonDocument::Compact));
    }
    static void installModel(VideoRAGBuildCoordinator &coordinator) {
        coordinator.setModelRequest([](const VideoBuildContext &, const QString &system, const QString &text,
                                       const QList<QImage> &, std::function<void(QString)> done) {
            QTimer::singleShot(0, [=] { done(modelReply(system, text)); });
        });
    }
};
