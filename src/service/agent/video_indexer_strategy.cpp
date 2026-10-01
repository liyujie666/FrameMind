#include "service/agent/frame_extractor.h"
#include "service/agent/media_probe.h"
#include "service/agent/video_indexer.h"
#include "service/clip_service.h"
#include "service/embedding_service.h"
#include "service/scene_detector.h"
#include "service/whisper_service.h"
#include "util/audio_decoder.h"
#include <QDir>
#include <QMutexLocker>
#include <QStandardPaths>
#include <algorithm>

AvailableCapabilities VideoIndexer::capabilities() const {
    AvailableCapabilities c;
#ifdef FRAMEMIND_HAS_WHISPER
    c.asr = m_whisper && m_whisper->isReady();
#endif
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    c.textVector = m_embedder && m_embedder->isReady();
    c.visualVector = m_clip && m_clip->isReady();
#endif
    return c;
}
QJsonObject VideoIndexer::modelVersions() const {
    return {{"whisper", m_whisper ? m_whisper->modelFingerprint() : QString()},
            {"bge", m_embedder ? m_embedder->modelFingerprint() : QString()},
            {"clip", m_clip ? m_clip->modelFingerprint() : QString()},
            {"shots", "histogram_v1"},
            {"pipeline", "semantic_context_v3"}};
}

void VideoIndexer::setPublished(const VideoRepresentation &value) {
    auto r = QSharedPointer<VideoRepresentation>::create(value);
    {
        QMutexLocker lock(&m_reprMutex);
        m_repr[value.metadata.filePath] = r;
        m_currentPath = value.metadata.filePath;
        m_currentVideoId = value.videoId;
    }
    emit levelReady(int(r->level), r);
    emit indexCompleted(r);
}

