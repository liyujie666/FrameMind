#ifndef FRAMEMIND_ENTITY_TRACKER_H
#define FRAMEMIND_ENTITY_TRACKER_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QHash>
#include <optional>

#include "model/entity_profile.h"

class VideoRAGStore;
class EmbeddingService;

/**
 * 实体追踪与共指消解（agent-core-design.md §6.3）。
 *
 * 职责：
 *   1. 从场景描述中提取实体，注册到档案
 *   2. 判断"新出现的人物"是不是已有实体（视觉/描述相似度匹配）
 *   3. 解析用户的指代表达（"那个人"、"刚才的红衣服"、"开头出现的女孩"）
 */
class EntityTracker : public QObject {
    Q_OBJECT
public:
    explicit EntityTracker(VideoRAGStore* store,
                            EmbeddingService* embedder,
                            QObject* parent = nullptr);

    /// 相似度阈值（默认 0.75）
    void setSimilarityThreshold(float thr) { m_similarity = thr; }

    // ---- 注册与匹配 ----

    /**
     * 注册新出现的实体或与已有实体匹配。
     * @return 该实体的 id（若匹配到已有实体则返回旧 id）
     */
    QString registerEntity(const QString& videoId,
                            EntityProfile::EntityType type,
                            const QString& description,
                            int sceneId,
                            int64_t timestampMs);

    /// 尝试在已注册实体中找到与 description 最相似的
    std::optional<EntityProfile> findMatching(const QString& videoId,
                                                const QString& description) const;

    // ---- 指代消解 ----

    /**
     * 解析用户的指代表达 → 具体实体。
     * @param userReference "那个人" / "红衣服" / "视频开头的女孩" 等
     * @param anchorTsMs   当前对话锚点时间（用于"刚才的"这类相对指代）
     */
    std::optional<EntityProfile> resolveReference(
        const QString& videoId,
        const QString& userReference,
        int64_t anchorTsMs = -1) const;

    /// 列出某视频的全部实体
    QVector<EntityProfile> listEntities(const QString& videoId) const;

    /// 重新加载（视频切换时调用）
    void reload(const QString& videoId);

signals:
    void entityRegistered(const EntityProfile& entity);

private:
    QString generateEntityId(EntityProfile::EntityType type) const;
    std::vector<float> encodeDesc(const QString& desc) const;

    VideoRAGStore*    m_store    = nullptr;
    EmbeddingService* m_embedder = nullptr;
    float             m_similarity = 0.75f;

    // 内存缓存：videoId → [entities]
    mutable QHash<QString, QVector<EntityProfile>> m_cache;
    mutable QHash<QString, int> m_counters;  // videoId → 下一个 entity 序号
};

#endif // FRAMEMIND_ENTITY_TRACKER_H
