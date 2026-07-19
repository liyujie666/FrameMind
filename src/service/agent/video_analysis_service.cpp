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
你是一个专业的视频内容分析师。请根据提供的视频帧序列，生成该片段的结构化描述。

## 输出要求（JSON 格式）
{
  "summary": "一句话概括该片段核心内容",
  "entities": [
    {"id": "自动编号", "type": "person|object|text|location",
     "description": "外观特征描述，确保足以在后续帧中重新识别"}
  ],
  "actions": ["正在发生的动作/事件列表"],
  "setting": "环境/场景描述",
  "interactions": ["实体间的交互关系"],
  "camera": "镜头语言（近景/远景/运动方向）",
  "temporal_cues": "时间线索"
}

## 注意
1. 客观准确，区分"确定看到的"和"推测的"
2. 实体描述要具体到可辨识
3. 画面中如有文字/字幕/标志请准确转录
4. 帧间变化要描述动态过程
)PROMPT";

const char* kVideoSummaryPrompt = R"PROMPT(
你是视频分析师。基于以下场景描述列表，生成一段简明的全视频摘要（200~400 字）。

要求：
- 覆盖主要场景与关键事件
- 说明视频类型/主题（如教学、访谈、监控、剧情）
- 使用中文
- 不要虚构未在场景描述中出现的信息
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

                const int prewarm = qMin(3, repr->scenes.size());
                if (prewarm > 0) {
                    startPrewarmAndSummarize(repr, prewarm);
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
// 预热 + 自动摘要（串行队列）
// ============================================================

void VideoAnalysisService::startPrewarmAndSummarize(
    QSharedPointer<VideoRepresentation> repr, int prewarm)
{
    if (prewarm <= 0) return;

    auto sceneIds = QSharedPointer<QVector<int>>::create();
    sceneIds->reserve(prewarm);
    for (int i = 0; i < prewarm; ++i) {
        sceneIds->append(repr->scenes[i].id);
    }

    QPointer<VideoAnalysisService> guard(this);

    // 将 std::function 本身放进 QSharedPointer，让内外层 lambda 都通过强引用
    // 持有它，避免按引用捕获栈变量导致的悬垂 UB。
    auto describeNext = QSharedPointer<std::function<void(int)>>::create();

    *describeNext = [guard, repr, sceneIds, describeNext](int idx) mutable {
        if (!guard) return;

        if (idx >= sceneIds->size()) {
            qDebug() << "[VideoAnalysisService] 预热完成，触发 summarizeVideo:"
                     << repr->metadata.fileName;
            guard->summarizeVideo(repr);
            return;
        }

        const int sceneId = (*sceneIds)[idx];
        auto nextConn = QSharedPointer<QMetaObject::Connection>::create();
        *nextConn = connect(guard, &VideoAnalysisService::sceneDescribed,
            guard, [guard, repr, sceneIds, describeNext, nextConn,
                    sceneId, idx](int doneId, const QString&) mutable {
                if (doneId != sceneId) return;
                QObject::disconnect(*nextConn);
                (*describeNext)(idx + 1);
            });

        guard->describeScene(sceneId, repr);
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
    // 已描述过则跳过
    if (repr->sceneDescriptions.contains(sceneId)
        && !repr->sceneDescriptions[sceneId].isEmpty()) return;

    doDescribeScene(sceneId, repr);
}

void VideoAnalysisService::doDescribeScene(int sceneId,
                                            QSharedPointer<VideoRepresentation> repr)
{
    const Scene& s = repr->scenes[sceneId];
    QList<QImage> frames;
    if (!s.keyframe.isNull()) frames.append(s.keyframe);

    // 若关键帧缺失，退化为对场景中间时间点截帧
    if (frames.isEmpty() && m_player) {
        auto fut = m_player->captureFrameAt(s.keyframeMs > 0 ? s.keyframeMs
                                                              : (s.startMs + s.endMs) / 2,
                                             2000);
        fut.waitForFinished();
        if (fut.resultCount() > 0) {
            const QImage img = fut.result();
            if (!img.isNull()) frames.append(img);
        }
    }
    if (frames.isEmpty()) return;

    const QString userText = tr("这是视频中 [%1ms - %2ms] 的场景，请生成结构化描述。")
                                 .arg(s.startMs).arg(s.endMs);

    QPointer<VideoAnalysisService> guard(this);
    oneShotVLM(QString::fromUtf8(kSceneDescPrompt),
               userText,
               frames,
               [guard, sceneId, repr](const QString& desc) {
        if (!guard) return;
        repr->sceneDescriptions.insert(sceneId, desc);

        // 写入 RAG store（作为 text_segments）
        if (guard->m_ragStore) {
            VideoChunk c;
            c.chunkId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            c.videoId = repr->videoId;
            c.startMs = repr->scenes[sceneId].startMs;
            c.endMs   = repr->scenes[sceneId].endMs;
            c.chunkType = VideoChunk::SceneSummary;
            c.textContent = desc;
            c.metadata.insert(QStringLiteral("scene_id"), sceneId);

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
            if (guard->m_embedder && guard->m_embedder->isReady()) {
                c.textEmbedding = guard->m_embedder->embed(desc);
            }
#endif
            guard->m_ragStore->insertChunk(VideoRAGStore::TextSegments, c);
        }
        emit guard->sceneDescribed(sceneId, desc);
    });
}

// ============================================================
// Level 2: 全视频摘要
// ============================================================

void VideoAnalysisService::summarizeVideo(QSharedPointer<VideoRepresentation> repr)
{
    if (!repr) return;
    // 组装所有场景描述
    QString scenesText;
    for (const Scene& s : repr->scenes) {
        const QString desc = repr->sceneDescriptions.value(s.id);
        if (desc.isEmpty()) continue;
        scenesText += tr("场景 %1 [%2-%3ms]:\n%4\n\n")
                          .arg(s.id).arg(s.startMs).arg(s.endMs).arg(desc);
    }
    if (scenesText.isEmpty()) return;

    QPointer<VideoAnalysisService> guard(this);
    oneShotVLM(QString::fromUtf8(kVideoSummaryPrompt),
               scenesText,
               {},
               [guard, repr](const QString& summary) {
        if (!guard) return;
        repr->videoSummary = summary;
        repr->level = VideoRepresentation::Level2;
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
    QString overview;
    const int maxScenes = qMin(15, repr->scenes.size());
    for (int i = 0; i < maxScenes; ++i) {
        const Scene& s = repr->scenes[i];
        const QString desc = repr->sceneDescriptions.value(s.id);
        overview += QString::fromUtf8("- [%1-%2ms] 场景%3%4\n")
                        .arg(s.startMs).arg(s.endMs).arg(s.id)
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

    // 使用聚合器捕获 finished
    QPointer<AgentService> agentGuard(m_agent);
    auto* aggregator = new QObject(this);

    auto onFinished = [aggregator, agentGuard, convId, done = std::move(onDone)](
                          const QString& id, const ChatMessage& msg) {
        if (id != convId) return;
        if (done) done(msg.content);
        if (agentGuard) agentGuard->clearHistory(convId);
        aggregator->deleteLater();
    };

    connect(m_agent, &AgentService::responseFinished,
            aggregator, onFinished);
    connect(m_agent, &AgentService::responseError,
            aggregator, [aggregator, convId](const QString& id, const QString& err) {
                if (id != convId) return;
                qWarning() << "[VLM oneShot] error:" << err;
                aggregator->deleteLater();
            });

    // 组装 videoCtx 传空——system prompt 直接写在 text 里
    // AgentService 内部会再加固定的角色定义；这里通过 userText 携带指令
    VideoContext emptyCtx;
    const QString fullUser = sysPrompt + QStringLiteral("\n\n") + userText;
    m_agent->sendMessage(convId, fullUser, frames, emptyCtx);
}