void VideoIndexer::extract(const VideoBuildContext &context, const VideoRAGBuildPlan &plan, bool probe,
                           std::function<void(ExtractionResult)> done) {
    // The indexer's single worker serializes shared Whisper/CLIP/BGE inference.
    m_pool.setMaxThreadCount(1);
    m_cancellations << context.cancelled;
    m_pool.start([this, context, plan, probe, done = std::move(done)] {
        ExtractionResult result;
        auto &r = result.representation;
        r.videoId = context.videoId;
        QString error;
        r.metadata = MediaProbe::inspect(context.filePath, context.cancelled.get(), &error);
        if (!error.isEmpty()) {
            result.diagnostics << error;
            result.state = ArtifactState::Failed;
        }
        const int64_t duration = r.metadata.durationMs;
        QVector<int64_t> targets;
        if (probe) {
            for (int i = 0; i < 5 && duration > 0; ++i)
                targets << qMin(duration - 1, duration * (2 * i + 1) / 10);
            std::sort(targets.begin(), targets.end());
            targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        } else
            for (int64_t t = 0; t < duration; t += qMax<int64_t>(1000, plan.frameIntervalMs))
                targets << t;
        const auto extractSpeech = [&] {
#ifdef FRAMEMIND_HAS_WHISPER
            if (r.metadata.hasAudio && m_whisper && m_whisper->isReady()) {
                QVector<QPair<int64_t, int64_t>> ranges;
                if (probe)
                    for (auto t : targets) {
                        const auto start = qMax<int64_t>(0, t - 7500), end = qMin(duration, start + 15000);
                        if (!ranges.isEmpty() && start <= ranges.last().second)
                            ranges.last().second = qMax(ranges.last().second, end);
                        else
                            ranges << qMakePair(start, end);
                    }
                else
                    for (int64_t t = 0; t < duration; t += 60000)
                        ranges << qMakePair(t, qMin(duration, t + 60000));
                for (const auto &range : ranges) {
                    if (context.isCancelled())
                        break;
                    AudioDecoder decoder;
                    decoder.setProgressCallback([&](int) {
                        if (context.isCancelled())
                            decoder.cancel();
                    });
                auto pcm = decoder.decodeToFloat32(context.filePath, range.first, range.second);
                if(context.isCancelled()) break;
                    if (pcm.empty()) {
                        result.diagnostics << "audio_decode_failed:" + QString::number(range.first);
                        continue;
                    }
                    auto segments = m_whisper->transcribe(pcm);
                    for (auto s : segments) {
                        s.startMs = qBound(range.first, s.startMs + range.first, range.second);
                        s.endMs = qBound(range.first, s.endMs + range.first, range.second);
                        if (s.endMs <= s.startMs || s.text.trimmed().isEmpty())
                            continue;
                        r.speechSegments << s;
                        VideoChunk c;
                        c.videoId = context.videoId;
                        c.startMs = s.startMs;
                        c.endMs = s.endMs;
                        c.chunkType = VideoChunk::SpeechSegment;
                        c.textContent = s.text;
                        c.chunkId =
                            context.rawSnapshotId + ":speech:" + QString::number(result.chunks.size());
                        c.metadata = {{"raw_snapshot_id", context.rawSnapshotId},
                                      {"source", "whisper"},
                                      {"speaker", "unknown"}};
                        result.chunks << c;
                    }
                }
            }
#endif
        };
        if (plan.audioFirst)
            extractSpeech();
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                             "/keyframes/" + context.videoId + "/" + context.rawSnapshotId +
                             (probe ? "/probe" : "");
        QDir().mkpath(root);
        QVector<QImage> thumbnails;
        QVector<int64_t> timestamps;
        QVector<SceneFrame> saved;
        // Bound image memory while retaining every sampled PTS on disk.
        const int batchSize = probe ? 1 : 128;
        for (int begin = 0; begin < targets.size() && !context.isCancelled(); begin += batchSize) {
            auto frames = FrameExtractor::extract(context.filePath, targets.mid(begin, batchSize),
                                                  FrameExtractor::Options{}, context.cancelled.get(), &error);
            if (!error.isEmpty())
                result.diagnostics << error;
            for (const auto &f : frames) {
                SceneFrame savedFrame;
                savedFrame.requestedMs = f.requestedMs;
                savedFrame.ptsMs = f.ptsMs;
                savedFrame.imagePath = root + "/" + QString::number(f.ptsMs) + ".jpg";
                if (!f.image.save(savedFrame.imagePath, "JPG", 85)) {
                    result.diagnostics << "frame_write_failed";
                    continue;
                }
                saved << savedFrame;
                thumbnails << f.image.scaled(64, 64);
                timestamps << f.ptsMs;
                VideoChunk c;
                c.videoId = context.videoId;
                c.startMs = qMax<int64_t>(0, f.ptsMs);
                c.endMs = qMin(duration, c.startMs + 1);
                c.chunkType = VideoChunk::FrameDesc;
                c.chunkId = context.rawSnapshotId + ":frame:" + QString::number(f.ptsMs);
                c.keyframePath = savedFrame.imagePath;
                c.metadata = {{"raw_snapshot_id", context.rawSnapshotId},
                              {"pts_ms", qlonglong(f.ptsMs)},
                              {"requested_ms", qlonglong(f.requestedMs)},
                              {"source", "sampled_frame"}};
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
                if (!probe && m_clip && m_clip->isReady() && !context.isCancelled()) {
                    c.frameEmbedding = m_clip->encodeImage(f.image);
                    c.metadata.insert("embedding_model_id", "clip_visual");
                    c.metadata.insert("embedding_version", "1");
                    if (c.frameEmbedding.empty())
                        result.diagnostics << "visual_embedding_failed:" + c.chunkId;
                }
#endif
                if (c.isValid())
                    result.chunks << c;
            }
        }
        // Shot detection stays independent of semantic segmentation.
        SceneDetector detector;
        r.scenes = detector.detectScenes(thumbnails, timestamps);
        if (r.scenes.isEmpty() && duration > 0) {
            Scene s;
            s.id = 0;
            s.endMs = duration;
            r.scenes << s;
        }
        if (!r.scenes.isEmpty()) {
            r.scenes.first().startMs = 0;
            r.scenes.last().endMs = duration;
        }
        for (auto &s : r.scenes)
            for (const auto &f : saved)
                if (s.contains(f.ptsMs)) {
                    s.representativeFrames << f;
                    if (s.keyframePath.isEmpty()) {
                        s.keyframeMs = f.ptsMs;
                        s.keyframePath = f.imagePath;
                    }
                }
        if (!plan.audioFirst)
            extractSpeech();
        if (r.metadata.hasAudio && !plan.asrAvailable && !probe)
            result.diagnostics << "asr_unavailable";
        if (saved.isEmpty())
            result.diagnostics << "visual_evidence_unavailable";
        if (context.isCancelled())
            result.state = ArtifactState::Cancelled;
        else if (result.state != ArtifactState::Failed && !result.diagnostics.isEmpty())
            result.state = ArtifactState::Partial;
        r.level = VideoRepresentation::Level1;
        QMetaObject::invokeMethod(
            this, [done, result = std::move(result)]() mutable { done(std::move(result)); },
            Qt::QueuedConnection);
    });
}

