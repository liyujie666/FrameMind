#ifndef FRAMEMIND_RETRIEVAL_RESULT_H
#define FRAMEMIND_RETRIEVAL_RESULT_H

#include <QString>
#include <QImage>
#include <QVariantMap>
#include <QMetaType>
#include <cstdint>
#include <vector>

/**
 * RAG 检索单元（VideoChunk），也是检索结果的载体。
 * 对齐 agent-core-design.md §9.3。
 *
 * 一个 chunk 必须能独立提供有意义的信息，携带时间定位与结构化元数据，
 * 双 embedding（text + visual）支持多路检索。
 */
struct VideoChunk {
    /// 检索单元的类型
    enum ChunkType {
        SceneSummary,    // 场景语义描述
        SpeechSegment,   // 语音转写段
        Event,           // 事件描述
        FrameDesc,       // 关键帧描述
        QAcache          // 历史问答缓存
    };

    QString chunkId;
    QString videoId;

    // 时间定位（每个 chunk 必须有明确的时间区间）
    int64_t startMs = 0;
    int64_t endMs   = 0;

    // 文本表示（必有）
    QString              textContent;
    std::vector<float>   textEmbedding;

    // 视觉表示（可选，帧级 chunk 有）
    std::vector<float>   frameEmbedding;
    QString              keyframePath;

    ChunkType chunkType = SceneSummary;

    /// 结构化元数据（entity_ids / actions / has_speech / motion_level / scene_id ...）
    QVariantMap metadata;

    bool isValid() const { return endMs >= startMs && !chunkId.isEmpty(); }
    int64_t durationMs() const { return endMs - startMs; }
};

/**
 * 单次检索返回的结果（chunk + 融合分数 + 命中来源）。
 * agent-core-design.md §9.4 RRF 融合后按 score 排序。
 */
struct RetrievalResult {
    VideoChunk chunk;
    float      score = 0.0f;            // 融合后分数
    QString    hitPath;                 // "visual" | "text" | "entity" | "qa_cache"

    // 供 UI 展示时按需加载
    QImage keyframeThumb;

    bool operator<(const RetrievalResult& other) const
    {
        return score > other.score;     // 逆序：分数大的在前
    }
};

Q_DECLARE_METATYPE(VideoChunk)
Q_DECLARE_METATYPE(RetrievalResult)

#endif // FRAMEMIND_RETRIEVAL_RESULT_H
