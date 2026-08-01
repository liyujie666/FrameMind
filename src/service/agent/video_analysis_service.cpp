#include "service/agent/video_analysis_service.h"

#include "service/agentservice.h"
#include "service/playerservice.h"
#include "service/agent/video_indexer.h"
#include "service/rag/video_rag_store.h"
#include "service/rag/audio_visual_aligner.h"

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
#  include "service/embedding_service.h"
#endif

#include <QUuid>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <algorithm>

namespace {
// Prompt 模板：agent-core-design.md §3.2
// 阶段一：纯视觉基线。此处严禁引入任何音频信息，保证视觉事实不被污染。
const char* kSceneDescPrompt = R"PROMPT(
你是一个专业的视频内容分析师。请根据提供的视频帧，用2~4句话描述该场景。

要求：
1. 描述画面中的主要人物、物体和环境
2. 说明正在发生的动作或事件
3. 如有文字/字幕/标志请转录
4. 客观描述，不做主观推断
5. 只描述画面中可见的内容，不要推测画面外的声音、对话或剧情
6. 直接输出描述文字，不要 JSON 格式，不要标题
)PROMPT";

// 阶段二：保守融合。归因约束是这个 prompt 的核心，不能放松。
const char* kSceneFusionPrompt = R"PROMPT(
你是影视内容分析师。你会收到同一时间段的两份独立证据：
  A. 纯视觉描述（由画面生成，是已确认的视觉事实）
  B. 同期音频转写（字幕/ASR，可能是对白、旁白、背景媒体，也可能与画面无关）
另外会给你程序计算出的关联度信号（时间覆盖率、语义相似度、关键词交集、跨场景跨度）
以及一个候选关系，供你参考。

## 判断音画关系，从以下四类中选一个
- strong：对白直接围绕画面中可见的对象、人物或正在发生的事件
- contextual：音频提供与画面相关的背景信息，但不是画面中直接可见的事实
- independent：音频与画面主题不同（如独立解说、无关话题、背景媒体内容）
- unknown：信息不足，无法判断关联

## 归因约束（必须严格遵守）
1. 不得因为台词中出现某个姓名，就确认画面中某张脸或某个人物的身份
2. 不得把画外音、旁白归属给画面中可见的人物
3. 旁白必须表述为"旁白称…"
4. 电视、广播等场景内媒体的声音必须表述为"电视/广播中提到…"
5. 音画无关（independent）时，必须分别描述视觉与音频，不得强行合并成一件事
6. 音频不能覆盖或修改纯视觉描述中的事实
7. 无法确认的对应关系要明确写出"当前证据不足以确认"

## 输出
只输出一个 JSON 对象，不要 Markdown 代码块，不要额外解释：
{
  "relation": "strong|contextual|independent|unknown",
  "confidence": 0.0~1.0,
  "audio_type": "dialogue|narration|background_media|ambient|unknown",
  "audio_summary": "同期音频说了什么的客观摘要，2句以内",
  "fused_description": "结合视觉事实与同期音频的场景描述，2~4句，遵守上述归因约束"
}
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

/**
 * 从模型回复中抽出第一个 JSON 对象。
 *
 * 模型经常会包裹 ```json 代码块或在前后添加说明文字，
 * 这里按第一个 '{' 到最后一个 '}' 截取后再解析。
 */
