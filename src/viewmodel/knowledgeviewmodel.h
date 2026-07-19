#ifndef FRAMEMIND_KNOWLEDGEVIEWMODEL_H
#define FRAMEMIND_KNOWLEDGEVIEWMODEL_H

#include <QObject>
#include <QString>
#include <QVector>
#include <QDateTime>

#include "model/retrieval_result.h"
#include "service/rag/video_rag_store.h"

class VideoRAGStore;
class VideoRAGRetriever;
class EmbeddingService;
class DatabaseManager;

/**
 * 知识库页面 ViewModel。
 *
 * 职责：
 *   - 聚合 VideoRAGStore 中已持久化的视频索引概要
 *   - 提供按视频查看 chunk 列表的能力
 *   - 支持在知识库内做检索测试（三路召回可视化）
 *   - 管理索引（删除、清理过期）
 */
class KnowledgeViewModel : public QObject {
    Q_OBJECT

public:
    /// 已索引视频的摘要信息
    struct VideoIndexSummary {
        QString   videoId;
        QString   filePath;
        QString   fileName;
        int64_t   durationMs   = 0;
        int       level        = -1;   // -1=未知 0=L0 1=L1 2=L2
        int       totalChunks  = 0;
        int       visualCount  = 0;
        int       textCount    = 0;
        int       qaCacheCount = 0;
        QDateTime lastIndexed;
    };

    explicit KnowledgeViewModel(VideoRAGStore*     ragStore,
                                VideoRAGRetriever* retriever,
                                DatabaseManager*   db,
                                QObject*           parent = nullptr);

    void setEmbeddingService(EmbeddingService* e) { m_embedder = e; }

    // ---- 数据访问 ----
    QVector<VideoIndexSummary> indexedVideos() const { return m_videos; }
    QVector<VideoChunk>        currentChunks() const { return m_chunks; }
    QVector<RetrievalResult>   searchResults() const { return m_searchResults; }

    QString selectedVideoId() const { return m_selectedVideoId; }
    bool    isSearching()    const { return m_searching; }

public slots:
    /// 刷新已索引视频列表
    void loadIndexedVideos();

    /// 选中某个视频，加载其 chunk 列表
    void selectVideo(const QString& videoId);

    /// 按集合过滤 chunk（-1 = 全部）
    void filterChunks(int collection);

    /// 删除某个视频的全部索引
    void removeVideoIndex(const QString& videoId);

    /// 清理 N 天前未访问的过期索引
    void cleanupStale(int maxAgeDays = 30);

    /// 检索测试：在当前选中视频内执行 RAG 检索
    void testSearch(const QString& query);

    /// 清空检索结果
    void clearSearchResults();

signals:
    /// 视频列表已刷新
    void indexedVideosChanged(const QVector<VideoIndexSummary>& videos);

    /// 当前 chunk 列表已更新
    void chunksChanged(const QVector<VideoChunk>& chunks);

    /// 检索结果就绪
    void searchResultsReady(const QVector<RetrievalResult>& results);

    /// 某视频索引已删除
    void videoIndexRemoved(const QString& videoId);

    /// 过期索引清理完成，影响条数
    void staleIndexCleaned(int removedCount);

    /// 状态消息（操作完成 / 出错）
    void statusMessage(const QString& msg, bool isError = false);

    void searchingChanged(bool searching);

private:
    VideoIndexSummary buildSummary(const QString& videoId) const;

    VideoRAGStore*     m_ragStore  = nullptr;
    VideoRAGRetriever* m_retriever = nullptr;
    DatabaseManager*   m_db        = nullptr;
    EmbeddingService*  m_embedder  = nullptr;

    QString                    m_selectedVideoId;
    int                        m_activeCollection = -1;   // -1 = 全部
    QVector<VideoIndexSummary> m_videos;
    QVector<VideoChunk>        m_chunks;
    QVector<VideoChunk>        m_allChunks;   // 当前视频全部 chunk（过滤前）
    QVector<RetrievalResult>   m_searchResults;
    bool                       m_searching = false;
};

#endif // FRAMEMIND_KNOWLEDGEVIEWMODEL_H
