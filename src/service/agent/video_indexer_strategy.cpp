#include "service/agent/frame_extractor.h"
#include "service/agent/media_probe.h"
#include "service/agent/video_indexer.h"
#include "service/clip_service.h"
#include "service/embedding_service.h"
#include "service/scene_detector.h"
#include "service/whisper_service.h"
#include "util/audio_decoder.h"
#include <QDir>
#include <QElapsedTimer>
#include "util/video_rag_log.h"
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
            {"pipeline", "semantic_context_v4"}};
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
        const QString stage = probe ? "probe" : "extract";
        auto log = [&](const QString& name, const QString& message, QJsonObject fields = {},
                       VideoRagLog::Level level = VideoRagLog::Level::Info) {
            if (context.log) context.log->event(stage, name, message, std::move(fields), level);
        };
        ExtractionResult result;
        auto &r = result.representation;
        r.videoId = context.videoId;
        QString error;
        QElapsedTimer metadataTimer; metadataTimer.start();
        r.metadata = MediaProbe::inspect(context.filePath, context.cancelled.get(), &error);
        if (!error.isEmpty()) {
            result.diagnostics << error;
            result.state = ArtifactState::Failed;
        }
        log("metadata", "媒体信息已读取", {{"video_duration_ms", qint64(r.metadata.durationMs)},
            {"has_audio", r.metadata.hasAudio}, {"duration_ms", metadataTimer.elapsed()}, {"error", error.left(500)}},
            error.isEmpty() ? VideoRagLog::Level::Info : VideoRagLog::Level::Warning);
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
                QElapsedTimer speechTimer; speechTimer.start();
                log("asr_started", "开始分窗解码与语音转写", {{"windows", ranges.size()}});
                int window = 0;
                for (const auto &range : ranges) {
                    if (context.isCancelled())
                        break;
                    QElapsedTimer windowTimer; windowTimer.start();
                    AudioDecoder decoder;
                    decoder.setProgressCallback([&](int) {
                        if (context.isCancelled())
                            decoder.cancel();
                    });
                    auto pcm = decoder.decodeToFloat32(context.filePath, range.first, range.second);
                    if(context.isCancelled()) break;
                    if (pcm.empty()) {
                        log("asr_window_failed", "音频窗口解码失败", {{"start_ms", qint64(range.first)},
                            {"end_ms", qint64(range.second)}}, VideoRagLog::Level::Warning);
                        ++window;
                        result.diagnostics << "audio_decode_failed:" + QString::number(range.first);
                        continue;
                    }
                    auto segments = m_whisper->transcribe(pcm);
                    ++window;
                    log("asr_window", "音频窗口转写结果", {{"window", window}, {"windows", ranges.size()},
                        {"start_ms", qint64(range.first)}, {"end_ms", qint64(range.second)},
                        {"segments", segments.size()}, {"duration_ms", windowTimer.elapsed()}}, VideoRagLog::Level::Debug);
                    if (context.log) context.log->progress(stage + ".asr", window, int(ranges.size()), "语音转写进度");
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
                log("asr_finished", "语音转写结束", {{"speech_segments", r.speechSegments.size()},
                    {"windows_processed", window}, {"windows", ranges.size()}, {"duration_ms", speechTimer.elapsed()},
                    {"cancelled", context.isCancelled()}});
            } else
                log("asr_skipped", "跳过语音转写", {{"reason", !r.metadata.hasAudio ? "no_audio" : "model_unavailable"}});
#else
            log("asr_skipped", "跳过语音转写", {{"reason", "whisper_disabled"}});
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
        QElapsedTimer framesTimer; framesTimer.start();
        log("frames_started", "开始采样画面（可用时同时编码视觉向量）", {{"target_frames", targets.size()}});
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
        log("frames_finished", "画面采样结束", {{"target_frames", targets.size()}, {"saved_frames", saved.size()},
            {"duration_ms", framesTimer.elapsed()}, {"cancelled", context.isCancelled()}});
        // Shot detection stays independent of semantic segmentation.
        SceneDetector detector;
        QElapsedTimer sceneTimer; sceneTimer.start();
        r.scenes = detector.detectScenes(thumbnails, timestamps);
        log("shots_finished", "镜头检测结束（镜头与语义单元分别统计）", {{"shots", r.scenes.size()},
            {"sampled_frames", thumbnails.size()}, {"mode", "histogram"}, {"duration_ms", sceneTimer.elapsed()}});
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
        log("evidence_finished", "原始证据提取结束", {{"status", artifactStateKey(result.state)},
            {"chunks", result.chunks.size()}, {"diagnostics", QJsonArray::fromStringList(result.diagnostics)}},
            result.diagnostics.isEmpty() ? VideoRagLog::Level::Info : VideoRagLog::Level::Warning);
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
        QElapsedTimer timer; timer.start();
        if (context.log) context.log->event("embedding", "started", "开始文本向量编码", {{"input_chunks", chunks.size()}});
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
        int ready = 0, failed = 0, skipped = 0;
        for (const auto& chunk : encoded) {
            const auto status = chunk.metadata.value("embedding_status").toString();
            ready += status == "ready"; failed += status == "failed"; skipped += status == "skipped";
        }
        if (context.log) context.log->event("embedding", "finished", "文本向量编码结束",
            {{"ready", ready}, {"failed", failed}, {"skipped", skipped}, {"output_chunks", encoded.size()},
             {"duration_ms", timer.elapsed()}, {"cancelled", context.isCancelled()}},
            failed ? VideoRagLog::Level::Warning : VideoRagLog::Level::Info);
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