QJsonObject extractJsonObject(const QString& raw)
{
    const int begin = raw.indexOf(QLatin1Char('{'));
    const int end   = raw.lastIndexOf(QLatin1Char('}'));
    if (begin < 0 || end <= begin) return {};

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(
        raw.mid(begin, end - begin + 1).toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return {};
    return doc.object();
}

QString msToTimeLabel(int64_t ms)
{
    const int h = static_cast<int>(ms / 3600000);
    const int m = static_cast<int>((ms % 3600000) / 60000);
    const int sec = static_cast<int>((ms % 60000) / 1000);
    if (h > 0)
        return QStringLiteral("%1:%2:%3")
                   .arg(h)
                   .arg(m, 2, 10, QChar('0'))
                   .arg(sec, 2, 10, QChar('0'));
    return QStringLiteral("%1:%2").arg(m).arg(sec, 2, 10, QChar('0'));
}

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
                // 检查是否已有场景描述（从数据库加载）
                const bool hasSceneDescriptions = !repr->sceneDescriptions.isEmpty() || 
                                                 !repr->sceneVisualDescriptions.isEmpty();
                
                if (hasSceneDescriptions) {
                    qDebug() << "[VideoAnalysisService] 检测到已有场景描述，跳过大模型调用 | 场景数:" 
                             << repr->scenes.size()
                             << "| 已有描述:" << repr->sceneDescriptions.size()
                             << "| 视觉描述:" << repr->sceneVisualDescriptions.size();
                    
                    // 重放已有的场景描述信号
                    for (auto it = repr->sceneDescriptions.constBegin();
                         it != repr->sceneDescriptions.constEnd(); ++it) {
                        emit sceneDescribed(it.key(), it.value());
                    }
                    
                    // 发出融合信号
                    for (auto it = repr->sceneFusions.constBegin();
                         it != repr->sceneFusions.constEnd(); ++it) {
                        emit sceneFused(it.key(), it.value());
                    }
                    
                    // 如果有视频摘要也发出
                    if (!repr->videoSummary.isEmpty()) {
                        emit summaryReady(repr->videoSummary);
                    } else {
                        // 没有视频摘要，但有场景描述，触发摘要生成
                        summarizeVideo(repr);
                    }
                    return;
                }

                // 没有场景描述，需要调用大模型
                if (!repr->scenes.isEmpty()) {
                    qDebug() << "[VideoAnalysisService] 未找到场景描述，开始调用大模型分析";
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

        // 只有完整融合结果存在时才跳过；旧版 sceneDescriptions 缓存仍需补做融合
        if (repr->sceneFusions.contains(sceneId)
            && repr->sceneFusions.value(sceneId).isValid()) {
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
    if (repr->sceneFusions.contains(sceneId)
        && repr->sceneFusions.value(sceneId).isValid()) return;

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

        // 阶段一产物：纯视觉描述。单独保存，后续融合不会覆盖它。
        const QString visualDesc = desc.trimmed();
        repr->sceneVisualDescriptions.insert(sceneId, visualDesc);
        if (sceneId >= 0 && sceneId < repr->scenes.size()) {
            repr->scenes[sceneId].visualDescription = visualDesc;
        }

        if (visualDesc.isEmpty()) {
            qWarning() << "[VideoAnalysisService] 场景" << sceneId
                       << "视觉描述为空，跳过融合";
            if (onDone) onDone(sceneId);
            return;
        }

        // 阶段二：同期音频对齐 + 语义门控 + 保守融合
        guard->fuseSceneAudio(sceneId, visualDesc, repr, onDone);
    });
}

// ============================================================
// 阶段二：同期音频对齐 + 语义门控 + 保守融合
// ============================================================

