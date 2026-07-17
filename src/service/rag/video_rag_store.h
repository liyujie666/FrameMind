#ifndef FRAMEMIND_VIDEO_RAG_STORE_H
#define FRAMEMIND_VIDEO_RAG_STORE_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QVariantMap>
#include <memory>
#include <vector>

#include "model/retrieval_result.h"
#include "model/entity_profile.h"

class DatabaseManager;

/**
 * Video RAG 统一存储（agent-core-design.md §9.2）。
 *
 * 四个 Collection：
 *   - visual_frames    帧级 CLIP embedding
 *   - text_segments    场景描述 / 语音转写 / 事件 的文本 embedding
 *   - entity_profiles  实体档案的语义 embedding
 *   - qa_cache         历史问答 embedding（复用规避重复分析）
 *
 * 实现选择（本项目单机、单视频规模）：
 *   - 向量：内存中维护每 collection 一个 vector<pair<VideoChunk, embedding>>
 *           后续可替换为 FAISS IndexFlatIP（同 API）
 *   - 持久化：SQLite 表 rag_chunks + rag_entities
 *   - 每次视频加载时惰性 loadFromDb(videoId) 到内存
 *
 * 线程安全：所有 public 方法用互斥锁保护；插入/检索都可跨线程调用。
 */
class VideoRAGStore : public QObject {
    Q_OBJECT
public:
    /// 集合枚举（用于 API 的 collection 参数）
    enum Collection {
        VisualFrames,
        TextSegments,
        EntityProfiles,
        QACache
    };
    Q_ENUM(Collection)

    /// 检索过滤器
    struct Filter {
        QString  videoId;
        int64_t  startMsGte = -1;              // -1 = 不限
        int64_t  endMsLte   = -1;
        VideoChunk::ChunkType chunkType = static_cast<VideoChunk::ChunkType>(-1); // -1 = 不限
        QStringList entityIds;                 // metadata.entities 必须包含
        float    minScore = 0.0f;              // 分数阈值
    };

    explicit VideoRAGStore(DatabaseManager* db, QObject* parent = nullptr);
    ~VideoRAGStore() override;

    /// 建表（rag_chunks / rag_entities），幂等
    bool initialize();

    // ---- 索引管理 ----

    /// 视频索引加载：从 DB 读入某 videoId 的所有 chunks 到内存
    void loadVideo(const QString& videoId);

    /// 清空 / 失效某视频的所有索引（视频文件变化 or 用户手动清除）
    void invalidateVideo(const QString& videoId);

    /// 清理超过 N 天未访问的视频索引
    void cleanupStale(int maxAgeDays = 30);

    // ---- Chunk 写入 ----

    /// 插入单条 chunk（写内存 + DB）
    bool insertChunk(Collection col, const VideoChunk& chunk);

    /// 批量插入
    bool insertChunks(Collection col, const std::vector<VideoChunk>& chunks);

    /// 删除某 chunk
    bool removeChunk(Collection col, const QString& chunkId);

    // ---- 检索 ----

    /**
     * 相似度检索。返回 topK 个（VideoChunk, cosine相似度）对。
     * 未加载 videoId 或该集合无数据时返回空。
     */
    QVector<QPair<VideoChunk, float>> search(Collection col,
                                              const std::vector<float>& queryVector,
                                              const Filter& filter,
                                              int topK);

    /// 精确匹配某 chunk（不走向量搜索）
    VideoChunk getChunk(Collection col, const QString& chunkId) const;

    /// 获取某视频/某集合的全部 chunk（用于导出/统计）
    QVector<VideoChunk> listChunks(Collection col, const QString& videoId) const;

    // ---- 实体档案 ----

    bool upsertEntity(const EntityProfile& entity);
    QVector<EntityProfile> listEntities(const QString& videoId) const;

    // ---- 工具 ----

    /// 余弦相似度（假设两向量都已 L2 归一化，则等价于点积）
    static float cosineSimilarity(const std::vector<float>& a,
                                   const std::vector<float>& b);

signals:
    void videoIndexLoaded(const QString& videoId, int chunkCount);
    void videoIndexInvalidated(const QString& videoId);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

#endif // FRAMEMIND_VIDEO_RAG_STORE_H
