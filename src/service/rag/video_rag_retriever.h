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
    QVector<RetrievalResult> deduplicate(
        const QVector<RetrievalResult>& results) const;

    static float timeOverlapRatio(const VideoChunk& a, const VideoChunk& b);

    VideoRAGStore*    m_store    = nullptr;
    ClipService*      m_clip     = nullptr;
    EmbeddingService* m_embedder = nullptr;
};

#endif // FRAMEMIND_VIDEO_RAG_RETRIEVER_H