void VideoAnalysisService::fuseSceneAudio(
    int sceneId,
    const QString& visualDescription,
    QSharedPointer<VideoRepresentation> repr,
    std::function<void(int)> onDone)
{
    SceneFusion fusion;
    fusion.sceneId = sceneId;
    fusion.visualDescription = visualDescription;
    fusion.fusedDescription  = visualDescription;
    fusion.relation  = AudioVisualRelation::Unknown;
    fusion.audioType = SceneAudioType::None;

    const Scene& scene = repr->scenes[sceneId];

    // 对齐器缺失或无转写数据：直接以纯视觉描述收尾，不调用模型
    if (!m_aligner || repr->speechSegments.isEmpty()) {
        commitSceneFusion(fusion, repr, std::move(onDone));
        return;
    }

    fusion.speechSegments = m_aligner->overlappingSpeechSegments(
        scene, repr->speechSegments);
    if (fusion.speechSegments.isEmpty()) {
        commitSceneFusion(fusion, repr, std::move(onDone));
        return;
    }

    fusion.gate = m_aligner->gate(visualDescription, fusion.speechSegments,
                                  scene, repr->scenes);

    // 先把程序判定填进去，模型解析失败时作为回退结果
    fusion.relation   = fusion.gate.candidate;
    fusion.confidence = fusion.gate.candidateConfidence;

    const QString transcript =
        AudioVisualAligner::formatTranscript(fusion.speechSegments);

    QString gateSignals;
    gateSignals += tr("- 同期语音时间覆盖率：%1\n")
                       .arg(fusion.gate.timeCoverage, 0, 'f', 2);
    if (fusion.gate.hasSemanticSimilarity()) {
        gateSignals += tr("- 视觉描述与转写的语义相似度：%1\n")
                           .arg(fusion.gate.semanticSimilarity, 0, 'f', 2);
    } else {
        gateSignals += tr("- 视觉描述与转写的语义相似度：未计算\n");
    }
    gateSignals += tr("- 关键词交集比例：%1\n")
                       .arg(fusion.gate.keywordOverlap, 0, 'f', 2);
    gateSignals += tr("- 该段音频横跨其他场景的比例：%1\n")
                       .arg(fusion.gate.crossSceneSpan, 0, 'f', 2);
    if (!fusion.gate.sharedKeywords.isEmpty()) {
        gateSignals += tr("- 共现关键词：%1\n")
                           .arg(fusion.gate.sharedKeywords.join(QStringLiteral("、")));
    }
    gateSignals += tr("- 程序候选关系：%1（置信度 %2）\n")
                       .arg(SceneFusion::relationToString(fusion.gate.candidate))
                       .arg(fusion.gate.candidateConfidence, 0, 'f', 2);

    const QString userText =
        tr("## 时间段\n%1 - %2\n\n"
           "## A. 纯视觉描述（已确认的视觉事实）\n%3\n\n"
           "## B. 同期音频转写\n%4\n"
           "## C. 程序计算的关联度信号\n%5")
            .arg(msToTimeLabel(scene.startMs), msToTimeLabel(scene.endMs),
                 visualDescription, transcript, gateSignals);

    QPointer<VideoAnalysisService> guard(this);
    oneShotVLM(QString::fromUtf8(kSceneFusionPrompt),
               userText,
               {},
               [guard, fusion, repr, onDone](const QString& reply) mutable {
        if (!guard) return;

        const QJsonObject obj = extractJsonObject(reply);
        if (obj.isEmpty()) {
            // 模型输出不可解析：保留程序判定，融合描述降级为分述
            qWarning() << "[VideoAnalysisService] 场景" << fusion.sceneId
                       << "融合结果解析失败，回退到程序判定";
            fusion.audioSummary =
                AudioVisualAligner::plainTranscript(fusion.speechSegments);
            fusion.fusedDescription = guard->tr(
                "视觉内容：%1\n同期音频：%2（与画面的关联未经确认）")
                    .arg(fusion.visualDescription, fusion.audioSummary);
            guard->commitSceneFusion(fusion, repr, onDone);
            return;
        }

        fusion.fromModel = true;
        fusion.relation = SceneFusion::relationFromString(
            obj.value(QStringLiteral("relation")).toString());
        fusion.audioType = SceneFusion::audioTypeFromString(
            obj.value(QStringLiteral("audio_type")).toString());
        fusion.audioSummary =
            obj.value(QStringLiteral("audio_summary")).toString().trimmed();

        const QString fused =
            obj.value(QStringLiteral("fused_description")).toString().trimmed();
        if (!fused.isEmpty()) fusion.fusedDescription = fused;

        // 模型置信度与程序候选一致时取较高值，冲突时取较低值（保守）
        const float modelConf = static_cast<float>(
            obj.value(QStringLiteral("confidence")).toDouble(0.0));
        const bool agrees = (fusion.relation == fusion.gate.candidate);
        fusion.confidence = agrees
            ? std::max(modelConf, fusion.gate.candidateConfidence)
            : std::min(std::max(modelConf, 0.3f), 0.6f);

        if (fusion.audioSummary.isEmpty()) {
            fusion.audioSummary =
                AudioVisualAligner::plainTranscript(fusion.speechSegments);
        }

        guard->commitSceneFusion(fusion, repr, onDone);
    });
}

