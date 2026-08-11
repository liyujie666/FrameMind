#include "service/agent/video_analysis_service.h"

#include "service/agent/one_shot_vlm_channel.h"
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
你是一个专业的视频内容分析师。请根据按时间顺序提供的视频帧，提取可验证的纯视觉证据。

要求：
1. 只描述画面中可见的人物、物体、环境、动作和状态变化，不推测画面外声音、对话或剧情
2. 画面文字/字幕/招牌/界面文字只转录清晰可辨的部分；看不清则不要猜测
3. action 与 visible_text 仅记录肉眼可见事实，不得从音频或常识补全
4. visual_description 用2~4句中文客观描述场景
5. 只输出一个 JSON 对象，不要 Markdown 代码块或额外说明：
{
  "visual_description": "...",
  "visible_text": ["..."],
  "actions": ["..."],
  "uncertain": ["..."],
  "confidence": 0.0
}
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

QStringList jsonStringList(const QJsonObject& object, const QString& key, int maxItems = 12)
{
    QStringList result;
    const QJsonArray values = object.value(key).toArray();
    for (const QJsonValue& value : values) {
        const QString text = value.toString().simplified();
        if (!text.isEmpty() && !result.contains(text)) result.append(text);
        if (result.size() >= maxItems) break;
    }
    return result;
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

VideoAnalysisService::VideoAnalysisService(OneShotVlmChannel* vlmChannel,
                                           VideoIndexer*      indexer,
                                           VideoRAGStore*     ragStore,
                                           PlayerService*     player,
                                           QObject*           parent)
    : QObject(parent)
    , m_vlmChannel(vlmChannel)
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
    const QString videoId = VideoIndexer::computeVideoId(videoPath);
    if (m_vlmChannel && !m_backgroundVideoId.isEmpty()
        && m_backgroundVideoId != videoId) {
        m_vlmChannel->cancelBackground(m_backgroundVideoId);
    }
    m_backgroundVideoId = videoId;
    if (m_ragStore) {
        // 先尝试加载已有索引；QA 缓存只能在其原始证据也仍存在时复用。
        m_ragStore->loadVideo(videoId);
        if (m_ragStore->hasIndexedContent(videoId)) {
            emit analysisProgress(100, tr("已加载持久化视频索引"));
            return;
        }
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
    for (const SceneFrame& representative : s.representativeFrames) {
        QImage image = representative.image;
        if (image.isNull() && !representative.imagePath.isEmpty()) {
            image.load(representative.imagePath);
        }
        if (!image.isNull()) frames.append(image);
    }
    if (frames.isEmpty() && !s.keyframe.isNull()) frames.append(s.keyframe);
    if (frames.isEmpty() && !s.keyframePath.isEmpty()) {
        const QImage persisted(s.keyframePath);
        if (!persisted.isNull()) frames.append(persisted);
    }

    // 若持久化代表帧也缺失，退化为对绑定视频的场景关键时间点截帧
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

    const QString userText = tr("这是视频中 [%1 - %2]（即 %3ms - %4ms）的场景。提供的帧按时间顺序覆盖开始、中间和结束状态；请描述可见内容及状态变化。")
                                 .arg(msToTime(s.startMs)).arg(msToTime(s.endMs))
                                 .arg(s.startMs).arg(s.endMs);

    QPointer<VideoAnalysisService> guard(this);
    oneShotVLM(QString::fromUtf8(kSceneDescPrompt),
               userText,
               frames,
               false,
               repr->videoId,
               [guard, sceneId, repr, onDone](const QString& reply) {
        if (!guard) return;

        // 阶段一产物：优先解析结构化纯视觉证据；旧模型或异常输出时保留原文回退。
        const QJsonObject object = extractJsonObject(reply);
        const QString visualDesc = object.value(QStringLiteral("visual_description"))
            .toString().trimmed().isEmpty() ? reply.trimmed()
            : object.value(QStringLiteral("visual_description")).toString().trimmed();
        repr->sceneVisualDescriptions.insert(sceneId, visualDesc);
        if (sceneId >= 0 && sceneId < repr->scenes.size()) {
            Scene& scene = repr->scenes[sceneId];
            scene.visualDescription = visualDesc;
            scene.visibleTexts = jsonStringList(object, QStringLiteral("visible_text"));
            scene.visibleActions = jsonStringList(object, QStringLiteral("actions"));
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
               false,
               repr->videoId,
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
    // 可见文字独立入库，供字幕、招牌、PPT 等精确文字问题走文本检索；
    // 当前来源是 VLM 可见文字转录，未来专用 OCR 可作为同类型补充来源。
    if (sceneId >= 0 && sceneId < repr->scenes.size()
        && !repr->scenes[sceneId].visibleTexts.isEmpty()) {
        writeSceneEvidence(fusion, repr, VideoChunk::Event,
                           QStringLiteral("visible_text"),
                           repr->scenes[sceneId].visibleTexts.join(QStringLiteral("\n")));
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
    if (evidenceType == QLatin1String("visible_text")) {
        c.metadata.insert(QStringLiteral("source"), QStringLiteral("vlm_visible_text"));
    }
    c.metadata.insert(QStringLiteral("audio_relation"),
                      SceneFusion::relationToString(fusion.relation));
    c.metadata.insert(QStringLiteral("relation_confidence"), fusion.confidence);
    c.metadata.insert(QStringLiteral("audio_type"),
                      SceneFusion::audioTypeToString(fusion.audioType));
    c.metadata.insert(QStringLiteral("has_speech"), fusion.hasAudio());
    c.metadata.insert(QStringLiteral("visible_texts"), scene.visibleTexts);
    c.metadata.insert(QStringLiteral("visible_actions"), scene.visibleActions);
    c.metadata.insert(QStringLiteral("embedding_model_id"), QStringLiteral("bge_text"));
    c.metadata.insert(QStringLiteral("embedding_version"), QStringLiteral("passage_v2"));
    c.metadata.insert(QStringLiteral("index_version"), 2);

#ifdef FRAMEMIND_HAS_ONNXRUNTIME
    if (m_embedder && m_embedder->isReady()) {
        c.textEmbedding = m_embedder->embedPassage(text);
        c.metadata.insert(QStringLiteral("embedding_dimension"),
                          static_cast<int>(c.textEmbedding.size()));
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
               false,
               repr->videoId,
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
               userText, { frame }, true, {}, std::move(onDone));
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

    oneShotVLM(sysPrompt, userText, frames, true, {}, std::move(onDone));
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
                                       bool interactive,
                                       const QString& cancellationKey,
                                       std::function<void(const QString&)> onDone)
{
    if (!m_vlmChannel) {
        if (onDone) onDone({});
        return;
    }
    m_vlmChannel->enqueue(sysPrompt, userText, frames,
                           interactive ? OneShotVlmChannel::Priority::Interactive
                                       : OneShotVlmChannel::Priority::Background,
                           cancellationKey, std::move(onDone));
}
