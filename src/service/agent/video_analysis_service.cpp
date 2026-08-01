#include "service/agent/video_analysis_service.h"

#include "service/agentservice.h"
#include "service/playerservice.h"
#include "service/agent/video_indexer.h"
#include "service/rag/video_rag_store.h"

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
#  include "service/embedding_service.h"
#endif

#include <QUuid>
#include <QPointer>
#include <QDebug>

namespace {
// Prompt 模板：agent-core-design.md §3.2
const char* kSceneDescPrompt = R"PROMPT(
你是一个专业的视频内容分析师。请根据提供的视频帧，用2~4句话描述该场景。

要求：
1. 描述画面中的主要人物、物体和环境
2. 说明正在发生的动作或事件
3. 如有文字/字幕/标志请转录
4. 客观描述，不做主观推断
5. 直接输出描述文字，不要 JSON 格式，不要标题
)PROMPT";

const char* kVideoSummaryPrompt = R"PROMPT(
你是视频内容分析师。我将提供一部视频的所有场景描述（按时间顺序排列），请综合生成一段连贯的全视频摘要。

## 要求
- 长度：300~500 字
- 使用 Markdown 格式，包含适当的段落和小标题
- **叙述连贯**：以完整故事线串联各场景，体现情节发展、人物变化或事件演进，不要逐条罗列
- 说明视频类型/主题（剧情片/纪录片/教学/访谈等）
- 提炼视频的核心主题或关键信息
- 不要虚构场景描述中未出现的信息
- 使用中文
)PROMPT";

const char* kFrameDescPrompt = R"PROMPT(
描述这一帧画面。要求：
- 具体到画面中的人物/物体/场景/文字
- 如有动作请说明
- 客观描述，不做主观推断
)PROMPT";
} // namespace

VideoAnalysisService::VideoAnalysisService(AgentService*    agent,
                                           VideoIndexer*    indexer,
                                           VideoRAGStore*   ragStore,
                                           PlayerService*   player,
                                           QObject*         parent)
    : QObject(parent)
    , m_agent(agent)
    , m_indexer(indexer)
    , m_ragStore(ragStore)
    , m_player(player)
{
    if (m_indexer) {
        connect(m_indexer, &VideoIndexer::levelReady,
                this, [this](int level, QSharedPointer<VideoRepresentation> repr) {
            emit analysisProgress(30 + level * 20,
                                  tr("Level %1 索引完成").arg(level));
            if (level == 1) {
                // 缓存命中：repr 已有场景描述和摘要，直接重放信号，不重复调 VLM
                if (!repr->videoSummary.isEmpty()) {
                    for (auto it = repr->sceneDescriptions.constBegin();
                         it != repr->sceneDescriptions.constEnd(); ++it) {
                        emit sceneDescribed(it.key(), it.value());
                    }
                    emit summaryReady(repr->videoSummary);
                    return;
                }

                if (!repr->scenes.isEmpty()) {
                    startDescribeAllScenes(repr);
                }
            }
        });
    }
}

// ============================================================
// 统筹入口
// ============================================================

void VideoAnalysisService::onVideoOpened(const QString& videoPath)
{
    if (m_ragStore) {
        // 先尝试加载已有索引
        const QString vid = VideoIndexer::computeVideoId(videoPath);
        m_ragStore->loadVideo(vid);
    }
    if (m_indexer) {
        m_indexer->startIndex(videoPath);
    }
}

void VideoAnalysisService::analyzeVideo(const QString& videoPath)
{
    onVideoOpened(videoPath);
    // 后续 Level 2 由 levelReady 触发
}

QSharedPointer<VideoRepresentation> VideoAnalysisService::representation(
    const QString& videoPath) const
{
    return m_indexer ? m_indexer->representation(videoPath) : nullptr;
}

// ============================================================
// 串行描述所有场景，全部完成后触发全视频摘要
// （AgentService 是单流设计，不支持并发请求，必须串行）
// ============================================================