void VideoAnalysisService::commitSceneFusion(
    const SceneFusion& fusion,
    QSharedPointer<VideoRepresentation> repr,
    std::function<void(int)> onDone)
{
    const int sceneId = fusion.sceneId;

    repr->sceneFusions.insert(sceneId, fusion);
    repr->sceneVisualDescriptions.insert(sceneId, fusion.visualDescription);
    // sceneDescriptions 存放最终（融合后）描述，供摘要 / UI / 既有调用方复用
    repr->sceneDescriptions.insert(sceneId, fusion.fusedDescription);

    if (sceneId >= 0 && sceneId < repr->scenes.size()) {
        Scene& scene = repr->scenes[sceneId];
        scene.visualDescription       = fusion.visualDescription;
        scene.audioSummary            = fusion.audioSummary;
        scene.fusedDescription        = fusion.fusedDescription;
        scene.description             = fusion.fusedDescription;
        scene.audioRelation           = fusion.relation;
        scene.audioRelationConfidence = fusion.confidence;
        scene.audioType               = fusion.audioType;
    }

    // 三类证据分别入库，融合描述不覆盖纯视觉事实
    writeSceneEvidence(fusion, repr, VideoChunk::SceneSummary,
                       QStringLiteral("visual"), fusion.visualDescription);
    if (!fusion.audioSummary.isEmpty()) {
        writeSceneEvidence(fusion, repr, VideoChunk::SceneAudio,
                           QStringLiteral("audio"), fusion.audioSummary);
    }
    // 无音频时融合描述与视觉描述相同，不重复占用一条 chunk
    if (fusion.hasAudio() && fusion.fusedDescription != fusion.visualDescription) {
        writeSceneEvidence(fusion, repr, VideoChunk::SceneFused,
                           QStringLiteral("fused"), fusion.fusedDescription);
    }

    qDebug() << "[VideoAnalysisService] 场景" << sceneId << "融合完成"
             << "| relation:" << SceneFusion::relationToString(fusion.relation)
             << "| conf:" << fusion.confidence
             << "| 语音段:" << fusion.speechSegments.size()
             << "| 来自模型:" << fusion.fromModel;

    emit sceneFused(sceneId, fusion);
    emit sceneDescribed(sceneId, fusion.fusedDescription);
    if (onDone) onDone(sceneId);
}

void VideoAnalysisService::writeSceneEvidence(
    const SceneFusion& fusion,
    QSharedPointer<VideoRepresentation> repr,
    VideoChunk::ChunkType chunkType,
    const QString& evidenceType,
    const QString& text)
{
    if (!m_ragStore || text.isEmpty()) return;
    if (fusion.sceneId < 0 || fusion.sceneId >= repr->scenes.size()) return;

    const Scene& scene = repr->scenes[fusion.sceneId];

    VideoChunk c;
    c.chunkId = VideoIndexer::makeChunkId(
        repr->videoId, chunkType, scene.startMs, scene.endMs,
        QString::number(fusion.sceneId));
    c.videoId     = repr->videoId;
    c.startMs     = scene.startMs;
    c.endMs       = scene.endMs;
    c.chunkType   = chunkType;
    c.textContent = text;
    c.keyframePath = scene.keyframePath;

    c.metadata.insert(QStringLiteral("scene_id"), fusion.sceneId);
    c.metadata.insert(QStringLiteral("keyframe_ms"),
                      static_cast<qlonglong>(scene.keyframeMs));
    c.metadata.insert(QStringLiteral("file_path"), repr->metadata.filePath);
    c.metadata.insert(QStringLiteral("evidence_type"), evidenceType);
    c.metadata.insert(QStringLiteral("audio_relation"),
                      SceneFusion::relationToString(fusion.relation));
    c.metadata.insert(QStringLiteral("relation_confidence"), fusion.confidence);
    c.metadata.insert(QStringLiteral("audio_type"),
                      SceneFusion::audioTypeToString(fusion.audioType));
    c.metadata.insert(QStringLiteral("has_speech"), fusion.hasAudio());

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (m_embedder && m_embedder->isReady()) {
        c.textEmbedding = m_embedder->embed(text);
    }
#endif
    m_ragStore->insertChunk(VideoRAGStore::TextSegments, c);
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
