#ifndef FRAMEMIND_VIDEO_RAG_RETRIEVER_H
#define FRAMEMIND_VIDEO_RAG_RETRIEVER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QMap>
#include <QVariantMap>

#include "model/retrieval_result.h"

class VideoRAGStore;
class ClipService;
class EmbeddingService;

/**
 * 视频 RAG 多路检索器（agent-core-design.md §9.4）。
 *
 * 三路召回 + RRF 融合排序：
 *   - Path A (text)   BGE 编码 → text_segments 语义搜索
 *   - Path B (visual) CLIP text encoder → visual_frames 图文检索
 *   - Path C (entity) BGE → entity_profiles 匹配（用于"那个人"等指代）
 *   - Path D (qa_cache) BGE → qa_cache（前置由 QACacheManager 处理，此处可选）
 *
 * 融合算法：RRF (Reciprocal Rank Fusion, k=60)。
 * 权重根据 QueryIntent（视觉侧重 / 文本侧重）动态调整。
 */
class VideoRAGRetriever : public QObject {
    Q_OBJECT
public:
    /// 检索约束
    struct Constraints {
        QString  videoId;
        int64_t  startMsGte = -1;
        int64_t  endMsLte   = -1;
        VideoChunk::ChunkType chunkType = static_cast<VideoChunk::ChunkType>(-1);
        QStringList entityIds;
        float    minScore = 0.0f;
    };

    /// 查询意图（简单启发式判断）
    struct QueryIntent {
        bool needsTextSearch   = true;
        bool needsVisualSearch = true;
        bool hasEntityReference = false;

        QString entityDesc;   // 若 hasEntityReference，提取的实体描述
        double weightText     = 0.4;
        double weightVisual   = 0.4;
        double weightEntity   = 0.2;

        /**
         * 证据类型偏好（音视频融合后同一场景有视觉/音频/融合三份证据）。
         *
         * 画面类问题压低音频证据，台词类问题压低纯视觉证据，
         * 叙事类问题（"这段发生了什么"）优先融合证据。
         */
        bool prefersVisualEvidence = false;
        bool prefersAudioEvidence  = false;
        bool prefersFusedEvidence  = true;

        /// 按证据类型给出的相似度乘子
        float evidenceWeight(VideoChunk::ChunkType t) const;
    };

    explicit VideoRAGRetriever(VideoRAGStore* store,
                                QObject* parent = nullptr);

    void setClipService(ClipService* clip)          { m_clip = clip; }
    void setEmbeddingService(EmbeddingService* e)   { m_embedder = e; }

    /// 主入口：多路检索 + 融合排序
    QVector<RetrievalResult> retrieve(const QString& query,
                                       const Constraints& constraints,
                                       int topK = 5);

    /// 查询意图分析（可单独使用）
    QueryIntent analyzeQuery(const QString& query) const;

private:
    /// Path A: 文本语义检索（BGE 在 text_segments）
    QVector<RetrievalResult> textPathSearch(const QString& query,
                                              const Constraints& c, int topK);
    /// Path B: 视觉语义检索（CLIP text→visual_frames）
    QVector<RetrievalResult> visualPathSearch(const QString& query,
                                                const Constraints& c, int topK);
    /// Path C: 实体检索
    QVector<RetrievalResult> entityPathSearch(const QString& entityDesc,
                                                const Constraints& c, int topK);

    /// RRF 融合：agent-core-design.md §9.4
    /// score(doc) = Σ weight_i * 1/(k + rank_i(doc))
    QVector<RetrievalResult> reciprocalRankFusion(
        const QMap<QString, QVector<RetrievalResult>>& perPath,
        const QMap<QString, double>& weights,
        int k = 60);

    /// 结果去重（按时间重叠度）
    /// 同一场景的视觉/音频/融合证据互补，不互相去重
    QVector<RetrievalResult> deduplicate(
        const QVector<RetrievalResult>& results) const;

    /// 按查询意图对证据类型重新加权（在 RRF 之前作用于单路分数）
    static void applyEvidencePreference(QVector<RetrievalResult>& results,
                                        const QueryIntent& intent);

    static float timeOverlapRatio(const VideoChunk& a, const VideoChunk& b);

    VideoRAGStore*    m_store    = nullptr;
    ClipService*      m_clip     = nullptr;
    EmbeddingService* m_embedder = nullptr;
};

#endif // FRAMEMIND_VIDEO_RAG_RETRIEVER_H
