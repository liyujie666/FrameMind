#ifndef FRAMEMIND_QA_CACHE_MANAGER_H
#define FRAMEMIND_QA_CACHE_MANAGER_H

#include <QObject>
#include <QString>
#include <QtGlobal>
#include <QVector>
#include <optional>
#include <vector>

#include "model/retrieval_result.h"

class VideoRAGStore;
class EmbeddingService;

/**
 * QA 缓存管理器（agent-core-design.md §9.6）。
 *
 * 场景：
 *   - 同一会话内相似问题复用之前的答案，避免重复分析
 *   - 跨会话打开同一视频时也可命中
 *
 * 关键阈值：默认 0.88（较高），只有非常相似的问题才复用
 * 命中后仍允许用户请求"重新分析"覆盖。
 */
class QACacheManager : public QObject {
    Q_OBJECT
public:
    struct CachedAnswer {
        QString question;
        QString answer;
        float   originalConfidence = 0.0f;
        float   similarity = 0.0f;   // 与当前提问的相似度
        int64_t startMs = 0;         // 证据范围
        int64_t endMs   = 0;
        QVector<int> evidenceSceneIds;
    };

    explicit QACacheManager(VideoRAGStore* store,
                            EmbeddingService* embedder,
                            QObject* parent = nullptr);

    /// 设置命中阈值（默认 0.88）
    void setThreshold(float thr) { m_threshold = qBound(0.0f, thr, 1.0f); }
    float threshold() const { return m_threshold; }

    /// 控制缓存结论最大有效期，过期内容必须重新检索原始证据。
    void setMaxAgeDays(int days) { m_maxAgeDays = qMax(0, days); }
    int maxAgeDays() const { return m_maxAgeDays; }

    /// 缓存一次成功的 QA
    /// @param evidenceScenes 该次回答涉及的场景 ID 列表（用于评估证据范围）
    void cache(const QString& videoId,
               const QString& question,
               const QString& answer,
               float confidence,
               const QVector<int>& evidenceSceneIds = {});

    /// 尝试从缓存回答
    /// 返回 std::nullopt 表示未命中；命中时返回缓存内容 + 相似度
    std::optional<CachedAnswer> tryAnswer(const QString& videoId,
                                           const QString& question);

    /// 清空某视频的缓存
    void clearVideo(const QString& videoId);

private:
    std::vector<float> encodeQuery(const QString& question) const;

    VideoRAGStore*    m_store   = nullptr;
    EmbeddingService* m_embedder = nullptr;
    float             m_threshold = 0.88f;
    int               m_maxAgeDays = 7;
};

#endif // FRAMEMIND_QA_CACHE_MANAGER_H