void VideoAnalysisService::startDescribeAllScenes(
    QSharedPointer<VideoRepresentation> repr)
{
    const int total = repr->scenes.size();
    if (total <= 0) return;

    qDebug() << "[VideoAnalysisService] 开始描述全部场景，共" << total << "个";
    emit analysisProgress(50, tr("分析场景内容（0/%1）").arg(total));

    QPointer<VideoAnalysisService> guard(this);

    // 递归串行：完成一个再触发下一个，保证 AgentService 单流不冲突
    auto describeNext = QSharedPointer<std::function<void(int)>>::create();

    *describeNext = [guard, repr, total, describeNext](int idx) mutable {
        if (!guard) return;

        // 所有场景处理完毕，触发摘要
        if (idx >= total) {
            qDebug() << "[VideoAnalysisService] 全部场景描述完成，触发 summarizeVideo:"
                     << repr->metadata.fileName;
            guard->summarizeVideo(repr);
            return;
        }

        const int sceneId = repr->scenes[idx].id;

        // 已描述过则跳过，直接处理下一个
        if (repr->sceneDescriptions.contains(sceneId)
            && !repr->sceneDescriptions[sceneId].isEmpty()) {
            emit guard->analysisProgress(
                50 + (idx + 1) * 40 / total,
                tr("分析场景内容（%1/%2）").arg(idx + 1).arg(total));
            (*describeNext)(idx + 1);
            return;
        }

        guard->doDescribeSceneWithCallback(
            sceneId, repr,
            [guard, total, idx, describeNext](int /*sceneId*/) mutable {
                if (!guard) return;
                emit guard->analysisProgress(
                    50 + (idx + 1) * 40 / total,
                    tr("分析场景内容（%1/%2）").arg(idx + 1).arg(total));
                (*describeNext)(idx + 1);
            });
    };

    (*describeNext)(0);
}

// ============================================================
// Level 2: 场景描述
// ============================================================

void VideoAnalysisService::describeScene(int sceneId,
                                          QSharedPointer<VideoRepresentation> repr)
{
    if (!repr || sceneId < 0 || sceneId >= repr->scenes.size()) return;
    if (repr->sceneDescriptions.contains(sceneId)
        && !repr->sceneDescriptions[sceneId].isEmpty()) return;

    doDescribeSceneWithCallback(sceneId, repr, nullptr);
}

