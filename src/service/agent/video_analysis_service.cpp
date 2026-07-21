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
// ============================================================
// Prompt 模板（优化版：强调上下文关联与叙事连贯性）
// ============================================================

const char* kSceneDescPrompt = R"PROMPT(
你是一个专业的视频内容分析师。请根据提供的视频帧序列，生成该片段的结构化描述。

## 上下文信息
你正在分析一部完整视频中的一个片段。请结合下方提供的"前后场景上下文"，
确保你的描述能够自然衔接前后场景，体现叙事的连续性。

## 输出要求（JSON 格式）
{
  "summary": "一句话概括该片段核心内容（需体现与前后场景的关联）",
  "entities": [
    {"id": "自动编号", "type": "person|object|text|location",
     "description": "外观特征描述，确保足以在后续帧中重新识别",
     "continuity": "是否在前后场景中出现过（new/continuing/returning）"}
  ],
  "actions": ["正在发生的动作/事件列表"],
  "setting": "环境/场景描述（与前一场景相比有何变化）",
  "interactions": ["实体间的交互关系"],
  "camera": "镜头语言（近景/远景/运动方向/切换方式）",
  "temporal_cues": "时间线索（与前后场景的时间关系）",
  "narrative_role": "该场景在整体叙事中的角色（开场/发展/转折/高潮/收尾/过渡）",
  "transition": "与前一场景的过渡方式（硬切/渐变/因果/并列/时间跳跃）"
}

## 注意
1. 客观准确，区分"确定看到的"和"推测的"
2. 实体描述要具体到可辨识，并标注是否与前后场景相同实体
3. 画面中如有文字/字幕/标志请准确转录
4. 帧间变化要描述动态过程
5. 必须参考前后场景上下文，让描述具有连贯性
)PROMPT";

const char* kVideoSummaryPrompt = R"PROMPT(
你是视频分析师。基于以下**所有场景**的结构化描述，生成一段连贯的全视频叙述性摘要。

要求：
- 覆盖所有场景，不得遗漏任何一个（每个场景至少一句话提及）
- 使用时间顺序组织，体现场景间的因果/并列/转折关系
- 说明视频类型/主题（如教学、访谈、监控、剧情、纪录片、Vlog）
- 识别并串联贯穿全片的主线/人物/主题
- 标注关键转折点和高潮段落
- 篇幅：200~500 字（场景多时可适当加长）
- 使用中文
- 不要虚构未在场景描述中出现的信息
- 输出格式：先给一句话总概（20字以内），然后换行写详细摘要
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
                // Level 1 完成 → 全量描述所有场景（替代原来只描述前3个的策略）
                describeAllScenes(repr);
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
}

QSharedPointer<VideoRepresentation> VideoAnalysisService::representation(
    const QString& videoPath) const
{
    return m_indexer ? m_indexer->representation(videoPath) : nullptr;
}

// ============================================================
// Level 2: 批量场景描述（全量覆盖 + 并发控制）
// ============================================================

void VideoAnalysisService::describeAllScenes(QSharedPointer<VideoRepresentation> repr)
{
    if (!repr || repr->scenes.isEmpty()) return;

    // 清空队列状态
    m_describeQueue.clear();
    m_activeTasks     = 0;
    m_describedCount  = 0;
    m_batchMode       = true;

    // 将所有未描述的场景加入队列
    for (const Scene& s : repr->scenes) {
        if (repr->sceneDescriptions.contains(s.id)
            && !repr->sceneDescriptions[s.id].isEmpty()) {
            // 已描述过的跳过
            m_describedCount++;
            continue;
        }
        DescribeTask task;
        task.sceneId = s.id;
        task.repr = repr;
        m_describeQueue.enqueue(task);
    }

    m_totalToDescribe = m_describeQueue.size() + m_describedCount;

    if (m_describeQueue.isEmpty()) {
        // 所有场景都已描述完毕，直接触发摘要
        m_batchMode = false;
        emit allScenesDescribed();
        summarizeVideo(repr);
        return;
    }

    emit analysisProgress(60, tr("开始场景描述 (0/%1)").arg(m_describeQueue.size()));

    // 启动并发处理
    drainDescribeQueue();
}

void VideoAnalysisService::drainDescribeQueue()
{
    while (m_activeTasks < m_maxConcurrent && !m_describeQueue.isEmpty()) {
        DescribeTask task = m_describeQueue.dequeue();
        m_activeTasks++;
        doDescribeSceneWithContext(task.sceneId, task.repr);
    }
}

