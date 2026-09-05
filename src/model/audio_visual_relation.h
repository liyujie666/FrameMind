#ifndef FRAMEMIND_AUDIO_VISUAL_RELATION_H
#define FRAMEMIND_AUDIO_VISUAL_RELATION_H

#include <QString>
#include <QStringList>
#include <QVector>
#include <QMetaType>

#include "model/speech_segment.h"

/**
 * 音画语义关系（video_rag 音视频融合方案 §第三阶段）。
 *
 * 用于表达"同期音频与当前画面的关联强度"，决定融合描述的归因保守程度。
 */
enum class AudioVisualRelation {
    Strong,        // 对白直接围绕可见对象或事件
    Contextual,    // 音频提供相关背景，但不是画面中的直接可见事实
    Independent,   // 音频与画面主题不同（解说/背景媒体/无关内容）
    Unknown        // 信息不足，无法判定
};

/// 同期音频的粗分类。CLAP 接入后可扩展为环境声/音乐等更细的标签。
enum class SceneAudioType {
    Dialogue,      // 同期对白
    Narration,     // 旁白 / 画外解说
    BackgroundMedia, // 电视 / 广播 / 场景内播放的媒体
    Ambient,       // 环境声为主，语音信息量低
    None,          // 无同期语音
    Unknown
};

/**
 * 程序侧语义门控的打分明细。
 *
 * 只做"候选判定"，最终 relation 由 VLM 在拿到这些信号后确认，
 * 目的是避免大模型在没有任何量化依据的情况下自由归因。
 */
struct AudioVisualGate {
    /// 同期语音对场景时长的覆盖比例 [0,1]
    float timeCoverage = 0.0f;

    /// 视觉描述 embedding 与转写文本 embedding 的余弦相似度 [-1,1]
    /// EmbeddingService 不可用时保持哨兵值 -2（表示未计算，区别于真实的 -1）
    static constexpr float kSimilarityNotComputed = -2.0f;
    float semanticSimilarity = kSimilarityNotComputed;

    /// 视觉描述与转写文本的关键词交集比例 [0,1]
    float keywordOverlap = 0.0f;

    /// 同期语音是否横跨大量场景（长段解说的特征），跨得越多越可能与单场景无关
    float crossSceneSpan = 0.0f;

    /// 转写文本总字符数
    int transcriptChars = 0;

    /// 程序判定的候选关系
    AudioVisualRelation candidate = AudioVisualRelation::Unknown;

    /// 候选关系的置信度 [0,1]
    float candidateConfidence = 0.0f;

    /// 参与判定的关键词交集（供 prompt 与调试展示）
    QStringList sharedKeywords;

    bool hasSemanticSimilarity() const
    {
        return semanticSimilarity > kSimilarityNotComputed + 0.5f;
    }
};

/**
 * 单个场景的音视频融合结果。
 *
 * 三类证据分别保留，融合描述不覆盖纯视觉事实：
 *   visualDescription  仅由关键帧生成，禁止音频参与
 *   audioSummary       同期音频的独立摘要
 *   fusedDescription   保守融合后的场景描述
 */
struct SceneFusion {
    int sceneId = -1;

    QString visualDescription;
    QString audioSummary;
    QString fusedDescription;

    AudioVisualRelation relation = AudioVisualRelation::Unknown;
    float               confidence = 0.0f;
    SceneAudioType      audioType = SceneAudioType::None;

    /// 参与融合的同期语音段（保留原始时间范围）
    QVector<SpeechSegment> speechSegments;

    /// 程序门控明细
    AudioVisualGate gate;

    /// VLM 是否成功返回结构化结果（false 表示走了程序回退路径）
    bool fromModel = false;

    bool hasAudio() const { return !speechSegments.isEmpty(); }
    bool isValid() const { return sceneId >= 0 && !visualDescription.isEmpty(); }

    // ---- 枚举 <-> 字符串 ----

    static QString relationToString(AudioVisualRelation r)
    {
        switch (r) {
        case AudioVisualRelation::Strong:      return QStringLiteral("strong");
        case AudioVisualRelation::Contextual:  return QStringLiteral("contextual");
        case AudioVisualRelation::Independent: return QStringLiteral("independent");
        case AudioVisualRelation::Unknown:     break;
        }
        return QStringLiteral("unknown");
    }

    static AudioVisualRelation relationFromString(const QString& s)
    {
        const QString v = s.trimmed().toLower();
        if (v == QLatin1String("strong"))      return AudioVisualRelation::Strong;
        if (v == QLatin1String("contextual"))  return AudioVisualRelation::Contextual;
        if (v == QLatin1String("independent")) return AudioVisualRelation::Independent;
        return AudioVisualRelation::Unknown;
    }

    static QString relationLabel(AudioVisualRelation r)
    {
        switch (r) {
        case AudioVisualRelation::Strong:      return QStringLiteral("音画强相关");
        case AudioVisualRelation::Contextual:  return QStringLiteral("音频提供背景");
        case AudioVisualRelation::Independent: return QStringLiteral("音画不相关");
        case AudioVisualRelation::Unknown:     break;
        }
        return QStringLiteral("关系未定");
    }

    static QString audioTypeToString(SceneAudioType t)
    {
        switch (t) {
        case SceneAudioType::Dialogue:        return QStringLiteral("dialogue");
        case SceneAudioType::Narration:       return QStringLiteral("narration");
        case SceneAudioType::BackgroundMedia: return QStringLiteral("background_media");
        case SceneAudioType::Ambient:         return QStringLiteral("ambient");
        case SceneAudioType::None:            return QStringLiteral("none");
        case SceneAudioType::Unknown:         break;
        }
        return QStringLiteral("unknown");
    }

    static SceneAudioType audioTypeFromString(const QString& s)
    {
        const QString v = s.trimmed().toLower();
        if (v == QLatin1String("dialogue"))         return SceneAudioType::Dialogue;
        if (v == QLatin1String("narration"))        return SceneAudioType::Narration;
        if (v == QLatin1String("background_media")) return SceneAudioType::BackgroundMedia;
        if (v == QLatin1String("ambient"))          return SceneAudioType::Ambient;
        if (v == QLatin1String("none"))             return SceneAudioType::None;
        return SceneAudioType::Unknown;
    }
};

Q_DECLARE_METATYPE(SceneFusion)

#endif // FRAMEMIND_AUDIO_VISUAL_RELATION_H