void VideoAnalysisService::doDescribeSceneWithCallback(
    int sceneId,
    QSharedPointer<VideoRepresentation> repr,
    std::function<void(int)> onDone)
{
    if (!repr || sceneId < 0 || sceneId >= repr->scenes.size()) {
        if (onDone) onDone(sceneId);
        return;
    }

    const Scene& s = repr->scenes[sceneId];
    QList<QImage> frames;
    if (!s.keyframe.isNull()) frames.append(s.keyframe);
    if (frames.isEmpty() && !s.keyframePath.isEmpty()) {
        const QImage persisted(s.keyframePath);
        if (!persisted.isNull()) frames.append(persisted);
    }

    // 若持久化关键帧也缺失，退化为对绑定视频的场景关键时间点截帧
    if (frames.isEmpty() && m_player) {
        const int64_t ts = s.keyframeMs;
        auto fut = m_player->captureFrameAt(repr->metadata.filePath, ts, 2000);
        fut.waitForFinished();
        if (fut.resultCount() > 0) {
            const QImage img = fut.result();
            if (!img.isNull()) frames.append(img);
        }
    }

    // 关键帧缺失时也发出完成回调，避免批次计数死锁
    if (frames.isEmpty()) {
        qWarning() << "[VideoAnalysisService] 场景" << sceneId << "无关键帧，跳过描述";
        if (onDone) onDone(sceneId);
        return;
    }

    // 转换时间戳为可读格式供 LM 参考
    const auto msToTime = [](int64_t ms) -> QString {
        const int h = static_cast<int>(ms / 3600000);
        const int m = static_cast<int>((ms % 3600000) / 60000);
        const int sec = static_cast<int>((ms % 60000) / 1000);
        if (h > 0)
            return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
        return QString("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
    };

    const QString userText = tr("这是视频中 [%1 - %2]（即 %3ms - %4ms）的场景，请描述画面内容。")
                                 .arg(msToTime(s.startMs)).arg(msToTime(s.endMs))
                                 .arg(s.startMs).arg(s.endMs);

    QPointer<VideoAnalysisService> guard(this);
    oneShotVLM(QString::fromUtf8(kSceneDescPrompt),
               userText,
               frames,
               [guard, sceneId, repr, onDone](const QString& desc) {
        if (!guard) return;

        const QString finalDesc = desc.trimmed();
        repr->sceneDescriptions.insert(sceneId, finalDesc);

        if (guard->m_ragStore && !finalDesc.isEmpty()) {
            VideoChunk c;
            const Scene& scene = repr->scenes[sceneId];
            c.chunkId = VideoIndexer::makeChunkId(
                repr->videoId, VideoChunk::SceneSummary,
                scene.startMs, scene.endMs, QString::number(sceneId));
            c.videoId = repr->videoId;
            c.startMs = scene.startMs;
            c.endMs   = scene.endMs;
            c.chunkType = VideoChunk::SceneSummary;
            c.textContent = finalDesc;
            c.keyframePath = scene.keyframePath;
            c.metadata.insert(QStringLiteral("scene_id"), sceneId);
            c.metadata.insert(QStringLiteral("keyframe_ms"),
                              static_cast<qlonglong>(scene.keyframeMs));
            c.metadata.insert(QStringLiteral("file_path"), repr->metadata.filePath);

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
            if (guard->m_embedder && guard->m_embedder->isReady()) {
                c.textEmbedding = guard->m_embedder->embed(finalDesc);
            }
#endif
            guard->m_ragStore->insertChunk(VideoRAGStore::TextSegments, c);
        }

        emit guard->sceneDescribed(sceneId, finalDesc);
        if (onDone) onDone(sceneId);
    });
}

// ============================================================
// Level 2: 全视频摘要
// ============================================================

void VideoAnalysisService::summarizeVideo(QSharedPointer<VideoRepresentation> repr)
{
    if (!repr) return;

    emit analysisProgress(92, tr("生成视频摘要..."));

    // 把时间戳转成可读的 mm:ss 格式
    const auto msToTime = [](int64_t ms) -> QString {
        const int h = static_cast<int>(ms / 3600000);
        const int m = static_cast<int>((ms % 3600000) / 60000);
        const int sec = static_cast<int>((ms % 60000) / 1000);
        if (h > 0)
            return QString("%1:%2:%3").arg(h).arg(m, 2, 10, QChar('0')).arg(sec, 2, 10, QChar('0'));
        return QString("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
    };

    // 组装场景描述列表，包含时间戳便于 LM 生成连贯叙述
    QString scenesText;
    int describedCount = 0;
    for (const Scene& s : repr->scenes) {
        const QString desc = repr->sceneDescriptions.value(s.id);
        if (desc.isEmpty()) continue;
        scenesText += tr("【场景 %1，时间 %2-%3】\n%4\n\n")
                          .arg(s.id + 1)
                          .arg(msToTime(s.startMs))
                          .arg(msToTime(s.endMs))
                          .arg(desc);
        ++describedCount;
    }

    if (scenesText.isEmpty()) {
        qWarning() << "[VideoAnalysisService] 没有任何场景描述，跳过摘要";
        return;
    }

    qDebug() << "[VideoAnalysisService] summarizeVideo: 共"
             << describedCount << "/" << repr->scenes.size() << "个场景有描述";

    // 附加基本元信息辅助 LM 理解上下文
    const QString metaInfo = tr("视频文件：%1，总时长：%2\n\n")
                                 .arg(repr->metadata.fileName)
                                 .arg(msToTime(repr->metadata.durationMs));
    const QString userText = metaInfo + scenesText;

    QPointer<VideoAnalysisService> guard(this);
    oneShotVLM(QString::fromUtf8(kVideoSummaryPrompt),
               userText,
               {},
               [guard, repr](const QString& summary) {
        if (!guard) return;
        if (summary.trimmed().isEmpty()) {
            qWarning() << "[VideoAnalysisService] 摘要生成为空（可能 API 错误或上下文超长）";
            emit guard->analysisProgress(100, tr("分析完成（摘要生成失败）"));
            return;
        }
        repr->videoSummary = summary;
        repr->level = VideoRepresentation::Level2;
        emit guard->analysisProgress(100, tr("分析完成"));
        emit guard->summaryReady(summary);
    });
}

// ============================================================
// Level 3: 单帧 / 区间
// ============================================================

void VideoAnalysisService::describeFrame(const QImage& frame,
                                          int64_t timestampMs,
                                          const QString& focus,
                                          std::function<void(const QString&)> onDone)
{
    if (frame.isNull()) {
        if (onDone) onDone(tr("<空帧>"));
        return;
    }
    const QString userText = focus.isEmpty()
        ? tr("请描述这一帧画面（时间戳 %1ms）。").arg(timestampMs)
        : tr("请描述这一帧画面（时间戳 %1ms），特别关注：%2").arg(timestampMs).arg(focus);

    oneShotVLM(QString::fromUtf8(kFrameDescPrompt),
               userText, { frame }, std::move(onDone));
}

void VideoAnalysisService::analyzeTimeRange(int64_t startMs, int64_t endMs,
                                             const QString& focus,
                                             int sampleCount,
                                             std::function<void(const QString&)> onDone)
{
    if (!m_player || sampleCount <= 0) {
        if (onDone) onDone(tr("<无法采样>"));
        return;
    }
    sampleCount = qBound(2, sampleCount, 10);
    const int64_t step = (endMs - startMs) / qMax(1, sampleCount - 1);

    QList<QImage> frames;
    frames.reserve(sampleCount);
    for (int i = 0; i < sampleCount; ++i) {
        const int64_t ts = startMs + step * i;
        auto fut = m_player->captureFrameAt(ts, 2000);
        fut.waitForFinished();
        if (fut.resultCount() > 0) {
            const QImage img = fut.result();
            if (!img.isNull()) frames.append(img);
        }
    }
    if (frames.isEmpty()) {
        if (onDone) onDone(tr("<截取帧失败>"));
        return;
    }

    const QString userText = tr(
        "以下是视频中 [%1ms - %2ms] 内按时间顺序采样的 %3 帧。请综合分析该段的过程。"
        "%4").arg(startMs).arg(endMs).arg(frames.size())
              .arg(focus.isEmpty() ? QString{} : tr("关注：%1").arg(focus));

    // System prompt 参考 SEQUENCE_ANALYSIS_PROMPT
    const QString sysPrompt = tr(
        "你正在分析一段视频的连续帧序列，请理解帧间的时间关系与运动变化，"
        "综合所有帧回答问题，而不是逐帧独立描述。");

    oneShotVLM(sysPrompt, userText, frames, std::move(onDone));
}

// ============================================================
// VideoContext 组装
// ============================================================

VideoContext VideoAnalysisService::buildVideoContext(
    QSharedPointer<VideoRepresentation> repr) const
{
    VideoContext ctx;
    if (!repr) return ctx;

    ctx.fileName    = repr->metadata.fileName;
    ctx.durationMs  = repr->metadata.durationMs;
    ctx.width       = repr->metadata.width;
    ctx.height      = repr->metadata.height;
    ctx.fps         = repr->metadata.frameRate;
    ctx.hasAudio    = repr->metadata.hasAudio;

    // 场景概览：ID + 时间区间 + 简短标题
    const auto msToTime = [](int64_t ms) -> QString {
        const int h = static_cast<int>(ms / 3600000);
        const int m = static_cast<int>((ms % 3600000) / 60000);
        const int sec = static_cast<int>((ms % 60000) / 1000);
        if (h > 0)
            return QStringLiteral("%1:%2:%3")
                       .arg(h)
                       .arg(m, 2, 10, QChar('0'))
                       .arg(sec, 2, 10, QChar('0'));
        return QStringLiteral("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
    };

    QString overview;
    const int maxScenes = qMin(15, repr->scenes.size());
    for (int i = 0; i < maxScenes; ++i) {
        const Scene& s = repr->scenes[i];
        const QString desc = repr->sceneDescriptions.value(s.id);
        overview += QString::fromUtf8("- [%1-%2] 场景%3%4\n")
                        .arg(msToTime(s.startMs))
                        .arg(msToTime(s.endMs))
                        .arg(s.id)
                        .arg(desc.isEmpty() ? QString{} : QStringLiteral(": ") + desc.left(60));
    }
    if (repr->scenes.size() > maxScenes) {
        overview += tr("...（共 %1 个场景）\n").arg(repr->scenes.size());
    }
    ctx.sceneOverview = overview;
    ctx.videoSummary  = repr->videoSummary;
    return ctx;
}

// ============================================================
// oneShotVLM
// ============================================================

void VideoAnalysisService::oneShotVLM(const QString& sysPrompt,
                                       const QString& userText,
                                       const QList<QImage>& frames,
                                       std::function<void(const QString&)> onDone)
{
    if (!m_agent) {
        if (onDone) onDone(QString{});
        return;
    }

    // 用临时 conversation 承载一次调用，收到 responseFinished 后回调并清理
    const QString convId = QStringLiteral("__vlm_oneshot_")
                            + QUuid::createUuid().toString(QUuid::WithoutBraces);

    // 用 shared_ptr 包装回调，确保 finished 和 error 两条路径都能访问
    auto sharedDone = QSharedPointer<std::function<void(const QString&)>>::create(std::move(onDone));

    QPointer<AgentService> agentGuard(m_agent);
    auto* aggregator = new QObject(this);

    connect(m_agent, &AgentService::responseFinished,
            aggregator, [aggregator, agentGuard, convId, sharedDone](
                            const QString& id, const ChatMessage& msg) {
                if (id != convId) return;
                if (*sharedDone) (*sharedDone)(msg.content);
                if (agentGuard) agentGuard->clearHistory(convId);
                aggregator->deleteLater();
            });

    connect(m_agent, &AgentService::responseError,
            aggregator, [aggregator, agentGuard, convId, sharedDone](
                            const QString& id, const QString& err) {
                if (id != convId) return;
                qWarning() << "[VLM oneShot] error:" << err;
                // 出错也必须触发回调（传空串），否则串行链路会断裂
                if (*sharedDone) (*sharedDone)(QString{});
                if (agentGuard) agentGuard->clearHistory(convId);
                aggregator->deleteLater();
            });

    VideoContext emptyCtx;
    const QString fullUser = sysPrompt + QStringLiteral("\n\n") + userText;
    m_agent->sendMessage(convId, fullUser, frames, emptyCtx);
}