void VideoAnalysisService::onSceneDescribeFinished(int sceneId,
                                                    QSharedPointer<VideoRepresentation> repr)
{
    m_activeTasks--;
    m_describedCount++;

    const int remaining = m_describeQueue.size();
    const int total = m_totalToDescribe;
    emit analysisProgress(
        60 + 30 * m_describedCount / qMax(1, total),
        tr("场景描述 (%1/%2)").arg(m_describedCount).arg(total));

    emit sceneDescribed(sceneId, repr->sceneDescriptions.value(sceneId));

    if (remaining > 0 || m_activeTasks > 0) {
        // 继续推进队列
        drainDescribeQueue();
    } else {
        // 全部完成
        m_batchMode = false;
        emit allScenesDescribed();
        emit analysisProgress(95, tr("生成全视频摘要"));
        // 全部场景描述完成后，自动生成摘要
        summarizeVideo(repr);
    }
}

// ============================================================
// Level 2: 单场景描述（带上下文）
// ============================================================

void VideoAnalysisService::describeScene(int sceneId,
                                          QSharedPointer<VideoRepresentation> repr)
{
    if (!repr || sceneId < 0 || sceneId >= repr->scenes.size()) return;
    if (repr->sceneDescriptions.contains(sceneId)
        && !repr->sceneDescriptions[sceneId].isEmpty()) return;

    doDescribeSceneWithContext(sceneId, repr);
}

QString VideoAnalysisService::buildSceneContext(int sceneId,
                                                 QSharedPointer<VideoRepresentation> repr) const
{
    if (!repr) return {};

    const int totalScenes = repr->scenes.size();
    QString context;

    // 全局定位
    context += tr("## 全局位置\n");
    context += tr("这是视频的第 %1/%2 个场景").arg(sceneId + 1).arg(totalScenes);

    const Scene& current = repr->scenes[sceneId];
    const int64_t duration = repr->metadata.durationMs;
    if (duration > 0) {
        const double progress = 100.0 * current.startMs / duration;
        context += tr("（位于视频 %.0f%% 处）").arg(progress);
    }
    context += QStringLiteral("。\n\n");

    // 前一个场景的上下文
    if (sceneId > 0) {
        const Scene& prev = repr->scenes[sceneId - 1];
        const QString prevDesc = repr->sceneDescriptions.value(prev.id);
        context += tr("## 前一场景 (场景%1, %2-%3ms)\n").arg(prev.id).arg(prev.startMs).arg(prev.endMs);
        if (!prevDesc.isEmpty()) {
            // 截取摘要部分（前200字符足矣传达上下文）
            context += prevDesc.left(300) + QStringLiteral("\n\n");
        } else {
            context += tr("（尚未描述）\n\n");
        }
    }

    // 后一个场景的上下文（如果已描述）
    if (sceneId < totalScenes - 1) {
        const Scene& next = repr->scenes[sceneId + 1];
        const QString nextDesc = repr->sceneDescriptions.value(next.id);
        context += tr("## 后一场景 (场景%1, %2-%3ms)\n").arg(next.id).arg(next.startMs).arg(next.endMs);
        if (!nextDesc.isEmpty()) {
            context += nextDesc.left(300) + QStringLiteral("\n\n");
        } else {
            context += tr("（尚未描述）\n\n");
        }
    }

    // 如果有全局实体清单，也注入
    if (!repr->entities.isEmpty()) {
        context += tr("## 已知实体列表\n");
        const int maxEntities = qMin(10, repr->entities.size());
        for (int i = 0; i < maxEntities; ++i) {
            const auto& ent = repr->entities[i];
            const QString label = ent.aliases.isEmpty() ? ent.id : ent.aliases.first();
            context += tr("- %1 (%2): %3\n")
                           .arg(label)
                           .arg(EntityProfile::typeToString(ent.type))
                           .arg(ent.primaryDescription.left(50));
        }
        context += QStringLiteral("\n");
    }

    return context;
}

