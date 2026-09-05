#ifndef FRAMEMIND_AUDIO_VISUAL_ALIGNER_H
#define FRAMEMIND_AUDIO_VISUAL_ALIGNER_H

#include <QObject>
#include <QString>
#include <QVector>

#include "model/scene.h"
#include "model/speech_segment.h"
#include "model/audio_visual_relation.h"

class EmbeddingService;

/**
 * 场景 ↔ 同期语音的时间对齐与语义门控。
 *
 * 只做程序侧的量化判断，不调用大模型：
 *   1. 按时间重叠找出属于场景的 SpeechSegment；
 *   2. 综合时间覆盖率、语义相似度、关键词交集、跨场景跨度，
 *      给出候选 AudioVisualRelation 与置信度。
 *
 * 输出交给 VLM 做最终确认，避免大模型在没有量化依据时自由归因。
 */
class AudioVisualAligner : public QObject {
    Q_OBJECT
public:
    /// 取段限制（防止长语音整段复制到每个短场景）
    struct Limits {
        int maxSegments = 8;        // 每个场景最多使用的语音段数
        int maxChars    = 1500;     // 转写文本总字符上限
        float minOverlapRatio = 0.05f; // 低于该重叠比例的段直接丢弃
    };

    explicit AudioVisualAligner(QObject* parent = nullptr);

    void setEmbeddingService(EmbeddingService* e) { m_embedder = e; }

    /**
     * 找出与场景时间重叠的语音段。
     *
     * 重叠判定：scene.startMs < speech.endMs && speech.startMs < scene.endMs
     * 按重叠比例降序优先保留，返回时恢复为时间升序并保留原始时间范围。
     */
    QVector<SpeechSegment> overlappingSpeechSegments(
        const Scene& scene,
        const QVector<SpeechSegment>& segments) const;

    QVector<SpeechSegment> overlappingSpeechSegments(
        const Scene& scene,
        const QVector<SpeechSegment>& segments,
        const Limits& limits) const;

    /**
     * 语义门控：综合多个信号给出候选关系。
     *
     * @param visualDescription 第一阶段的纯视觉描述
     * @param sceneSegments     该场景的同期语音（overlappingSpeechSegments 的结果）
     * @param scene             当前场景
     * @param allScenes         全部场景（用于计算音频跨场景跨度）
     */
    AudioVisualGate gate(const QString& visualDescription,
                         const QVector<SpeechSegment>& sceneSegments,
                         const Scene& scene,
                         const QVector<Scene>& allScenes) const;

    /// 把语音段拼成带时间戳的转写文本（供 prompt 使用）
    static QString formatTranscript(const QVector<SpeechSegment>& segments);

    /// 纯文本拼接（不含时间戳，供 embedding 使用）
    static QString plainTranscript(const QVector<SpeechSegment>& segments);

private:
    /// 提取可比较的语义关键词（中文按 2-gram + 英文/数字按词）
    static QStringList extractKeywords(const QString& text);

    EmbeddingService* m_embedder = nullptr;
};

#endif // FRAMEMIND_AUDIO_VISUAL_ALIGNER_H