void VideoIndexer::encodeChunks(const VideoBuildContext &context, QVector<VideoChunk> chunks,
                                std::function<void(QVector<VideoChunk>)> done) {
    m_cancellations << context.cancelled;
    m_pool.setMaxThreadCount(1);
    m_pool.start([this, context, chunks = std::move(chunks), done = std::move(done)]() mutable {
        QVector<VideoChunk> encoded;
        for (const auto &c : chunks) {
            if (context.isCancelled())
                break;
            if (c.chunkType == VideoChunk::FrameDesc) {
                encoded << c;
                continue;
            }
            QStringList texts = m_embedder ? m_embedder->splitPassage(c.textContent) : QStringList{};
            if (!m_embedder)
                for (int i = 0; i < c.textContent.size(); i += 498)
                    texts << c.textContent.mid(i, 498);
            if (texts.isEmpty())
                texts << c.textContent;
            for (int page = 0; page < texts.size(); ++page) {
                auto part = c;
                part.textContent = texts[page];
                if (texts.size() > 1)
                    part.chunkId += ":page:" + QString::number(page);
                part.metadata.insert("evidence_page", page);
                part.metadata.insert("evidence_pages", texts.size());
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
                if (m_embedder && m_embedder->isReady()) {
                    part.textEmbedding = m_embedder->embedPassage(part.textContent);
                    part.metadata.insert("embedding_model_id", "bge_text");
                    part.metadata.insert("embedding_version", "passage_v2");
                }
#endif
                part.metadata.insert("embedding_status",
                                     part.textEmbedding.empty()
                                         ? (capabilities().textVector ? "failed" : "skipped")
                                         : "ready");
                encoded << part;
            }
        }
        QMetaObject::invokeMethod(
            this, [done, encoded = std::move(encoded)]() mutable { done(std::move(encoded)); },
            Qt::QueuedConnection);
    });
}

void VideoIndexer::extractUnitFrames(const VideoBuildContext &context, const QVector<SemanticUnit> &units,
                                     const VideoRAGBuildPlan &plan,
                                     std::function<void(ExtractionResult)> done) {
    m_cancellations << context.cancelled;
    m_pool.setMaxThreadCount(1);
    m_pool.start([this, context, units, plan, done = std::move(done)] {
        ExtractionResult result;
        QVector<int64_t> targets;
        for (const auto &u : units)
            for (int i = 0; i < plan.framesPerUnit; ++i)
                targets << u.startMs + (u.endMs - u.startMs - 1) * i / qMax(1, plan.framesPerUnit - 1);
        std::sort(targets.begin(), targets.end());
        targets.erase(std::unique(targets.begin(), targets.end()), targets.end());
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation) +
                             "/keyframes/" + context.videoId + "/" + context.rawSnapshotId + "/units";
        QDir().mkpath(root);
        for (int offset = 0; offset < targets.size() && !context.isCancelled(); offset += 128) {
            QString error;
            auto frames = FrameExtractor::extract(context.filePath, targets.mid(offset, 128),
                                                  FrameExtractor::Options{}, context.cancelled.get(), &error);
            if (!error.isEmpty())
                result.diagnostics << error;
            for (const auto &f : frames) {
                VideoChunk c;
                c.videoId = context.videoId;
                c.chunkId = context.rawSnapshotId + ":unit_frame:" + QString::number(f.ptsMs);
                c.startMs = qMax<int64_t>(0, f.ptsMs);
                c.endMs = c.startMs + 1;
                c.chunkType = VideoChunk::FrameDesc;
                c.keyframePath = root + "/" + QString::number(f.ptsMs) + ".jpg";
                if (!f.image.save(c.keyframePath, "JPG", 85)) {
                    result.diagnostics << "unit_frame_write_failed";
                    continue;
                }
                c.metadata = {{"raw_snapshot_id", context.rawSnapshotId},
                              {"pts_ms", qlonglong(f.ptsMs)},
                              {"requested_ms", qlonglong(f.requestedMs)},
                              {"source", "unit_boundary_sample"}};
#ifdef FRAMEMIND_HAS_ONNXRUNTIME
                if (m_clip && m_clip->isReady() && !context.isCancelled()) {
                    c.frameEmbedding = m_clip->encodeImage(f.image);
                    c.metadata.insert("embedding_model_id", "clip_visual");
                    c.metadata.insert("embedding_version", "1");
                    if (c.frameEmbedding.empty())
                        result.diagnostics << "visual_embedding_failed:" + c.chunkId;
                }
#endif
                result.chunks << c;
            }
        }
        if (context.isCancelled())
            result.state = ArtifactState::Cancelled;
        else if (!result.diagnostics.isEmpty())
            result.state = ArtifactState::Partial;
        QMetaObject::invokeMethod(
            this, [done, result = std::move(result)]() mutable { done(std::move(result)); },
            Qt::QueuedConnection);
    });
}