void VideoAnalysisService::doDescribeSceneWithContext(int sceneId,
                                                      QSharedPointer<VideoRepresentation> repr)
{
    const Scene& s = repr->scenes[sceneId];
    QList<QImage> frames;

    // 收集多帧：关键帧 + 场景首帧 + 场景尾帧（提供更丰富的视觉信息）
    if (!s.keyframe.isNull()) {
        frames.append(s.keyframe);
    }

    // 尝试额外截取场景的首尾帧以展示动态过程
    if (m_player && s.durationMs() > 2000) {
        // 截取场景开始处
        auto futStart = m_player->captureFrameAt(s.startMs + 100, 2000);
        futStart.waitForFinished();
        if (futStart.resultCount() > 0) {
            const QImage img = futStart.result();
            if (!img.isNull() && frames.isEmpty()) frames.append(img);
        }

        // 截取场景结束处（展示变化）
        auto futEnd = m_player->captureFrameAt(s.endMs - 100, 2000);
        futEnd.waitForFinished();
        if (futEnd.resultCount() > 0) {
            const QImage img = futEnd.result();
            if (!img.isNull()) frames.append(img);
        }
    }

    // 兜底：中间时间点截帧
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
    if (frames.isEmpty()) {
        // 无法获取帧，跳过并通知完成
        if (m_batchMode) onSceneDescribeFinished(sceneId, repr);
        return;
    }

    // 构建带上下文的 user prompt
    const QString sceneContext = buildSceneContext(sceneId, repr);
    const QString userText = sceneContext
        + tr("\n## 当前场景\n时间范围: [%1ms - %2ms]，时长 %3ms\n请生成结构化描述。")
              .arg(s.startMs).arg(s.endMs).arg(s.durationMs());

    QPointer<VideoAnalysisService> guard(this);
    const bool batchMode = m_batchMode;

    oneShotVLM(QString::fromUtf8(kSceneDescPrompt),
               userText,
               frames,
               [guard, sceneId, repr, batchMode](const QString& desc) {
        if (!guard) return;

        if (desc.isEmpty()) {
            // VLM 返回空结果（可能调用失败），仍需推进队列
            qWarning() << "[VideoAnalysis] 场景" << sceneId << "描述为空，跳过";
            if (batchMode) {
                guard->onSceneDescribeFinished(sceneId, repr);
            }
            return;
        }

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
            c.metadata.insert(QStringLiteral("scene_total"), repr->scenes.size());
            c.metadata.insert(QStringLiteral("scene_position"),
                              static_cast<double>(sceneId) / qMax(1, repr->scenes.size() - 1));

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
            if (guard->m_embedder && guard->m_embedder->isReady()) {
                c.textEmbedding = guard->m_embedder->embed(desc);
            }
#endif
            guard->m_ragStore->insertChunk(VideoRAGStore::TextSegments, c);
        }

        if (batchMode) {
            guard->onSceneDescribeFinished(sceneId, repr);
        } else {
            emit guard->sceneDescribed(sceneId, desc);
        }
    });
}

// ============================================================
// Level 2: 全视频摘要
// ============================================================

void VideoAnalysisService::summarizeVideo(QSharedPointer<VideoRepresentation> repr)
{
    if (!repr) return;

    // 组装所有场景描述（按时间顺序）
    QString scenesText;
    int describedCount = 0;
    for (const Scene& s : repr->scenes) {
        const QString desc = repr->sceneDescriptions.value(s.id);
        if (desc.isEmpty()) continue;
        describedCount++;
        scenesText += tr("### 场景 %1 [%2ms - %3ms, 时长 %4ms]\n%5\n\n")
                          .arg(s.id).arg(s.startMs).arg(s.endMs)
                          .arg(s.durationMs()).arg(desc);
    }
    if (scenesText.isEmpty()) return;

    // 添加全局信息
    QString fullInput;
    fullInput += tr("## 视频基本信息\n");
    fullInput += tr("- 文件名: %1\n").arg(repr->metadata.fileName);
    fullInput += tr("- 总时长: %1ms (%2秒)\n")
                     .arg(repr->metadata.durationMs)
                     .arg(repr->metadata.durationMs / 1000.0, 0, 'f', 1);
    fullInput += tr("- 分辨率: %1x%2\n").arg(repr->metadata.width).arg(repr->metadata.height);
    fullInput += tr("- 场景总数: %1，已描述: %2\n\n").arg(repr->scenes.size()).arg(describedCount);
    fullInput += tr("## 各场景结构化描述\n\n");
    fullInput += scenesText;

    QPointer<VideoAnalysisService> guard(this);
    oneShotVLM(QString::fromUtf8(kVideoSummaryPrompt),
               fullInput,
               {},
               [guard, repr](const QString& summary) {
        if (!guard) return;
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

    // 场景概览：全部场景（而非截断到15个），但摘要限长
    QString overview;
    for (int i = 0; i < repr->scenes.size(); ++i) {
        const Scene& s = repr->scenes[i];
        const QString desc = repr->sceneDescriptions.value(s.id);
        overview += QString::fromUtf8("- [%1-%2ms] 场景%3%4\n")
                        .arg(s.startMs).arg(s.endMs).arg(s.id)
                        .arg(desc.isEmpty() ? QString{}
                                            : QStringLiteral(": ") + desc.left(80));
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

    const QString convId = QStringLiteral("__vlm_oneshot_")
                            + QUuid::createUuid().toString(QUuid::WithoutBraces);

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
            aggregator, [aggregator, convId, this](const QString& id, const QString& err) {
                if (id != convId) return;
                qWarning() << "[VLM oneShot] error:" << err;
                emit analysisError(tr("VLM 调用失败: %1").arg(err));
                aggregator->deleteLater();
            });

    VideoContext emptyCtx;
    const QString fullUser = sysPrompt + QStringLiteral("\n\n") + userText;
    m_agent->sendMessage(convId, fullUser, frames, emptyCtx);
}
